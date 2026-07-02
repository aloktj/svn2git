/*
 *  svn2git enhanced — migration audit trail (EN 50128)
 *
 *  Produces the human-readable audit.log that documents who migrated
 *  what, when, on which machine, through which milestones, and with
 *  what outcome. The trail is append-only in memory and rendered as a
 *  complete document by generateAuditTrail(); an optional detached GPG
 *  signature provides tamper evidence for the migration dossier.
 *
 *  Related classes: RevisionMapper supplies mapping statistics;
 *  ErrorReporter's count is recorded in the outcome section.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_AUDIT_LOGGER_H
#define SVN2GIT_AUDIT_LOGGER_H

#include "svn2git/command_runner.h"

#include <string>
#include <vector>

namespace svn2git {

/// One recorded audit event.
struct AuditEvent {
    std::string timestamp; ///< ISO8601 UTC
    std::string type; ///< e.g. "Validation", "Conversion", "Milestone"
    std::string details; ///< free-text description
    std::string status; ///< e.g. "OK", "FAILED", "" for informational
};

/// Records migration events and renders the audit trail document.
class AuditLogger {
public:
    /// Begins a new audit session. The migration ID is derived from the
    /// start time and repository name (deterministic given those inputs).
    /// Operator defaults to $USER and machine to the hostname when the
    /// corresponding arguments are empty.
    /// @param repositoryName  logical repository being migrated
    /// @param operatorId      responsible person (email or username)
    /// @param machine         host executing the migration
    /// @param runner          command executor (for signAuditLog; tests
    ///                        inject a fake)
    explicit AuditLogger(std::string repositoryName,
                         std::string operatorId = std::string(),
                         std::string machine = std::string(),
                         Runner runner = &CommandRunner::run);

    /// Record a generic event.
    /// @param type     category ("Validation", "Conversion", …)
    /// @param details  what happened
    /// @param status   outcome ("OK", "FAILED", "" when not applicable)
    void logEvent(const std::string& type, const std::string& details,
                  const std::string& status);

    /// Record a milestone with its commit count and duration.
    void logMilestone(const std::string& milestone, int commitCount, long durationMs);

    /// Record the final outcome; rendered as the "Completed:" line.
    /// @param success  overall migration result
    /// @param summary  optional extra detail (error count, hint)
    void setOutcome(bool success, const std::string& summary = std::string());

    /// Render the complete audit trail to @p filename.
    /// @return false (and logs) when the file cannot be written
    bool generateAuditTrail(const std::string& filename) const;

    /// Create a detached ASCII-armored GPG signature (<file>.asc) for a
    /// previously generated audit trail. Requires generateAuditTrail()
    /// to have been called first with @p auditFilePath.
    /// @param gpgKeyId       signing key (fingerprint or email)
    /// @param auditFilePath  the audit file to sign
    /// @return false (and logs) when gpg is unavailable or signing fails
    bool signAuditLog(const std::string& gpgKeyId,
                      const std::string& auditFilePath) const;

    /// Accessors used by tests and the CLI summary.
    const std::string& migrationId() const { return m_migrationId; }
    const std::string& operatorId() const { return m_operatorId; }
    const std::string& machine() const { return m_machine; }
    const std::vector<AuditEvent>& events() const { return m_events; }

private:
    std::string m_repositoryName;
    std::string m_operatorId;
    std::string m_machine;
    std::string m_migrationId;
    std::string m_startedAt; ///< ISO8601 UTC session start
    Runner m_runner;
    std::vector<AuditEvent> m_events;
    bool m_outcomeSet = false;
    bool m_success = false;
    std::string m_outcomeSummary;
};

} // namespace svn2git

#endif // SVN2GIT_AUDIT_LOGGER_H
