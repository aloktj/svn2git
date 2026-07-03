/*
 *  svn2git enhanced — post-migration file content validation (EN 50128)
 *
 *  Proves the converted Git repository carries the same file contents as
 *  the source SVN repository, branch by branch and tag by tag. This is
 *  the check that catches cheap-copy data loss: a branch created in SVN
 *  as a cheap copy + diffs can arrive in Git missing files while every
 *  structural check (fsck, commit counts, ref inventory) still passes.
 *
 *  Two comparisons per mapped ref:
 *    1. complete file inventory  — svn ls -R  vs  git ls-tree -r
 *    2. exact content equality   — the git blob hash of `svn cat` output
 *       must equal the blob ID recorded in the Git tree (byte-identical
 *       content by construction of git's content addressing)
 *
 *  Related classes: reports through ErrorReporter (ContentMismatch,
 *  FileMissingInGit); ref mappings come from RulesValidator::resolveTarget
 *  or the standard trunk/branches/tags layout convention.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_CONTENT_VALIDATOR_H
#define SVN2GIT_CONTENT_VALIDATOR_H

#include "svn2git/command_runner.h"
#include "svn2git/error_reporter.h"
#include "svn2git/rules_validator.h"

#include <string>
#include <vector>

namespace svn2git {

/// One SVN location (branch or tag) mapped to a Git ref.
struct RefMapping {
    std::string svnPath; ///< e.g. "trunk", "branches/release-1.0"
    std::string gitRef; ///< e.g. "master", "release-1.0", "tags/v1.0.0"
    std::string repository {}; ///< target repository from the rules ("" when
                               ///< derived from the standard layout)
    std::string pathPrefix {}; ///< prefix the converter prepends inside the
                               ///< branch (from the rule's `prefix`), "" = none
};

/// Per-ref outcome of the content comparison.
struct RefContentResult {
    std::string svnPath; ///< SVN side of the mapping
    std::string gitRef; ///< Git side of the mapping
    bool refMissing = false; ///< the Git ref does not exist at all
    long filesInSvn = 0; ///< files listed at the SVN HEAD of this path
    long filesHashed = 0; ///< files whose content hash was compared
    std::vector<std::string> missingInGit; ///< in SVN, absent from the ref
    std::vector<std::string> contentMismatches; ///< blob hash differs
    std::vector<std::string> toolFailures; ///< svn/git invocation failed

    /// True when the ref exists and no file is missing or different.
    bool ok() const
    {
        return !refMissing && missingInGit.empty() && contentMismatches.empty()
            && toolFailures.empty();
    }
};

/// Aggregated outcome across every mapped ref.
struct ContentReport {
    bool ok = false; ///< every ref passed
    std::vector<RefContentResult> refs; ///< per-ref details

    /// Render as human-readable text for the migration dossier.
    std::string toText() const;
};

/// Verifies file contents of a converted Git repository against SVN.
class ContentValidator {
public:
    /// @param svnRepoUrl   URL of the source SVN repository
    /// @param gitRepoPath  filesystem path of the converted repository
    /// @param reporter     shared error sink
    /// @param runner       command executor (tests inject a fake)
    ContentValidator(std::string svnRepoUrl, std::string gitRepoPath,
                     ErrorReporter& reporter, Runner runner = &CommandRunner::run);

    /// Removes the scratch file created for `svn cat` transfers.
    ~ContentValidator();

    ContentValidator(const ContentValidator&) = delete;
    ContentValidator& operator=(const ContentValidator&) = delete;

    /// Compare every mapping. The file inventory comparison is always
    /// complete; @p sampleLimit caps the number of per-ref content-hash
    /// checks (0 = hash every file), spread evenly across the inventory
    /// so large repositories can be spot-checked in bounded time.
    ContentReport verify(const std::vector<RefMapping>& mappings, long sampleLimit = 0);

    /// Mappings for the standard layout convention used by the converter
    /// when no rules file is given: trunk→master, branches/X→X,
    /// tags/X→tags/X. @p branches uses SVNValidator naming, i.e. it may
    /// contain "trunk" alongside names found under /branches.
    static std::vector<RefMapping>
    standardLayoutMappings(const std::vector<std::string>& branches,
                           const std::vector<std::string>& tags);

    /// Mappings derived from a parsed rules file (first match wins,
    /// backreferences expanded — see RulesValidator::resolveTarget).
    /// Each mapping carries the rule's target repository and path
    /// prefix; a multi-repository rules file therefore yields mappings
    /// for several repositories, which callers must filter to the one
    /// being validated. Branch/tag directories matching no rule land in
    /// @p unmapped — their entire history would be dropped by the
    /// converter; paths matching an explicit ignore rule land in
    /// @p ignored.
    static std::vector<RefMapping> mapWithRules(const RulesValidator& rules,
                                                const std::vector<std::string>& branches,
                                                const std::vector<std::string>& tags,
                                                std::vector<std::string>& unmapped,
                                                std::vector<std::string>& ignored);

private:
    /// Files (not directories) at the HEAD of @p svnPath, sorted.
    /// @return false when the svn invocation failed
    bool listSvnFiles(const std::string& svnPath, std::vector<std::string>& files);

    /// path → blob SHA for every file reachable from @p gitRef.
    /// @return false when the ref cannot be resolved
    bool listGitBlobs(const std::string& gitRef,
                      std::vector<std::pair<std::string, std::string>>& blobs);

    /// Git blob hash of the SVN file content (via a scratch file so a
    /// failed `svn cat` is detected instead of hashing partial output).
    /// @return empty string on failure
    std::string svnContentHash(const std::string& svnPath, const std::string& file);

    std::string m_svnRepoUrl;
    std::string m_gitRepoPath;
    std::string m_scratchFile; ///< mkstemp-created private temp file for
                               ///< svn cat output ("" when creation failed)
    ErrorReporter& m_reporter;
    Runner m_runner;
};

} // namespace svn2git

#endif // SVN2GIT_CONTENT_VALIDATOR_H
