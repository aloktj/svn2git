/*
 *  svn2git enhanced — SVN author extraction and mapping validation
 *
 *  Extracts the set of committing authors from an SVN repository,
 *  validates that authors.txt covers every one of them with a
 *  well-formed identity, and can generate placeholder mappings for
 *  unmapped authors (--auto-map-authors).
 *
 *  Related classes: reports failures through ErrorReporter; invoked by
 *  ConfigValidator during pre-flight and by the CLI for
 *  --validate-authors-only.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_AUTHOR_VALIDATOR_H
#define SVN2GIT_AUTHOR_VALIDATOR_H

#include "svn2git/command_runner.h"
#include "svn2git/error_reporter.h"

#include <map>
#include <string>
#include <vector>

namespace svn2git {

/// One SVN username together with its Git identity mapping.
struct AuthorMapping {
    std::string svnUsername; ///< login recorded in SVN history
    std::string fullName; ///< mapped Git author name ("" when unmapped)
    std::string email; ///< mapped Git author email ("" when unmapped)
    long commitCount = 0; ///< commits by this author in the SVN history
};

/// Outcome of validating authors.txt coverage against SVN history.
struct AuthorValidationReport {
    bool ok = false; ///< true when fully covered & well-formed
    long totalSvnAuthors = 0; ///< distinct authors found in SVN
    long mappedAuthors = 0; ///< authors present in authors.txt
    std::vector<AuthorMapping> missingAuthors; ///< in SVN but not in authors.txt
    std::vector<std::string> invalidEmails; ///< authors.txt entries with bad email
    std::vector<std::string> duplicateEntries; ///< usernames mapped more than once
    std::vector<std::string> messages; ///< human-readable findings
};

/// Validates SVN-author → Git-identity mapping before a migration.
class AuthorValidator {
public:
    /// @param svnRepoUrl       URL of the SVN repository (file://, http(s)://, svn://)
    /// @param authorsFilePath  path to the authors.txt mapping file
    /// @param reporter         shared error sink for structured errors
    /// @param runner           command executor (tests inject a fake;
    ///                         defaults to CommandRunner::run)
    AuthorValidator(std::string svnRepoUrl, std::string authorsFilePath,
                    ErrorReporter& reporter, Runner runner = &CommandRunner::run);

    /// Query SVN history and return every distinct author with commit
    /// counts, sorted by username. Mapped identity fields are filled in
    /// from authors.txt when available.
    ///
    /// On failure (svn unreachable, no authors found) an error is
    /// reported through the ErrorReporter and an empty vector returned.
    std::vector<AuthorMapping> extractSVNAuthors();

    /// Full coverage validation: every SVN author mapped, every mapping
    /// well-formed (valid email, no duplicates).
    AuthorValidationReport validateCoverage();

    /// Generate authors.txt content covering every SVN author. Existing
    /// mappings are preserved verbatim; unmapped authors receive
    /// placeholder identities ("username = username <username@localhost>")
    /// that a human must review before the real migration.
    /// @return the generated file content ("" on extraction failure)
    std::string autoMapAuthors();

    /// RFC 5322-inspired pragmatic email check: exactly one '@',
    /// non-empty local part, domain with at least one dot, no whitespace
    /// or control characters.
    static bool isValidEmail(const std::string& email);

    /// Heuristic authors.txt line for an unmapped SVN username, e.g.
    /// "jsmith" → "jsmith = jsmith <jsmith@localhost>". Dotted usernames
    /// are title-cased: "john.smith" → "John Smith <john.smith@localhost>".
    static std::string suggestMapping(const std::string& svnUsername);

    /// Parse authors.txt into username → (name, email). Malformed lines
    /// and duplicates are reported through the ErrorReporter.
    /// Exposed for reuse by ConfigValidator.
    std::map<std::string, std::pair<std::string, std::string>> loadAuthorsFile();

private:
    std::string m_svnRepoUrl;
    std::string m_authorsFilePath;
    ErrorReporter& m_reporter;
    Runner m_runner;
};

} // namespace svn2git

#endif // SVN2GIT_AUTHOR_VALIDATOR_H
