/*
 *  svn2git enhanced — ContentValidator unit tests (Catch2)
 *
 *  svn/git access is faked through the injectable Runner, so these
 *  tests run without subversion or git installed. The fake dispatches
 *  on the command text and remembers the last `svn cat` target so
 *  hash-object results can differ per file.
 */

#include "svn2git/content_validator.h"

#include "unit_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <string>

using svn2git::CommandResult;
using svn2git::ContentReport;
using svn2git::ContentValidator;
using svn2git::ErrorCode;
using svn2git::ErrorReporter;
using svn2git::RefMapping;
using svn2git::RulesValidator;

namespace {

/// State shared by the fake runner across the calls of one test case.
struct FakeMigration {
    std::string svnListOutput; ///< canned `svn list -R`
    std::string gitLsTreeOutput; ///< canned `git ls-tree -r -z` (NUL-separated)
    int gitLsTreeExit = 0; ///< non-zero simulates a missing ref
    std::map<std::string, std::string> svnHashByFile; ///< file → fake blob hash
    std::string lastCatFile; ///< file of the most recent `svn cat`

    svn2git::Runner runner()
    {
        return [this](const std::string& command) -> CommandResult {
            if (command.rfind("svn list", 0) == 0)
                return CommandResult {0, svnListOutput};
            if (command.find("ls-tree") != std::string::npos)
                return CommandResult {gitLsTreeExit, gitLsTreeOutput};
            if (command.rfind("svn cat", 0) == 0) {
                // Extract the file name from ".../<svnPath>/<file>' > ..."
                const std::size_t quote = command.find("' >");
                const std::size_t start = command.rfind('/', quote);
                lastCatFile = command.substr(start + 1, quote - start - 1);
                return CommandResult {0, ""};
            }
            if (command.find("hash-object") != std::string::npos) {
                const auto it = svnHashByFile.find(lastCatFile);
                if (it == svnHashByFile.end())
                    return CommandResult {1, "unknown file"};
                return CommandResult {0, it->second + "\n"};
            }
            return CommandResult {127, "unexpected command: " + command};
        };
    }
};

/// NUL-separated ls-tree entry as git produces with -z.
std::string lsTreeEntry(const std::string& sha, const std::string& path)
{
    return "100644 blob " + sha + "\t" + path + std::string(1, '\0');
}

/// Temporary rules file, deleted on scope exit.
struct TempRulesFile {
    std::string path;
    explicit TempRulesFile(const std::string& content)
        : path(testhelpers::uniqueTempPath("svn2git-content-rules", ".rules"))
    {
        std::ofstream file(path, std::ios::trunc);
        file << content;
    }
    ~TempRulesFile() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("identical inventories and hashes pass", "[content-validator]")
{
    FakeMigration fake;
    fake.svnListOutput = "README.md\nsrc/\nsrc/main.c\n";
    fake.gitLsTreeOutput = lsTreeEntry("hash-readme", "README.md")
        + lsTreeEntry("hash-main", "src/main.c");
    fake.svnHashByFile = {{"README.md", "hash-readme"}, {"main.c", "hash-main"}};

    ErrorReporter reporter;
    ContentValidator validator("file:///fake", "/fake/git", reporter, fake.runner());
    const ContentReport report = validator.verify({RefMapping {"trunk", "master"}});

    REQUIRE(report.refs.size() == 1);
    CHECK(report.ok);
    CHECK(report.refs[0].ok());
    CHECK(report.refs[0].filesInSvn == 2);
    CHECK(report.refs[0].filesHashed == 2);
    CHECK_FALSE(reporter.hasErrors());
    CHECK(report.toText().find("PASS") != std::string::npos);
}

TEST_CASE("a file missing from the git ref fails validation", "[content-validator]")
{
    FakeMigration fake;
    fake.svnListOutput = "README.md\nsrc/\nsrc/main.c\n";
    // README.md was lost during conversion (the cheap-copy symptom).
    fake.gitLsTreeOutput = lsTreeEntry("hash-main", "src/main.c");
    fake.svnHashByFile = {{"main.c", "hash-main"}};

    ErrorReporter reporter;
    ContentValidator validator("file:///fake", "/fake/git", reporter, fake.runner());
    const ContentReport report
        = validator.verify({RefMapping {"branches/release-1.0", "release-1.0"}});

    CHECK_FALSE(report.ok);
    REQUIRE(report.refs.size() == 1);
    REQUIRE(report.refs[0].missingInGit.size() == 1);
    CHECK(report.refs[0].missingInGit[0] == "README.md");
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries()[0].code == ErrorCode::FileMissingInGit);
    CHECK(report.toText().find("README.md") != std::string::npos);
}

TEST_CASE("differing content hashes fail validation", "[content-validator]")
{
    FakeMigration fake;
    fake.svnListOutput = "README.md\nsrc/\nsrc/main.c\n";
    fake.gitLsTreeOutput = lsTreeEntry("hash-readme", "README.md")
        + lsTreeEntry("hash-WRONG", "src/main.c");
    fake.svnHashByFile = {{"README.md", "hash-readme"}, {"main.c", "hash-main"}};

    ErrorReporter reporter;
    ContentValidator validator("file:///fake", "/fake/git", reporter, fake.runner());
    const ContentReport report = validator.verify({RefMapping {"trunk", "master"}});

    CHECK_FALSE(report.ok);
    REQUIRE(report.refs.size() == 1);
    REQUIRE(report.refs[0].contentMismatches.size() == 1);
    CHECK(report.refs[0].contentMismatches[0] == "src/main.c");
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries()[0].code == ErrorCode::ContentMismatch);
}

TEST_CASE("a missing git ref is reported as unconverted", "[content-validator]")
{
    FakeMigration fake;
    fake.svnListOutput = "README.md\n";
    fake.gitLsTreeExit = 128; // fatal: not a valid object name

    ErrorReporter reporter;
    ContentValidator validator("file:///fake", "/fake/git", reporter, fake.runner());
    const ContentReport report = validator.verify({RefMapping {"branches/lost", "lost"}});

    CHECK_FALSE(report.ok);
    REQUIRE(report.refs.size() == 1);
    CHECK(report.refs[0].refMissing);
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries()[0].code == ErrorCode::FileMissingInGit);
    CHECK(report.toText().find("GIT REF MISSING") != std::string::npos);
}

TEST_CASE("sample limit caps the number of hashed files", "[content-validator]")
{
    FakeMigration fake;
    for (int i = 0; i < 10; ++i) {
        const std::string file = "file" + std::to_string(i) + ".txt";
        fake.svnListOutput += file + "\n";
        fake.gitLsTreeOutput += lsTreeEntry("hash-" + file, file);
        fake.svnHashByFile[file] = "hash-" + file;
    }

    ErrorReporter reporter;
    ContentValidator validator("file:///fake", "/fake/git", reporter, fake.runner());
    const ContentReport report
        = validator.verify({RefMapping {"trunk", "master"}}, /*sampleLimit=*/3);

    REQUIRE(report.refs.size() == 1);
    CHECK(report.ok);
    CHECK(report.refs[0].filesInSvn == 10);
    CHECK(report.refs[0].filesHashed == 3);
}

TEST_CASE("standard layout mappings follow the converter convention",
          "[content-validator]")
{
    const std::vector<RefMapping> mappings
        = ContentValidator::standardLayoutMappings({"trunk", "release-1.0"}, {"v1.0.0"});

    REQUIRE(mappings.size() == 3);
    CHECK(mappings[0].svnPath == "trunk");
    CHECK(mappings[0].gitRef == "master");
    CHECK(mappings[1].svnPath == "branches/release-1.0");
    CHECK(mappings[1].gitRef == "release-1.0");
    CHECK(mappings[2].svnPath == "tags/v1.0.0");
    CHECK(mappings[2].gitRef == "tags/v1.0.0");
}

TEST_CASE("rules-based mapping expands backreferences and detects gaps",
          "[content-validator]")
{
    TempRulesFile rules("create repository myrepo\n"
                        "end repository\n"
                        "match /trunk/\n"
                        "  repository myrepo\n"
                        "  branch master\n"
                        "end match\n"
                        "match /branches/([^/]+)/\n"
                        "  repository myrepo\n"
                        "  branch \\1\n"
                        "end match\n"
                        "match /tags/old-.+/\n"
                        "end match\n");

    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);
    REQUIRE(validator.validate().valid);

    std::vector<std::string> unmapped;
    std::vector<std::string> ignored;
    const std::vector<RefMapping> mappings = ContentValidator::mapWithRules(
        validator, {"trunk", "release-1.0"}, {"old-0.9", "v1.0.0"}, unmapped, ignored);

    REQUIRE(mappings.size() == 2);
    CHECK(mappings[0].svnPath == "trunk");
    CHECK(mappings[0].gitRef == "master");
    CHECK(mappings[1].svnPath == "branches/release-1.0");
    CHECK(mappings[1].gitRef == "release-1.0"); // \1 expanded

    REQUIRE(ignored.size() == 1);
    CHECK(ignored[0] == "tags/old-0.9"); // explicit ignore rule

    REQUIRE(unmapped.size() == 1);
    CHECK(unmapped[0] == "tags/v1.0.0"); // no rule — would be dropped
}

TEST_CASE("a rule without an explicit branch maps to master", "[content-validator]")
{
    TempRulesFile rules("create repository myrepo\n"
                        "end repository\n"
                        "match /trunk/\n"
                        "  repository myrepo\n"
                        "end match\n");

    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);
    REQUIRE(validator.validate().valid);

    std::string repository;
    std::string branch;
    REQUIRE(validator.resolveTarget("/trunk/", repository, branch)
            == RulesValidator::Resolution::Mapped);
    CHECK(repository == "myrepo");
    CHECK(branch == "master");
}
