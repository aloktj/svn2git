/*
 *  svn2git enhanced — content validation integration tests (Catch2)
 *
 *  Exercises ContentValidator against a real SVN repository whose
 *  branch and tag are created as cheap copies (the fixture's r4/r6),
 *  compared with real Git repositories: a faithful conversion must
 *  pass, and a conversion that lost a file from the cheap-copied
 *  branch must fail naming exactly that file.
 */

#include "svn2git/content_validator.h"

#include "integration_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using svn2git::ContentReport;
using svn2git::ContentValidator;
using svn2git::ErrorCode;
using svn2git::ErrorReporter;
using svn2git::RefMapping;
using testhelpers::createTestSvnRepo;
using testhelpers::gitIn;
using testhelpers::TempDir;

namespace {

/// Git repository mirroring the fixture SVN repository at HEAD:
///   master       README.md + src/main.c (return 42)   ← /trunk
///   release-1.0  README.md + src/main.c (return 1)    ← /branches/release-1.0
///   v1.0.0 tag   at the release-1.0 head              ← /tags/v1.0.0
void createFaithfulMirror(const std::string& repoPath)
{
    svn2git::CommandRunner::run("mkdir -p "
                                + svn2git::CommandRunner::shellQuote(repoPath));
    gitIn(repoPath, "init --quiet --initial-branch=master");
    gitIn(repoPath, "config user.name 'John Smith'");
    gitIn(repoPath, "config user.email john.smith@example.com");

    svn2git::CommandRunner::run("mkdir -p "
                                + svn2git::CommandRunner::shellQuote(repoPath + "/src"));
    svn2git::CommandRunner::run(
        "printf '# Test Project\\n' > "
        + svn2git::CommandRunner::shellQuote(repoPath + "/README.md"));
    svn2git::CommandRunner::run(
        "printf 'int main(void) { return 42; }\\n' > "
        + svn2git::CommandRunner::shellQuote(repoPath + "/src/main.c"));
    gitIn(repoPath, "add .");
    gitIn(repoPath, "commit --quiet -m 'trunk state'");

    gitIn(repoPath, "checkout --quiet -b release-1.0");
    svn2git::CommandRunner::run(
        "printf 'int main(void) { return 1; }\\n' > "
        + svn2git::CommandRunner::shellQuote(repoPath + "/src/main.c"));
    gitIn(repoPath, "commit --quiet -am 'hotfix on release branch'");
    gitIn(repoPath, "tag v1.0.0");
    gitIn(repoPath, "checkout --quiet master");
}

const std::vector<RefMapping> kFixtureMappings = {
    RefMapping {"trunk", "master"},
    RefMapping {"branches/release-1.0", "release-1.0"},
    RefMapping {"tags/v1.0.0", "tags/v1.0.0"},
};

} // namespace

TEST_CASE("a faithful conversion passes content validation",
          "[integration][content-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin", "git");

    TempDir dir;
    const std::string svnUrl = createTestSvnRepo(dir);
    const std::string gitRepo = dir.path + "/git-faithful";
    createFaithfulMirror(gitRepo);

    ErrorReporter reporter;
    ContentValidator validator(svnUrl, gitRepo, reporter);
    const ContentReport report = validator.verify(kFixtureMappings);

    INFO(report.toText());
    CHECK(report.ok);
    REQUIRE(report.refs.size() == 3);
    for (const auto& ref : report.refs) {
        CHECK(ref.ok());
        CHECK(ref.filesInSvn == 2); // README.md + src/main.c
        CHECK(ref.filesHashed == 2);
    }
    CHECK_FALSE(reporter.hasErrors());
}

TEST_CASE("a file lost from a cheap-copied branch fails content validation",
          "[integration][content-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin", "git");

    TempDir dir;
    const std::string svnUrl = createTestSvnRepo(dir);
    const std::string gitRepo = dir.path + "/git-corrupted";
    createFaithfulMirror(gitRepo);

    // Simulate cheap-copy data loss: the converter dropped README.md
    // from the branch (its content only existed via the copy source).
    gitIn(gitRepo, "checkout --quiet release-1.0");
    gitIn(gitRepo, "rm --quiet README.md");
    gitIn(gitRepo, "commit --quiet -m 'simulated conversion loss'");
    gitIn(gitRepo, "checkout --quiet master");

    ErrorReporter reporter;
    ContentValidator validator(svnUrl, gitRepo, reporter);
    const ContentReport report = validator.verify(kFixtureMappings);

    INFO(report.toText());
    CHECK_FALSE(report.ok);
    REQUIRE(report.refs.size() == 3);
    CHECK(report.refs[0].ok()); // trunk untouched
    REQUIRE(report.refs[1].missingInGit.size() == 1);
    CHECK(report.refs[1].missingInGit[0] == "README.md");
    CHECK(report.refs[2].ok()); // tag taken before the loss

    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries()[0].code == ErrorCode::FileMissingInGit);
}

TEST_CASE("corrupted file content fails content validation",
          "[integration][content-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin", "git");

    TempDir dir;
    const std::string svnUrl = createTestSvnRepo(dir);
    const std::string gitRepo = dir.path + "/git-mismatch";
    createFaithfulMirror(gitRepo);

    // Same inventory, different bytes: only the content hash catches this.
    svn2git::CommandRunner::run(
        "printf 'int main(void) { return 99; }\\n' > "
        + svn2git::CommandRunner::shellQuote(gitRepo + "/src/main.c"));
    gitIn(gitRepo, "commit --quiet -am 'simulated corruption'");

    ErrorReporter reporter;
    ContentValidator validator(svnUrl, gitRepo, reporter);
    const ContentReport report = validator.verify(kFixtureMappings);

    INFO(report.toText());
    CHECK_FALSE(report.ok);
    REQUIRE(report.refs[0].contentMismatches.size() == 1);
    CHECK(report.refs[0].contentMismatches[0] == "src/main.c");
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries()[0].code == ErrorCode::ContentMismatch);
}

TEST_CASE("a branch missing from the converted repository is detected",
          "[integration][content-validator]")
{
    REQUIRE_TOOLS("svn", "svnadmin", "git");

    TempDir dir;
    const std::string svnUrl = createTestSvnRepo(dir);
    const std::string gitRepo = dir.path + "/git-noref";
    createFaithfulMirror(gitRepo);
    gitIn(gitRepo, "branch -D release-1.0");

    ErrorReporter reporter;
    ContentValidator validator(svnUrl, gitRepo, reporter);
    const ContentReport report = validator.verify(kFixtureMappings);

    INFO(report.toText());
    CHECK_FALSE(report.ok);
    CHECK(report.refs[1].refMissing);
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries()[0].code == ErrorCode::FileMissingInGit);
}
