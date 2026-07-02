/*
 *  svn2git enhanced — GitValidator integration tests (Catch2)
 *
 *  Builds real Git repositories and verifies post-migration reporting,
 *  expectation matching, and integrity checking.
 */

#include "svn2git/error_reporter.h"
#include "svn2git/git_validator.h"
#include "svn2git/svn_validator.h"

#include "integration_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using svn2git::ErrorReporter;
using svn2git::GitReport;
using svn2git::GitValidationStatus;
using svn2git::GitValidator;
using svn2git::SVNReport;
using testhelpers::TempDir;

TEST_CASE("post-migration report captures commits, branches, tags and authors",
          "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    TempDir dir;
    const std::string repo = dir.path + "/git-repo";
    testhelpers::createTestGitRepo(repo, 6);

    ErrorReporter reporter;
    GitValidator validator(repo, reporter);
    const GitReport report = validator.generatePostMigrationReport();

    CHECK(report.ok);
    CHECK(report.totalCommits == 6);
    CHECK(report.authorCount == 1);
    REQUIRE(report.branches.size() == 2);
    CHECK(std::find(report.branches.begin(), report.branches.end(), "main")
          != report.branches.end());
    CHECK(std::find(report.branches.begin(), report.branches.end(), "release-1.0")
          != report.branches.end());
    REQUIRE(report.tags.size() == 1);
    CHECK(report.tags.front() == "v1.0.0");

    CHECK(report.toText().find("Total commits   : 6") != std::string::npos);
}

TEST_CASE("matching expectations pass validation", "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    TempDir dir;
    const std::string repo = dir.path + "/git-repo";
    testhelpers::createTestGitRepo(repo, 6);

    SVNReport expected;
    expected.ok = true;
    expected.totalRevisions = 6;
    expected.authorCount = 1;
    expected.branches = {"release-1.0", "trunk"}; // trunk maps to main
    expected.tags = {"v1.0.0"};

    ErrorReporter reporter;
    GitValidator validator(repo, reporter);
    const GitValidationStatus status = validator.validateAgainstExpected(expected);

    CHECK(status.passed);
    CHECK(status.discrepancies.empty());
}

TEST_CASE("missing branch and tag are reported as discrepancies",
          "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    TempDir dir;
    const std::string repo = dir.path + "/git-repo";
    testhelpers::createTestGitRepo(repo, 6);

    SVNReport expected;
    expected.ok = true;
    expected.totalRevisions = 6;
    expected.authorCount = 1;
    expected.branches = {"trunk", "release-1.0", "feature-x"}; // feature-x absent
    expected.tags = {"v1.0.0", "v2.0.0"}; // v2.0.0 absent

    ErrorReporter reporter;
    GitValidator validator(repo, reporter);
    const GitValidationStatus status = validator.validateAgainstExpected(expected);

    CHECK_FALSE(status.passed);
    REQUIRE(status.discrepancies.size() == 2);
    CHECK(status.discrepancies[0].find("feature-x") != std::string::npos);
    CHECK(status.discrepancies[1].find("v2.0.0") != std::string::npos);
    CHECK(reporter.hasErrors());
}

TEST_CASE("more Git commits than SVN revisions is tolerated",
          "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    TempDir dir;
    const std::string repo = dir.path + "/git-repo";
    testhelpers::createTestGitRepo(repo, 6);

    // One SVN revision touching several branches legitimately produces
    // one Git commit per branch, so a higher Git commit count must NOT
    // fail validation (it is logged, not reported as a discrepancy).
    SVNReport expected;
    expected.ok = true;
    expected.totalRevisions = 3; // fewer than the 6 commits in the repo
    expected.authorCount = 1;

    ErrorReporter reporter;
    GitValidator validator(repo, reporter);
    const GitValidationStatus status = validator.validateAgainstExpected(expected);

    CHECK(status.passed);
    CHECK(status.discrepancies.empty());
}

TEST_CASE("empty converted repository is a discrepancy", "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    TempDir dir;
    const std::string repo = dir.path + "/git-repo";
    // Repository with refs but no commits: init only.
    svn2git::CommandRunner::run("mkdir -p " + svn2git::CommandRunner::shellQuote(repo));
    testhelpers::gitIn(repo, "init --quiet --initial-branch=main");

    SVNReport expected;
    expected.ok = true;
    expected.totalRevisions = 6;
    expected.authorCount = 1;

    ErrorReporter reporter;
    GitValidator validator(repo, reporter);
    const GitValidationStatus status = validator.validateAgainstExpected(expected);

    CHECK_FALSE(status.passed);
    REQUIRE_FALSE(status.discrepancies.empty());
    CHECK(status.discrepancies.front().find("no commits") != std::string::npos);
}

TEST_CASE("healthy repository passes the integrity check", "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    TempDir dir;
    const std::string repo = dir.path + "/git-repo";
    testhelpers::createTestGitRepo(repo, 3);

    ErrorReporter reporter;
    GitValidator validator(repo, reporter);
    CHECK(validator.validateRepositoryIntegrity());
    CHECK_FALSE(reporter.hasErrors());
}

TEST_CASE("nonexistent repository fails all validations cleanly",
          "[integration][git-validator]")
{
    REQUIRE_TOOLS("git");

    ErrorReporter reporter;
    GitValidator validator("/nonexistent/git-repo", reporter);

    CHECK_FALSE(validator.generatePostMigrationReport().ok);
    CHECK_FALSE(validator.validateRepositoryIntegrity());
    CHECK(reporter.hasErrors());
}
