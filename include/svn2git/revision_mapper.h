/*
 *  svn2git enhanced — SVN↔Git revision traceability (EN 50128)
 *
 *  Records every SVN-revision → Git-commit correspondence produced by a
 *  migration and exports it as both a JSON document
 *  (svn_to_git_mapping.json) and a queryable SQLite database
 *  (traceability.db). Bidirectional lookup and 1:1 validation guarantee
 *  no commit is lost or duplicated — the core traceability evidence
 *  required for safety-case audits.
 *
 *  Related classes: fed by the conversion loop; AuditLogger records the
 *  final mapping statistics; GitValidator cross-checks the counts.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_REVISION_MAPPER_H
#define SVN2GIT_REVISION_MAPPER_H

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace svn2git {

/// One SVN-revision → Git-commit correspondence.
struct RevisionMapping {
    long svnRevision = 0; ///< SVN revision number
    std::string gitCommitSha; ///< full 40-hex-digit Git SHA-1
    std::string author; ///< resolved Git author display name
    std::string timestamp; ///< ISO8601 UTC commit timestamp
    std::string message; ///< commit message (snippet stored on export)
};

/// Records, validates and exports SVN↔Git revision mappings.
class RevisionMapper {
public:
    /// @param repositoryName  logical name recorded in every export
    explicit RevisionMapper(std::string repositoryName);

    /// Record one mapping. Re-recording an SVN revision or Git SHA is
    /// remembered as a duplicate and fails validateOneToOneMapping();
    /// the first recording wins (deterministic behavior).
    void recordMapping(long svnRevision, const std::string& gitCommitSha,
                       const std::string& author, const std::string& timestamp,
                       const std::string& message);

    /// Write all mappings to @p filename as JSON (schema documented in
    /// the implementation). @return false (and logs) on I/O failure.
    bool generateMappingFile(const std::string& filename) const;

    /// Create/overwrite an SQLite database at @p dbPath with a
    /// `mappings` table (indexed by revision and by SHA) for ad-hoc
    /// traceability queries. @return false (and logs) on any SQLite error.
    bool generateMappingDatabase(const std::string& dbPath) const;

    /// Verify strict 1:1 correspondence: no SVN revision recorded twice,
    /// no Git SHA recorded twice, and no gaps in the recorded revision
    /// range that were not explicitly skipped.
    /// @return true when the mapping is bijective over its domain
    bool validateOneToOneMapping() const;

    /// Full Git SHA for @p svnRevision, or "" when unmapped.
    std::string findGitCommitBySvnRevision(long svnRevision) const;

    /// SVN revision for @p gitSha (full or unambiguous prefix ≥ 7 chars),
    /// or -1 when unmapped.
    long findSvnRevisionByGitCommit(const std::string& gitSha) const;

    /// Mark @p svnRevision as intentionally unconverted (e.g. a revision
    /// touching only ignored paths) so gap validation stays meaningful.
    void recordSkippedRevision(long svnRevision, const std::string& reason);

    /// Revisions inside [minRecorded, maxRecorded] that are neither
    /// mapped nor explicitly skipped.
    std::vector<long> missingRevisions() const;

    /// Number of recorded mappings.
    std::size_t size() const { return m_byRevision.size(); }

    /// Repository name used on export.
    const std::string& repositoryName() const { return m_repositoryName; }

private:
    std::string m_repositoryName;
    std::map<long, RevisionMapping> m_byRevision; ///< ordered for export
    std::unordered_map<std::string, long> m_byGitSha; ///< reverse index
    std::map<long, std::string> m_skipped; ///< revision → reason
    std::vector<long> m_duplicateRevisions; ///< re-recorded revisions
    std::vector<std::string> m_duplicateShas; ///< re-recorded SHAs
};

} // namespace svn2git

#endif // SVN2GIT_REVISION_MAPPER_H
