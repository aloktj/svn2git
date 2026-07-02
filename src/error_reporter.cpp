/*
 *  svn2git enhanced — error reporting implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/error_reporter.h"

#include "svn2git/logging.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>

namespace svn2git {

std::string currentIso8601Utc()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
    gmtime_r(&seconds, &utc);
    char buffer[sizeof "2025-07-02T10:30:00Z"];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return std::string(buffer);
}

ErrorReporter::ErrorReporter() = default;

void ErrorReporter::report(ErrorCode code, const std::string& message,
                           const std::string& context)
{
    ErrorEntry entry {code, message, context, currentIso8601Utc()};
    m_entries.push_back(entry);

    auto log = logging::get("error-reporter");
    if (context.empty())
        log->error("[{}] {}", codeName(code), message);
    else
        log->error("[{}] {} (context: {})", codeName(code), message, context);
}

std::string ErrorReporter::codeName(ErrorCode code)
{
    switch (code) {
    case ErrorCode::InvalidRulesSyntax:
        return "E001_INVALID_RULES_SYNTAX";
    case ErrorCode::MissingAuthors:
        return "E002_MISSING_AUTHORS";
    case ErrorCode::StructuralRevisionError:
        return "E003_STRUCTURAL_REVISION_ERROR";
    case ErrorCode::CopySourceNotFound:
        return "E004_COPY_SOURCE_NOT_FOUND";
    case ErrorCode::CommitCountMismatch:
        return "E005_COMMIT_COUNT_MISMATCH";
    case ErrorCode::AuthorNotFound:
        return "E006_AUTHOR_NOT_FOUND";
    case ErrorCode::GitLabConnectionError:
        return "E007_GITLAB_CONNECTION_ERROR";
    case ErrorCode::InvalidEmail:
        return "E008_INVALID_EMAIL";
    case ErrorCode::DuplicateAuthorEntry:
        return "E009_DUPLICATE_AUTHOR_ENTRY";
    case ErrorCode::InvalidRegexPattern:
        return "E010_INVALID_REGEX_PATTERN";
    case ErrorCode::FileAccessError:
        return "E011_FILE_ACCESS_ERROR";
    case ErrorCode::ExternalToolError:
        return "E012_EXTERNAL_TOOL_ERROR";
    }
    return "E999_UNKNOWN"; // defensive: unreachable with a valid enum
}

std::string ErrorReporter::suggestionFor(ErrorCode code)
{
    switch (code) {
    case ErrorCode::InvalidRulesSyntax:
        return "Check the .rules file against samples/*.rules: every 'match' and "
               "'create repository' block must be closed with 'end match' / "
               "'end repository', and match patterns must end in a slash.";
    case ErrorCode::MissingAuthors:
        return "Run 'svn2git --validate-authors-only <svn-url>' to list unmapped "
               "authors, then add them to authors.txt, or use --auto-map-authors "
               "to generate placeholder entries to review.";
    case ErrorCode::StructuralRevisionError:
        return "Inspect the offending revision with 'svn log -v -r <rev>' and add "
               "an explicit match rule (or min/max revision bounds) covering the "
               "paths touched by that revision.";
    case ErrorCode::CopySourceNotFound:
        return "The copy source path is outside all match rules. Add a rule for "
               "the source path or a 'match … end match' ignore rule if the "
               "history before the copy is intentionally dropped.";
    case ErrorCode::CommitCountMismatch:
        return "Compare the pre-migration report (SVNValidator) with the "
               "post-migration report (GitValidator). Check for revisions that "
               "matched no rule and for merged/ignored branches.";
    case ErrorCode::AuthorNotFound:
        return "Add the SVN username to authors.txt in the form "
               "'svnuser = Full Name <email@example.com>' and re-run the migration.";
    case ErrorCode::GitLabConnectionError:
        return "Verify the GitLab URL, network reachability and that the access "
               "token has 'write_repository' scope. Retry with --debug for the "
               "full transport log.";
    case ErrorCode::InvalidEmail:
        return "Fix the address in authors.txt: it must contain exactly one '@' "
               "with a non-empty local part and a domain containing a dot "
               "(e.g. john.smith@example.com).";
    case ErrorCode::DuplicateAuthorEntry:
        return "Remove or merge the duplicated SVN username in authors.txt — the "
               "first entry wins and later entries are ignored, which is almost "
               "never what you want in an auditable migration.";
    case ErrorCode::InvalidRegexPattern:
        return "Test the pattern with 'svn2git --debug-rules'. Remember patterns "
               "are ECMAScript regular expressions; escape literal dots and "
               "ensure capture groups referenced as \\1 exist.";
    case ErrorCode::FileAccessError:
        return "Check that the path exists, is readable/writable by the current "
               "user, and that the enclosing directory exists for output files.";
    case ErrorCode::ExternalToolError:
        return "Ensure the required tool (svn, git, gpg) is installed and on "
               "PATH; re-run with --debug to capture the full command output.";
    }
    return "No suggestion available."; // defensive: unreachable with a valid enum
}

std::string ErrorReporter::format(const ErrorEntry& entry)
{
    std::ostringstream out;
    out << "ERROR " << codeName(entry.code) << " [" << entry.timestamp << "]\n"
        << "  Message   : " << entry.message << '\n';
    if (!entry.context.empty())
        out << "  Context   : " << entry.context << '\n';
    out << "  Suggestion: " << suggestionFor(entry.code) << '\n';
    return out.str();
}

bool ErrorReporter::generateReport(const std::string& filename) const
{
    auto log = logging::get("error-reporter");

    std::ofstream file(filename, std::ios::trunc);
    if (!file.is_open()) {
        log->error("cannot open error report file '{}' for writing", filename);
        return false;
    }

    file << "SVN2GIT MIGRATION ERROR REPORT\n"
         << "Generated: " << currentIso8601Utc() << '\n'
         << "Total errors: " << m_entries.size() << "\n\n";

    for (const ErrorEntry& entry : m_entries)
        file << format(entry) << '\n';

    if (m_entries.empty())
        file << "No errors were recorded during this run.\n";

    file.flush();
    if (!file.good()) {
        log->error("write failure while generating '{}'", filename);
        return false;
    }

    log->info("error report with {} entrie(s) written to '{}'", m_entries.size(),
              filename);
    return true;
}

void ErrorReporter::clear()
{
    logging::get("error-reporter")
        ->debug("clearing {} recorded error(s)", m_entries.size());
    m_entries.clear();
}

} // namespace svn2git
