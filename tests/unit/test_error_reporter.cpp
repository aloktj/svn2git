/*
 *  svn2git enhanced — ErrorReporter unit tests (Catch2)
 */

#include "svn2git/error_reporter.h"

#include "unit_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

using svn2git::ErrorCode;
using svn2git::ErrorEntry;
using svn2git::ErrorReporter;

namespace {

/// Read a whole file into a string ("" when unreadable).
std::string slurp(const std::string& path)
{
    std::ifstream file(path);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

/// RAII deleter for temporary report files.
struct FileGuard {
    std::string path;
    ~FileGuard() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("every error code has a distinct stable name", "[error-reporter]")
{
    const ErrorCode codes[] = {
        ErrorCode::InvalidRulesSyntax,      ErrorCode::MissingAuthors,
        ErrorCode::StructuralRevisionError, ErrorCode::CopySourceNotFound,
        ErrorCode::CommitCountMismatch,     ErrorCode::AuthorNotFound,
        ErrorCode::GitLabConnectionError,   ErrorCode::InvalidEmail,
        ErrorCode::DuplicateAuthorEntry,    ErrorCode::InvalidRegexPattern,
        ErrorCode::FileAccessError,         ErrorCode::ExternalToolError,
        ErrorCode::ContentMismatch,         ErrorCode::FileMissingInGit,
        ErrorCode::UnmappedSvnPath,
    };
    std::set<std::string> names;
    for (const ErrorCode code : codes) {
        const std::string name = ErrorReporter::codeName(code);
        CHECK(name.rfind("E0", 0) == 0); // E0xx prefix
        names.insert(name);
    }
    CHECK(names.size() == std::size(codes)); // all distinct
}

TEST_CASE("every error code maps to a non-empty suggestion", "[error-reporter]")
{
    const ErrorCode codes[] = {
        ErrorCode::InvalidRulesSyntax,      ErrorCode::MissingAuthors,
        ErrorCode::StructuralRevisionError, ErrorCode::CopySourceNotFound,
        ErrorCode::CommitCountMismatch,     ErrorCode::AuthorNotFound,
        ErrorCode::GitLabConnectionError,   ErrorCode::InvalidEmail,
        ErrorCode::DuplicateAuthorEntry,    ErrorCode::InvalidRegexPattern,
        ErrorCode::FileAccessError,         ErrorCode::ExternalToolError,
        ErrorCode::ContentMismatch,         ErrorCode::FileMissingInGit,
        ErrorCode::UnmappedSvnPath,
    };
    for (const ErrorCode code : codes)
        CHECK_FALSE(ErrorReporter::suggestionFor(code).empty());
}

TEST_CASE("reporting accumulates entries with timestamps", "[error-reporter]")
{
    ErrorReporter reporter;
    REQUIRE_FALSE(reporter.hasErrors());

    reporter.report(ErrorCode::MissingAuthors, "user 'jdoe' unmapped", "authors.txt");
    reporter.report(ErrorCode::InvalidEmail, "bad email");

    REQUIRE(reporter.hasErrors());
    REQUIRE(reporter.errorCount() == 2);

    const ErrorEntry& first = reporter.entries().front();
    CHECK(first.code == ErrorCode::MissingAuthors);
    CHECK(first.message == "user 'jdoe' unmapped");
    CHECK(first.context == "authors.txt");
    // ISO8601 Z-suffixed timestamp, e.g. 2025-07-02T10:30:00Z
    REQUIRE(first.timestamp.size() == 20);
    CHECK(first.timestamp[4] == '-');
    CHECK(first.timestamp[10] == 'T');
    CHECK(first.timestamp.back() == 'Z');
}

TEST_CASE("formatting includes code, message, context and suggestion", "[error-reporter]")
{
    ErrorReporter reporter;
    reporter.report(ErrorCode::InvalidRegexPattern, "bad pattern '['", "rules:12");
    const std::string text = ErrorReporter::format(reporter.entries().front());

    CHECK(text.find("E010_INVALID_REGEX_PATTERN") != std::string::npos);
    CHECK(text.find("bad pattern '['") != std::string::npos);
    CHECK(text.find("rules:12") != std::string::npos);
    CHECK(text.find("Suggestion:") != std::string::npos);
}

TEST_CASE("report file is generated with all entries", "[error-reporter]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-error-report", ".txt")};

    ErrorReporter reporter;
    reporter.report(ErrorCode::CommitCountMismatch, "expected 10, got 9");
    reporter.report(ErrorCode::AuthorNotFound, "author 'ghost'");

    REQUIRE(reporter.generateReport(guard.path));
    const std::string content = slurp(guard.path);
    CHECK(content.find("Total errors: 2") != std::string::npos);
    CHECK(content.find("E005_COMMIT_COUNT_MISMATCH") != std::string::npos);
    CHECK(content.find("E006_AUTHOR_NOT_FOUND") != std::string::npos);
}

TEST_CASE("empty report notes the absence of errors", "[error-reporter]")
{
    FileGuard guard {testhelpers::uniqueTempPath("svn2git-empty-report", ".txt")};

    ErrorReporter reporter;
    REQUIRE(reporter.generateReport(guard.path));
    CHECK(slurp(guard.path).find("No errors were recorded") != std::string::npos);
}

TEST_CASE("report generation fails cleanly on unwritable path", "[error-reporter]")
{
    ErrorReporter reporter;
    reporter.report(ErrorCode::FileAccessError, "sample");
    CHECK_FALSE(reporter.generateReport("/nonexistent-dir/report.txt"));
}

TEST_CASE("clear() empties the reporter", "[error-reporter]")
{
    ErrorReporter reporter;
    reporter.report(ErrorCode::MissingAuthors, "x");
    REQUIRE(reporter.errorCount() == 1);
    reporter.clear();
    CHECK(reporter.errorCount() == 0);
    CHECK_FALSE(reporter.hasErrors());
}
