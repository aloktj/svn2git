/*
 *  svn2git enhanced — error reporting with codes and remediation hints
 *
 *  Collects structured errors during a migration run, maps every error
 *  code to an actionable remediation suggestion, and renders both console
 *  output and a persistent error_report.txt for the migration dossier.
 *
 *  Related classes: all validators report through ErrorReporter;
 *  AuditLogger records the final error count in the audit trail.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_ERROR_REPORTER_H
#define SVN2GIT_ERROR_REPORTER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace svn2git {

/// Machine-readable error classification for migration failures.
enum class ErrorCode : std::uint8_t {
    InvalidRulesSyntax, ///< .rules file could not be parsed
    MissingAuthors, ///< SVN authors absent from authors.txt
    StructuralRevisionError, ///< SVN revision with inconsistent structure
    CopySourceNotFound, ///< SVN copy-from path missing in history
    CommitCountMismatch, ///< post-migration commit count differs
    AuthorNotFound, ///< commit author unresolvable during import
    GitLabConnectionError, ///< push/API connection to GitLab failed
    InvalidEmail, ///< malformed email address in authors.txt
    DuplicateAuthorEntry, ///< same SVN username mapped twice
    InvalidRegexPattern, ///< match pattern is not a valid regex
    FileAccessError, ///< configuration/output file unreadable or unwritable
    ExternalToolError, ///< svn/git/gpg invocation failed
    ContentMismatch, ///< file content differs between SVN and converted Git
    FileMissingInGit, ///< file or ref present in SVN but absent from Git
    UnmappedSvnPath ///< SVN branch/tag path matched by no rule
};

/// One recorded error occurrence.
struct ErrorEntry {
    ErrorCode code; ///< classification
    std::string message; ///< specific description of this occurrence
    std::string context; ///< where it happened (file:line, path, rev…)
    std::string timestamp; ///< ISO8601 UTC time of recording
};

/// Accumulates errors, offers formatting, and writes error_report.txt.
///
/// All mutation is logged via spdlog so the console trail and the
/// persistent report can never diverge silently.
class ErrorReporter {
public:
    ErrorReporter();

    /// Record an error occurrence.
    /// @param code     classification
    /// @param message  human-readable description of this instance
    /// @param context  optional locus (file, revision, path…)
    void report(ErrorCode code, const std::string& message,
                const std::string& context = std::string());

    /// Stable machine-readable identifier, e.g. "E003_STRUCTURAL_REVISION_ERROR".
    static std::string codeName(ErrorCode code);

    /// Actionable remediation hint for @p code (never empty).
    static std::string suggestionFor(ErrorCode code);

    /// Render one entry as a multi-line, human-friendly block:
    /// code, message, context and suggestion.
    static std::string format(const ErrorEntry& entry);

    /// Write all collected errors to @p filename (default error_report.txt).
    /// @return false (and logs) when the file cannot be written.
    bool generateReport(const std::string& filename = "error_report.txt") const;

    /// True when at least one error has been recorded.
    bool hasErrors() const { return !m_entries.empty(); }

    /// Number of recorded errors.
    std::size_t errorCount() const { return m_entries.size(); }

    /// Read-only access to every recorded entry (in recording order).
    const std::vector<ErrorEntry>& entries() const { return m_entries; }

    /// Discard all recorded errors (used between validation stages).
    void clear();

private:
    std::vector<ErrorEntry> m_entries;
};

/// Current UTC time formatted as ISO8601 with 'Z' suffix
/// (e.g. "2025-07-02T10:30:00Z"). Shared by ErrorReporter, AuditLogger
/// and RevisionMapper so every artifact uses identical timestamps.
std::string currentIso8601Utc();

} // namespace svn2git

#endif // SVN2GIT_ERROR_REPORTER_H
