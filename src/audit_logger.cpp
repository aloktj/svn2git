/*
 *  svn2git enhanced — migration audit trail implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/audit_logger.h"

#include "svn2git/error_reporter.h" // currentIso8601Utc
#include "svn2git/logging.h"

#include <unistd.h>

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

namespace svn2git {

namespace {

/// Hostname of the executing machine, "unknown-host" when unavailable.
std::string detectHostname()
{
    char buffer[256] = {0};
    if (gethostname(buffer, sizeof(buffer) - 1) == 0 && buffer[0] != '\0')
        return std::string(buffer);
    return "unknown-host";
}

/// $USER / $LOGNAME, "unknown-operator" when unavailable.
std::string detectOperator()
{
    for (const char* variable : {"USER", "LOGNAME"}) {
        const char* value = std::getenv(variable);
        if (value != nullptr && value[0] != '\0')
            return std::string(value);
    }
    return "unknown-operator";
}

/// Deterministic migration ID: mig-<YYYYMMDDHHMMSS>-<repo-hash6>.
/// FNV-1a over the repository name gives a stable, non-cryptographic
/// suffix that distinguishes repositories without introducing randomness
/// (reproducibility requirement). It carries no integrity guarantees —
/// tamper evidence comes from signAuditLog(), not from this ID.
std::string makeMigrationId(const std::string& startedAtIso,
                            const std::string& repositoryName)
{
    std::string compactTime;
    for (const char c : startedAtIso) {
        if (std::isdigit(static_cast<unsigned char>(c)) != 0)
            compactTime += c;
    }

    unsigned long hash = 2166136261UL; // FNV-1a 32-bit offset basis
    for (const unsigned char c : repositoryName) {
        hash ^= c;
        hash *= 16777619UL;
        hash &= 0xFFFFFFFFUL;
    }

    std::ostringstream id;
    id << "mig-" << compactTime << '-' << std::hex << std::setw(6) << std::setfill('0')
       << (hash & 0xFFFFFFUL);
    return id.str();
}

} // namespace

AuditLogger::AuditLogger(std::string repositoryName, std::string operatorId,
                         std::string machine, Runner runner)
    : m_repositoryName(std::move(repositoryName))
    , m_operatorId(operatorId.empty() ? detectOperator() : std::move(operatorId))
    , m_machine(machine.empty() ? detectHostname() : std::move(machine))
    , m_startedAt(currentIso8601Utc())
    , m_runner(std::move(runner))
{
    m_migrationId = makeMigrationId(m_startedAt, m_repositoryName);
    logging::get("audit-logger")
        ->info("audit session {} started for '{}' by {} on {}", m_migrationId,
               m_repositoryName, m_operatorId, m_machine);
}

void AuditLogger::logEvent(const std::string& type, const std::string& details,
                           const std::string& status)
{
    AuditEvent event {currentIso8601Utc(), type, details, status};
    m_events.push_back(event);

    auto log = logging::get("audit-logger");
    if (status.empty())
        log->info("[{}] {}: {}", m_migrationId, type, details);
    else
        log->info("[{}] {}: {}... {}", m_migrationId, type, details, status);
}

void AuditLogger::logMilestone(const std::string& milestone, int commitCount,
                               long durationMs)
{
    std::ostringstream details;
    details << milestone << " (" << commitCount << " commit(s) in " << durationMs
            << " ms)";
    logEvent("Milestone", details.str(), "OK");
}

void AuditLogger::setOutcome(bool success, const std::string& summary)
{
    m_outcomeSet = true;
    m_success = success;
    m_outcomeSummary = summary;
    logEvent("Completed",
             summary.empty() ? (success ? "migration succeeded" : "migration failed")
                             : summary,
             success ? "SUCCESS" : "FAILURE");
}

bool AuditLogger::generateAuditTrail(const std::string& filename) const
{
    auto log = logging::get("audit-logger");
    log->info("writing audit trail with {} event(s) to '{}'", m_events.size(), filename);

    std::ofstream file(filename, std::ios::trunc);
    if (!file.is_open()) {
        log->error("cannot open audit trail file '{}' for writing", filename);
        return false;
    }

    file << "MIGRATION AUDIT TRAIL\n"
         << "Repository: " << m_repositoryName << '\n'
         << "Migration ID: " << m_migrationId << '\n'
         << "Started: " << m_startedAt << '\n'
         << "Operator: " << m_operatorId << '\n'
         << "Machine: " << m_machine << "\n\n";

    for (const AuditEvent& event : m_events) {
        // Render the time-of-day portion of the ISO timestamp in the
        // bracketed prefix; the full date is in the header.
        std::string timeOfDay = event.timestamp;
        const std::size_t tPos = timeOfDay.find('T');
        if (tPos != std::string::npos)
            timeOfDay = timeOfDay.substr(tPos + 1, 8);

        file << '[' << timeOfDay << "] " << event.type << ": " << event.details;
        if (!event.status.empty())
            file << "... " << event.status;
        file << '\n';
    }

    if (!m_outcomeSet)
        file << "\nWARNING: no final outcome was recorded for this session\n";

    file.flush();
    if (!file.good()) {
        log->error("write failure while generating '{}'", filename);
        return false;
    }

    log->info("audit trail '{}' written successfully", filename);
    return true;
}

bool AuditLogger::signAuditLog(const std::string& gpgKeyId,
                               const std::string& auditFilePath) const
{
    auto log = logging::get("audit-logger");

    if (gpgKeyId.empty()) {
        log->error("cannot sign audit log: empty GPG key id");
        return false;
    }
    if (!std::ifstream(auditFilePath).good()) {
        log->error("cannot sign audit log: '{}' does not exist — call "
                   "generateAuditTrail() first",
                   auditFilePath);
        return false;
    }

    const std::string command = "gpg --batch --yes --armor --detach-sign --local-user "
        + CommandRunner::shellQuote(gpgKeyId) + " "
        + CommandRunner::shellQuote(auditFilePath);
    const CommandResult result = m_runner(command);

    if (!result.ok()) {
        log->error("gpg signing failed (exit {}): {}", result.exitCode,
                   result.output.substr(0, 300));
        return false;
    }

    log->info("audit trail '{}' signed with key '{}' ({}.asc created)", auditFilePath,
              gpgKeyId, auditFilePath);
    return true;
}

} // namespace svn2git
