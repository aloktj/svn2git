/*
 *  svn2git enhanced — shared helpers for integration tests
 *
 *  Integration tests exercise real svn/svnadmin/git binaries. When a
 *  required tool is absent the test is skipped (not failed) so the unit
 *  suite stays useful on minimal machines; CI installs all tools and
 *  therefore always runs everything.
 */

#ifndef SVN2GIT_TESTS_INTEGRATION_HELPERS_H
#define SVN2GIT_TESTS_INTEGRATION_HELPERS_H

#include "svn2git/command_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <unistd.h> // mkdtemp lives here on macOS (POSIX puts it in stdlib.h)

#include <cstdlib>
#include <string>

namespace testhelpers {

/// True when @p tool is runnable on PATH.
inline bool haveTool(const std::string& tool)
{
    return svn2git::CommandRunner::run("command -v " + tool).ok();
}

/// Skip the enclosing test unless every listed tool is available.
#define REQUIRE_TOOLS(...)                                                               \
    do {                                                                                 \
        for (const char* toolName : {__VA_ARGS__}) {                                     \
            if (!testhelpers::haveTool(toolName))                                        \
                SKIP("required tool not available: " << toolName);                       \
        }                                                                                \
    } while (0)

/// Temporary directory, recursively removed on destruction.
struct TempDir {
    std::string path;

    TempDir()
    {
        char templatePath[] = "/tmp/svn2git-test-XXXXXX";
        const char* created = mkdtemp(templatePath);
        REQUIRE(created != nullptr);
        path = created;
    }

    ~TempDir()
    {
        if (!path.empty() && path.rfind("/tmp/svn2git-test-", 0) == 0)
            svn2git::CommandRunner::run("rm -rf "
                                        + svn2git::CommandRunner::shellQuote(path));
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

/// Create the fixture SVN repository via create-test-svn-repo.sh.
/// @return the file:// URL of the repository
inline std::string createTestSvnRepo(const TempDir& dir)
{
    const std::string script
        = std::string(SVN2GIT_FIXTURES_DIR) + "/create-test-svn-repo.sh";
    const svn2git::CommandResult result = svn2git::CommandRunner::run(
        "sh " + svn2git::CommandRunner::shellQuote(script) + " "
        + svn2git::CommandRunner::shellQuote(dir.path));
    INFO("fixture script output: " << result.output);
    REQUIRE(result.ok());
    return "file://" + dir.path + "/repo";
}

/// Run a git command inside @p repoPath, requiring success.
inline std::string gitIn(const std::string& repoPath, const std::string& args)
{
    const svn2git::CommandResult result = svn2git::CommandRunner::run(
        "git -C " + svn2git::CommandRunner::shellQuote(repoPath) + " " + args);
    INFO("git " << args << " output: " << result.output);
    REQUIRE(result.ok());
    return result.output;
}

/// Create a Git repository with @p commitCount commits on 'main',
/// a 'release-1.0' branch and a 'v1.0.0' tag — the shape the fixture
/// SVN repository converts into.
inline void createTestGitRepo(const std::string& repoPath, int commitCount = 6)
{
    svn2git::CommandRunner::run("mkdir -p "
                                + svn2git::CommandRunner::shellQuote(repoPath));
    gitIn(repoPath, "init --quiet --initial-branch=main");
    gitIn(repoPath, "config user.name 'John Smith'");
    gitIn(repoPath, "config user.email john.smith@example.com");
    for (int i = 1; i <= commitCount; ++i) {
        svn2git::CommandRunner::run(
            "echo 'revision " + std::to_string(i) + "' > "
            + svn2git::CommandRunner::shellQuote(repoPath + "/file.txt"));
        gitIn(repoPath, "add file.txt");
        gitIn(repoPath, "commit --quiet -m 'commit " + std::to_string(i) + "'");
    }
    gitIn(repoPath, "branch release-1.0");
    gitIn(repoPath, "tag v1.0.0");
}

} // namespace testhelpers

#endif // SVN2GIT_TESTS_INTEGRATION_HELPERS_H
