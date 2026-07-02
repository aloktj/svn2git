/*
 *  svn2git enhanced — SVNValidator integration tests (Catch2)
 *
 *  Creates a real SVN repository (svnadmin + svn) via the fixture
 *  script, then verifies the pre-migration analysis against the known
 *  shape of that repository: 6 revisions, 2 authors, 1 branch + trunk,
 *  1 tag.
 */

#include "svn2git/error_reporter.h"
#include "svn2git/svn_validator.h"

#include "integration_helpers.h"

#include <catch2/catch_test_macros.hpp>

using svn2git::ErrorReporter;
using svn2git::SVNReport;
using svn2git::SVNValidator;
using testhelpers::TempDir;

TEST_CASE("pre-migration report captures the fixture repository shape",
          "[integration][svn-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin");

    TempDir dir;
    const std::string url = testhelpers::createTestSvnRepo(dir);

    ErrorReporter reporter;
    SVNValidator validator(url, reporter);
    const SVNReport report = validator.generatePreMigrationReport();

    CHECK(report.ok);
    CHECK(report.totalRevisions == 6);
    CHECK(report.authorCount == 2); // jsmith, mmustermann

    // Branch inventory: trunk + release-1.0.
    REQUIRE(report.branches.size() == 2);
    CHECK(report.branches[0] == "release-1.0");
    CHECK(report.branches[1] == "trunk");

    REQUIRE(report.tags.size() == 1);
    CHECK(report.tags[0] == "v1.0.0");

    // The textual report must render without throwing and mention key data.
    const std::string text = report.toText();
    CHECK(text.find("Total revisions : 6") != std::string::npos);
    CHECK(text.find("release-1.0") != std::string::npos);
}

TEST_CASE("per-branch commit counts reflect the fixture history",
          "[integration][svn-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin");

    TempDir dir;
    const std::string url = testhelpers::createTestSvnRepo(dir);

    ErrorReporter reporter;
    SVNValidator validator(url, reporter);

    const std::map<std::string, long> byBranch = validator.countCommitsByBranch();
    REQUIRE(byBranch.count("trunk") == 1);
    REQUIRE(byBranch.count("release-1.0") == 1);
    // trunk touched in r1 (via layout), r2, r3; copies also count on the
    // branch history — assert plausible lower bounds rather than exact
    // svn log semantics.
    CHECK(byBranch.at("trunk") >= 3);
    CHECK(byBranch.at("release-1.0") >= 2); // r4 copy + r5 fix

    const std::map<std::string, long> byTag = validator.countCommitsByTag();
    REQUIRE(byTag.count("v1.0.0") == 1);
    CHECK(byTag.at("v1.0.0") >= 1);
}

TEST_CASE("repository size and largest files are computed",
          "[integration][svn-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin");

    TempDir dir;
    const std::string url = testhelpers::createTestSvnRepo(dir);

    ErrorReporter reporter;
    SVNValidator validator(url, reporter);

    const double sizeMB = validator.calculateRepositorySize();
    CHECK(sizeMB >= 0.0); // tiny repo, but never negative on success
    CHECK(sizeMB < 1.0); // sanity upper bound for the fixture

    const SVNReport report = validator.generatePreMigrationReport();
    REQUIRE_FALSE(report.largestFiles.empty());
    // All fixture files live under trunk/branches/tags — this guards the
    // path-column parsing (a truncated path would lose the prefix).
    for (const svn2git::SvnFileInfo& info : report.largestFiles) {
        const bool underLayout = info.path.rfind("trunk/", 0) == 0
            || info.path.rfind("branches/", 0) == 0 || info.path.rfind("tags/", 0) == 0;
        INFO("path: " << info.path);
        CHECK(underLayout);
        CHECK(info.sizeBytes > 0);
    }
}

TEST_CASE("unreachable repository produces a clean failure",
          "[integration][svn-validator]")
{
    REQUIRE_TOOLS("svn");

    ErrorReporter reporter;
    SVNValidator validator("file:///nonexistent/svn-repo", reporter);
    const SVNReport report = validator.generatePreMigrationReport();

    CHECK_FALSE(report.ok);
    CHECK(reporter.hasErrors());
}
