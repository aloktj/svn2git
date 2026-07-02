/*
 *  svn2git enhanced — revision traceability implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/revision_mapper.h"

#include "svn2git/error_reporter.h"
#include "svn2git/logging.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <utility>

namespace svn2git {

namespace {

/// True when @p sha looks like a (possibly abbreviated) hex SHA-1.
bool isHexSha(const std::string& sha)
{
    return !sha.empty() && sha.size() <= 40
        && std::all_of(sha.begin(), sha.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

/// Truncate a commit message to a single-line snippet for export.
std::string messageSnippet(const std::string& message, std::size_t maxLength = 80)
{
    std::string snippet = message.substr(0, message.find('\n'));
    if (snippet.size() > maxLength) {
        snippet.resize(maxLength - 3);
        snippet += "...";
    }
    return snippet;
}

/// RAII owner for a sqlite3 connection.
struct SqliteCloser {
    void operator()(sqlite3* db) const noexcept
    {
        if (db != nullptr)
            sqlite3_close(db);
    }
};
using SqliteHandle = std::unique_ptr<sqlite3, SqliteCloser>;

/// RAII owner for a prepared statement.
struct StmtFinalizer {
    void operator()(sqlite3_stmt* stmt) const noexcept
    {
        if (stmt != nullptr)
            sqlite3_finalize(stmt);
    }
};
using StmtHandle = std::unique_ptr<sqlite3_stmt, StmtFinalizer>;

} // namespace

RevisionMapper::RevisionMapper(std::string repositoryName)
    : m_repositoryName(std::move(repositoryName))
{
    logging::get("revision-mapper")
        ->debug("revision mapper created for repository '{}'", m_repositoryName);
}

void RevisionMapper::recordMapping(long svnRevision, const std::string& gitCommitSha,
                                   const std::string& author,
                                   const std::string& timestamp,
                                   const std::string& message)
{
    auto log = logging::get("revision-mapper");

    if (svnRevision <= 0 || !isHexSha(gitCommitSha) || gitCommitSha.size() != 40) {
        // Malformed input is remembered as a duplicate-style defect so
        // validateOneToOneMapping() fails loudly instead of silently
        // accepting bad traceability data.
        log->error("rejecting malformed mapping: r{} -> '{}'", svnRevision, gitCommitSha);
        m_duplicateRevisions.push_back(svnRevision);
        return;
    }

    if (m_byRevision.count(svnRevision) != 0) {
        log->error("duplicate mapping for SVN revision r{} (kept first)", svnRevision);
        m_duplicateRevisions.push_back(svnRevision);
        return;
    }
    if (m_byGitSha.count(gitCommitSha) != 0) {
        log->error("duplicate mapping for Git commit {} (kept first)",
                   gitCommitSha.substr(0, 7));
        m_duplicateShas.push_back(gitCommitSha);
        return;
    }

    RevisionMapping mapping;
    mapping.svnRevision = svnRevision;
    mapping.gitCommitSha = gitCommitSha;
    mapping.author = author;
    mapping.timestamp = timestamp;
    mapping.message = message;

    m_byGitSha.emplace(gitCommitSha, svnRevision);
    m_byRevision.emplace(svnRevision, std::move(mapping));
    log->debug("recorded r{} -> {}", svnRevision, gitCommitSha.substr(0, 7));
}

void RevisionMapper::recordSkippedRevision(long svnRevision, const std::string& reason)
{
    m_skipped[svnRevision] = reason;
    logging::get("revision-mapper")
        ->debug("recorded skipped revision r{}: {}", svnRevision, reason);
}

bool RevisionMapper::generateMappingFile(const std::string& filename) const
{
    auto log = logging::get("revision-mapper");
    log->info("writing {} mapping(s) to JSON file '{}'", m_byRevision.size(), filename);

    nlohmann::json root;
    root["repository"] = m_repositoryName;
    root["created_at"] = currentIso8601Utc();
    root["total_mappings"] = m_byRevision.size();

    nlohmann::json mappings = nlohmann::json::array();
    for (const auto& [revision, mapping] : m_byRevision) {
        mappings.push_back({
            {"svn_revision", revision},
            {"git_commit_sha", mapping.gitCommitSha},
            {"git_short_sha", mapping.gitCommitSha.substr(0, 7)},
            {"author", mapping.author},
            {"timestamp", mapping.timestamp},
            {"message_snippet", messageSnippet(mapping.message)},
        });
    }
    root["mappings"] = std::move(mappings);

    if (!m_skipped.empty()) {
        nlohmann::json skipped = nlohmann::json::array();
        for (const auto& [revision, reason] : m_skipped)
            skipped.push_back({{"svn_revision", revision}, {"reason", reason}});
        root["skipped_revisions"] = std::move(skipped);
    }

    std::ofstream file(filename, std::ios::trunc);
    if (!file.is_open()) {
        log->error("cannot open '{}' for writing", filename);
        return false;
    }
    file << root.dump(2) << '\n';
    file.flush();
    if (!file.good()) {
        log->error("write failure while generating '{}'", filename);
        return false;
    }

    log->info("JSON mapping file '{}' written successfully", filename);
    return true;
}

bool RevisionMapper::generateMappingDatabase(const std::string& dbPath) const
{
    auto log = logging::get("revision-mapper");
    log->info("writing {} mapping(s) to SQLite database '{}'", m_byRevision.size(),
              dbPath);

    // Recreate from scratch so repeated runs stay deterministic.
    std::remove(dbPath.c_str());

    sqlite3* raw = nullptr;
    if (sqlite3_open(dbPath.c_str(), &raw) != SQLITE_OK) {
        const std::string error
            = (raw != nullptr) ? sqlite3_errmsg(raw) : "out of memory";
        log->error("cannot create database '{}': {}", dbPath, error);
        sqlite3_close(raw);
        return false;
    }
    SqliteHandle db(raw);

    const auto exec = [&db, &log](const char* sql) {
        char* errorMessage = nullptr;
        if (sqlite3_exec(db.get(), sql, nullptr, nullptr, &errorMessage) != SQLITE_OK) {
            log->error("SQLite error: {} (statement: {})",
                       errorMessage != nullptr ? errorMessage : "unknown", sql);
            sqlite3_free(errorMessage);
            return false;
        }
        return true;
    };

    if (!exec("CREATE TABLE metadata ("
              "  key TEXT PRIMARY KEY,"
              "  value TEXT NOT NULL)")
        || !exec("CREATE TABLE mappings ("
                 "  svn_revision INTEGER PRIMARY KEY,"
                 "  git_commit_sha TEXT NOT NULL UNIQUE,"
                 "  author TEXT NOT NULL,"
                 "  timestamp TEXT NOT NULL,"
                 "  message TEXT NOT NULL)")
        || !exec("CREATE INDEX idx_mappings_sha ON mappings(git_commit_sha)")
        || !exec("BEGIN TRANSACTION"))
        return false;

    // Metadata rows document provenance for auditors.
    {
        StmtHandle stmt;
        sqlite3_stmt* rawStmt = nullptr;
        if (sqlite3_prepare_v2(db.get(),
                               "INSERT INTO metadata(key, value) VALUES(?1, ?2)", -1,
                               &rawStmt, nullptr)
            != SQLITE_OK) {
            log->error("cannot prepare metadata insert: {}", sqlite3_errmsg(db.get()));
            return false;
        }
        stmt.reset(rawStmt);

        const std::pair<const char*, std::string> rows[] = {
            {"repository", m_repositoryName},
            {"created_at", currentIso8601Utc()},
            {"total_mappings", std::to_string(m_byRevision.size())},
        };
        for (const auto& [key, value] : rows) {
            sqlite3_reset(stmt.get());
            sqlite3_bind_text(stmt.get(), 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
                log->error("metadata insert failed: {}", sqlite3_errmsg(db.get()));
                return false;
            }
        }
    }

    {
        StmtHandle stmt;
        sqlite3_stmt* rawStmt = nullptr;
        if (sqlite3_prepare_v2(db.get(),
                               "INSERT INTO mappings(svn_revision, git_commit_sha,"
                               " author, timestamp, message)"
                               " VALUES(?1, ?2, ?3, ?4, ?5)",
                               -1, &rawStmt, nullptr)
            != SQLITE_OK) {
            log->error("cannot prepare mapping insert: {}", sqlite3_errmsg(db.get()));
            return false;
        }
        stmt.reset(rawStmt);

        for (const auto& [revision, mapping] : m_byRevision) {
            sqlite3_reset(stmt.get());
            sqlite3_bind_int64(stmt.get(), 1, revision);
            sqlite3_bind_text(stmt.get(), 2, mapping.gitCommitSha.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 3, mapping.author.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 4, mapping.timestamp.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt.get(), 5, mapping.message.c_str(), -1,
                              SQLITE_TRANSIENT);
            if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
                log->error("mapping insert failed for r{}: {}", revision,
                           sqlite3_errmsg(db.get()));
                return false;
            }
        }
    }

    if (!exec("COMMIT"))
        return false;

    log->info("SQLite traceability database '{}' written successfully", dbPath);
    return true;
}

bool RevisionMapper::validateOneToOneMapping() const
{
    auto log = logging::get("revision-mapper");

    bool valid = true;
    if (!m_duplicateRevisions.empty()) {
        log->error("1:1 validation: {} duplicate/malformed SVN revision(s) recorded",
                   m_duplicateRevisions.size());
        valid = false;
    }
    if (!m_duplicateShas.empty()) {
        log->error("1:1 validation: {} duplicate Git SHA(s) recorded",
                   m_duplicateShas.size());
        valid = false;
    }

    const std::vector<long> missing = missingRevisions();
    if (!missing.empty()) {
        log->error("1:1 validation: {} revision(s) in range neither mapped nor "
                   "explicitly skipped (first: r{})",
                   missing.size(), missing.front());
        valid = false;
    }

    // Internal consistency: forward and reverse indexes must agree.
    if (m_byRevision.size() != m_byGitSha.size()) {
        log->error("1:1 validation: index inconsistency ({} revisions vs {} SHAs)",
                   m_byRevision.size(), m_byGitSha.size());
        valid = false;
    }

    if (m_byRevision.empty()) {
        log->error("1:1 validation: no mappings recorded");
        valid = false;
    }

    log->info("1:1 mapping validation over {} mapping(s): {}", m_byRevision.size(),
              valid ? "PASS" : "FAIL");
    return valid;
}

std::string RevisionMapper::findGitCommitBySvnRevision(long svnRevision) const
{
    const auto found = m_byRevision.find(svnRevision);
    return (found != m_byRevision.end()) ? found->second.gitCommitSha : std::string();
}

long RevisionMapper::findSvnRevisionByGitCommit(const std::string& gitSha) const
{
    if (!isHexSha(gitSha) || gitSha.size() < 7)
        return -1; // reject ambiguous short prefixes outright

    const auto exact = m_byGitSha.find(gitSha);
    if (exact != m_byGitSha.end())
        return exact->second;

    // Prefix lookup — must be unambiguous.
    long match = -1;
    for (const auto& [sha, revision] : m_byGitSha) {
        if (sha.compare(0, gitSha.size(), gitSha) == 0) {
            if (match != -1)
                return -1; // ambiguous prefix
            match = revision;
        }
    }
    return match;
}

std::vector<long> RevisionMapper::missingRevisions() const
{
    std::vector<long> missing;
    if (m_byRevision.empty())
        return missing;

    const long first = m_byRevision.begin()->first;
    const long last = m_byRevision.rbegin()->first;
    for (long revision = first; revision <= last; ++revision) {
        if (m_byRevision.count(revision) == 0 && m_skipped.count(revision) == 0)
            missing.push_back(revision);
    }
    return missing;
}

} // namespace svn2git
