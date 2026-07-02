/*
 *  svn2git enhanced — post-migration Git repository validation (EN 50128)
 *
 *  Inspects a converted Git repository and compares it against the
 *  pre-migration SVNReport to prove the conversion preserved history:
 *  commit counts, branch/tag inventory, author counts, and repository
 *  integrity (git fsck, orphaned commits).
 *
 *  Related classes: consumes SVNValidator's SVNReport; discrepancies are
 *  reported through ErrorReporter (CommitCountMismatch etc.).
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_GIT_VALIDATOR_H
#define SVN2GIT_GIT_VALIDATOR_H

#include "svn2git/command_runner.h"
#include "svn2git/error_reporter.h"
#include "svn2git/svn_validator.h"

#include <string>
#include <vector>

namespace svn2git {

/// Post-migration statistics of a converted Git repository.
struct GitReport {
    bool ok = false; ///< analysis succeeded
    long totalCommits = 0; ///< commits reachable from all refs
    long authorCount = 0; ///< distinct commit authors
    std::vector<std::string> branches; ///< local branch names
    std::vector<std::string> tags; ///< tag names

    /// Render as human-readable text for the migration dossier.
    std::string toText() const;
};

/// Outcome of comparing a converted repository against expectations.
struct GitValidationStatus {
    bool passed = false; ///< no blocking discrepancy
    std::vector<std::string> discrepancies; ///< human-readable findings
};

/// Validates a converted Git repository after migration.
class GitValidator {
public:
    /// @param gitRepoPath  filesystem path of the converted repository
    /// @param reporter     shared error sink
    /// @param runner       command executor (tests inject a fake)
    GitValidator(std::string gitRepoPath, ErrorReporter& reporter,
                 Runner runner = &CommandRunner::run);

    /// Collect the post-migration statistics.
    GitReport generatePostMigrationReport();

    /// Compare the converted repository against the pre-migration
    /// @p expected report. Branch/tag inventory and commit counts must
    /// be plausible; blocking mismatches are reported through the
    /// ErrorReporter as CommitCountMismatch.
    GitValidationStatus validateAgainstExpected(const SVNReport& expected);

    /// Run `git fsck --full` and check for dangling/orphaned commits.
    /// @return true when the object database is sound
    bool validateRepositoryIntegrity();

private:
    /// Run a git command inside the repository (git -C <path> …).
    CommandResult git(const std::string& arguments) const;

    std::string m_gitRepoPath;
    ErrorReporter& m_reporter;
    Runner m_runner;
};

} // namespace svn2git

#endif // SVN2GIT_GIT_VALIDATOR_H
