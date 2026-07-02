/*
 *  svn2git enhanced — end-to-end migration pipeline test (Catch2)
 *
 *  Exercises the full validation/compliance pipeline the way a real
 *  migration uses it:
 *
 *    1. create a real SVN repository (fixture script)
 *    2. pre-flight configuration validation (fixture authors + rules)
 *    3. pre-migration analysis (SVNValidator)
 *    4. conversion — simulated by building the equivalent Git repository
 *       (the Qt-based converter binary is exercised by the bats suite)
 *    5. traceability recording (RevisionMapper → JSON + SQLite)
 *    6. post-migration validation (GitValidator vs SVNReport)
 *    7. audit trail generation (AuditLogger)
 */

#include "svn2git/audit_logger.h"
#include "svn2git/author_validator.h"
#include "svn2git/config_validator.h"
#include "svn2git/error_reporter.h"
#include "svn2git/git_validator.h"
#include "svn2git/revision_mapper.h"
#include "svn2git/rules_validator.h"
#include "svn2git/svn_validator.h"

#include "integration_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using testhelpers::TempDir;

namespace {

std::string slurp(const std::string& path)
{
    std::ifstream file(path);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

} // namespace

TEST_CASE("full migration pipeline produces consistent compliance artifacts",
          "[integration][full-migration]")
{
    REQUIRE_TOOLS("svn", "svnadmin", "git");

    TempDir dir;
    svn2git::ErrorReporter reporter;
    svn2git::AuditLogger audit("testproject", "test-operator@example.com",
                               "test-machine");

    // ---- 1. real SVN repository --------------------------------------
    const std::string svnUrl = testhelpers::createTestSvnRepo(dir);
    audit.logEvent("Setup", "created fixture SVN repository", "OK");

    // ---- 2. pre-flight configuration ----------------------------------
    const std::string fixtures = SVN2GIT_FIXTURES_DIR;
    svn2git::ConfigValidator config(fixtures + "/authors.txt", fixtures + "/test.rules",
                                    "", reporter);
    const svn2git::ConfigReport configReport = config.validateAll();
    CHECK(configReport.ok);
    audit.logEvent("Validation", "pre-flight configuration",
                   configReport.ok ? "OK" : "FAILED");

    // Author coverage against the real SVN history.
    svn2git::AuthorValidator authors(svnUrl, fixtures + "/authors.txt", reporter);
    const svn2git::AuthorValidationReport coverage = authors.validateCoverage();
    CHECK(coverage.ok);
    CHECK(coverage.totalSvnAuthors == 2);
    audit.logEvent("Validation", "author coverage", coverage.ok ? "OK" : "FAILED");

    // Rules dry-run over the fixture layout.
    svn2git::RulesValidator rules(fixtures + "/test.rules", reporter);
    const svn2git::RulesValidationResult dryRun
        = rules.dryRun({"/trunk/", "/branches/release-1.0/", "/tags/v1.0.0/"});
    CHECK(dryRun.valid);
    REQUIRE(dryRun.dryRunLines.size() == 3);
    for (const std::string& line : dryRun.dryRunLines)
        CHECK(line.find("NO MATCH") == std::string::npos);

    // ---- 3. pre-migration analysis ------------------------------------
    svn2git::SVNValidator svnValidator(svnUrl, reporter);
    const svn2git::SVNReport svnReport = svnValidator.generatePreMigrationReport();
    REQUIRE(svnReport.ok);
    CHECK(svnReport.totalRevisions == 6);
    audit.logEvent("Validation", "pre-migration SVN analysis", "OK");

    // ---- 4. conversion (simulated) ------------------------------------
    const std::string gitRepo = dir.path + "/converted";
    testhelpers::createTestGitRepo(gitRepo, 6);
    audit.logMilestone("conversion", 6, 1);

    // ---- 5. traceability ----------------------------------------------
    const std::string shaList
        = testhelpers::gitIn(gitRepo, "log --reverse --format=%H main");
    std::vector<std::string> shas;
    {
        std::istringstream stream(shaList);
        std::string line;
        while (std::getline(stream, line))
            if (line.size() == 40)
                shas.push_back(line);
    }
    REQUIRE(shas.size() == 6);

    svn2git::RevisionMapper mapper("testproject");
    for (std::size_t i = 0; i < shas.size(); ++i)
        mapper.recordMapping(static_cast<long>(i + 1), shas[i], "John Smith",
                             "2024-01-15T10:30:00Z", "commit " + std::to_string(i + 1));
    CHECK(mapper.validateOneToOneMapping());

    const std::string jsonPath = dir.path + "/svn_to_git_mapping.json";
    const std::string dbPath = dir.path + "/traceability.db";
    REQUIRE(mapper.generateMappingFile(jsonPath));
    REQUIRE(mapper.generateMappingDatabase(dbPath));
    CHECK(slurp(jsonPath).find("\"total_mappings\": 6") != std::string::npos);
    audit.logEvent("Traceability", "mapping JSON + SQLite generated", "OK");

    // Bidirectional queries hold end to end.
    CHECK(mapper.findGitCommitBySvnRevision(3) == shas[2]);
    CHECK(mapper.findSvnRevisionByGitCommit(shas[5]) == 6);

    // ---- 6. post-migration validation ---------------------------------
    svn2git::GitValidator gitValidator(gitRepo, reporter);
    CHECK(gitValidator.validateRepositoryIntegrity());
    const svn2git::GitValidationStatus status
        = gitValidator.validateAgainstExpected(svnReport);
    // Fixture SVN counts authors=2 while the simulated conversion used a
    // single identity — that is within tolerance (fewer authors allowed);
    // branches/tags/commit-count all line up.
    CHECK(status.passed);
    audit.logEvent("Validation", "post-migration Git analysis",
                   status.passed ? "OK" : "FAILED");

    // ---- 7. audit trail ------------------------------------------------
    audit.setOutcome(status.passed && coverage.ok, "pipeline completed");
    const std::string auditPath = dir.path + "/audit.log";
    REQUIRE(audit.generateAuditTrail(auditPath));

    const std::string trail = slurp(auditPath);
    CHECK(trail.find("MIGRATION AUDIT TRAIL") != std::string::npos);
    CHECK(trail.find("Repository: testproject") != std::string::npos);
    CHECK(trail.find("Operator: test-operator@example.com") != std::string::npos);
    CHECK(trail.find("SUCCESS") != std::string::npos);

    // The whole pipeline ran without recording a single error.
    CHECK_FALSE(reporter.hasErrors());
}

TEST_CASE("pipeline surfaces missing-author failures before conversion",
          "[integration][full-migration]")
{
    REQUIRE_TOOLS("svn", "svnadmin");

    TempDir dir;
    const std::string svnUrl = testhelpers::createTestSvnRepo(dir);

    // authors file that omits mmustermann.
    const std::string authorsPath = dir.path + "/authors-incomplete.txt";
    {
        std::ofstream file(authorsPath);
        file << "jsmith = John Smith <john.smith@example.com>\n";
    }

    svn2git::ErrorReporter reporter;
    svn2git::AuthorValidator authors(svnUrl, authorsPath, reporter);
    const svn2git::AuthorValidationReport coverage = authors.validateCoverage();

    CHECK_FALSE(coverage.ok);
    REQUIRE(coverage.missingAuthors.size() == 1);
    CHECK(coverage.missingAuthors.front().svnUsername == "mmustermann");
    CHECK(reporter.hasErrors());

    // The error report names the failure with a remediation hint.
    const std::string reportPath = dir.path + "/error_report.txt";
    REQUIRE(reporter.generateReport(reportPath));
    const std::string report = slurp(reportPath);
    CHECK(report.find("E002_MISSING_AUTHORS") != std::string::npos);
    CHECK(report.find("Suggestion:") != std::string::npos);
}
