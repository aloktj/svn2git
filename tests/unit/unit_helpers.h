/*
 *  svn2git enhanced — shared helpers for unit tests
 */

#ifndef SVN2GIT_TESTS_UNIT_HELPERS_H
#define SVN2GIT_TESTS_UNIT_HELPERS_H

#include <unistd.h> // getpid

#include <atomic>
#include <filesystem>
#include <string>

namespace testhelpers {

/// Unique temp-file path for one test case: system temp directory plus a
/// PID + per-process counter suffix. Keeps parallel test execution
/// (ctest -j) from clobbering a shared fixture file.
inline std::string uniqueTempPath(const std::string& stem, const std::string& extension)
{
    static std::atomic<unsigned> counter {0};
    const std::string name = stem + "-" + std::to_string(getpid()) + "-"
        + std::to_string(++counter) + extension;
    return (std::filesystem::temp_directory_path() / name).string();
}

} // namespace testhelpers

#endif // SVN2GIT_TESTS_UNIT_HELPERS_H
