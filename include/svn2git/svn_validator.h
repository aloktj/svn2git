/*
 *  svn2git enhanced — pre-migration SVN repository analysis (EN 50128)
 *
 *  Captures the reference statistics of the source SVN repository
 *  before conversion: total revisions, authors, branches/tags with
 *  per-branch commit counts, repository size, and the largest files.
 *  GitValidator later compares the converted repository against this
 *  report to prove nothing was lost.
 *
 *  Assumes a standard layout (trunk/branches/tags) for branch/tag
 *  discovery; repositories with custom layouts still get the global
 *  statistics (revisions, authors, size).
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_SVN_VALIDATOR_H
#define SVN2GIT_SVN_VALIDATOR_H

#include "svn2git/command_runner.h"
#include "svn2git/error_reporter.h"

#include <map>
#include <string>
#include <vector>

namespace svn2git {

/// A file with its size, for largest-file reporting.
struct SvnFileInfo {
    std::string path;
    long long sizeBytes = 0;
};

/// Pre-migration statistics of an SVN repository.
struct SVNReport {
    bool ok = false; ///< analysis succeeded
    long totalRevisions = 0; ///< HEAD revision number
    long authorCount = 0; ///< distinct committing authors
    std::vector<std::string> branches; ///< names under /branches + trunk
    std::vector<std::string> tags; ///< names under /tags
    std::map<std::string, long> commitsByBranch; ///< branch → commit count
    std::map<std::string, long> commitsByTag; ///< tag → commit count
    double repositorySizeMB = 0.0; ///< sum of file sizes at HEAD
    std::vector<SvnFileInfo> largestFiles; ///< top files by size (≤ 10)

    /// Render as human-readable text for the migration dossier.
    std::string toText() const;
};

/// Analyzes an SVN repository before migration.
class SVNValidator {
public:
    /// @param svnRepoUrl  URL of the SVN repository
    /// @param reporter    shared error sink
    /// @param runner      command executor (tests inject a fake)
    SVNValidator(std::string svnRepoUrl, ErrorReporter& reporter,
                 Runner runner = &CommandRunner::run);

    /// Run the complete pre-migration analysis. Individual failures are
    /// reported through the ErrorReporter; report.ok reflects whether
    /// the mandatory statistics (revisions, authors) were obtained.
    SVNReport generatePreMigrationReport();

    /// Commit counts for trunk and every /branches/<name> directory.
    std::map<std::string, long> countCommitsByBranch();

    /// Commit counts for every /tags/<name> directory.
    std::map<std::string, long> countCommitsByTag();

    /// Sum of all file sizes at HEAD, in megabytes; also fills the
    /// largest-file list of the last generated report. Returns -1.0 on
    /// failure (reported through ErrorReporter).
    double calculateRepositorySize();

private:
    /// Names of direct children of @p directory ("branches" or "tags").
    std::vector<std::string> listChildren(const std::string& directory);

    /// Number of revisions touching @p path (via `svn log --quiet`).
    long countCommitsOnPath(const std::string& path);

    std::string m_svnRepoUrl;
    ErrorReporter& m_reporter;
    Runner m_runner;
    std::vector<SvnFileInfo> m_largestFiles; ///< filled by calculateRepositorySize
};

} // namespace svn2git

#endif // SVN2GIT_SVN_VALIDATOR_H
