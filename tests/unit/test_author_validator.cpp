/*
 *  svn2git enhanced — AuthorValidator unit tests (Catch2)
 *
 *  SVN access is faked through the injectable Runner, so these tests
 *  run without a subversion installation.
 */

#include "svn2git/author_validator.h"

#include "unit_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using svn2git::AuthorMapping;
using svn2git::AuthorValidationReport;
using svn2git::AuthorValidator;
using svn2git::CommandResult;
using svn2git::ErrorCode;
using svn2git::ErrorReporter;

namespace {

/// Canned `svn log --quiet` output: 3 authors, 5 commits.
const char* kSvnLogOutput
    = "------------------------------------------------------------------------\n"
      "r5 | jsmith | 2024-03-01 09:00:00 +0000 (Fri, 01 Mar 2024)\n"
      "------------------------------------------------------------------------\n"
      "r4 | mmustermann | 2024-02-01 09:00:00 +0000 (Thu, 01 Feb 2024)\n"
      "------------------------------------------------------------------------\n"
      "r3 | jsmith | 2024-01-20 09:00:00 +0000 (Sat, 20 Jan 2024)\n"
      "------------------------------------------------------------------------\n"
      "r2 | akowalska | 2024-01-10 09:00:00 +0000 (Wed, 10 Jan 2024)\n"
      "------------------------------------------------------------------------\n"
      "r1 | jsmith | 2024-01-01 09:00:00 +0000 (Mon, 01 Jan 2024)\n"
      "------------------------------------------------------------------------\n";

svn2git::Runner fakeSvn(int exitCode, std::string output)
{
    return [exitCode, output = std::move(output)](const std::string&) {
        return CommandResult {exitCode, output};
    };
}

/// Temporary authors file with a unique per-test path (safe under
/// parallel test execution), deleted on scope exit.
struct TempAuthorsFile {
    std::string path;
    explicit TempAuthorsFile(const std::string& content)
        : path(testhelpers::uniqueTempPath("svn2git-authors", ".txt"))
    {
        std::ofstream file(path, std::ios::trunc);
        file << content;
    }
    ~TempAuthorsFile() { std::remove(path.c_str()); }
};

} // namespace

TEST_CASE("authors are extracted with commit counts", "[author-validator]")
{
    TempAuthorsFile authors(""); // empty mapping file
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const std::vector<AuthorMapping> extracted = validator.extractSVNAuthors();
    REQUIRE(extracted.size() == 3);

    // std::map ordering — alphabetical by username.
    CHECK(extracted[0].svnUsername == "akowalska");
    CHECK(extracted[0].commitCount == 1);
    CHECK(extracted[1].svnUsername == "jsmith");
    CHECK(extracted[1].commitCount == 3);
    CHECK(extracted[2].svnUsername == "mmustermann");
    CHECK(extracted[2].commitCount == 1);
}

TEST_CASE("mapped identities are attached during extraction", "[author-validator]")
{
    TempAuthorsFile authors("jsmith = John Smith <john.smith@example.com>\n");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const auto extracted = validator.extractSVNAuthors();
    REQUIRE(extracted.size() == 3);
    CHECK(extracted[1].fullName == "John Smith");
    CHECK(extracted[1].email == "john.smith@example.com");
    CHECK(extracted[0].fullName.empty()); // akowalska unmapped
}

TEST_CASE("svn failure is reported and yields no authors", "[author-validator]")
{
    TempAuthorsFile authors("");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(1, "svn: E170013: Unable to connect\n"));

    CHECK(validator.extractSVNAuthors().empty());
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries().front().code == ErrorCode::ExternalToolError);
}

TEST_CASE("empty history is reported as an error", "[author-validator]")
{
    TempAuthorsFile authors("");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter, fakeSvn(0, ""));

    CHECK(validator.extractSVNAuthors().empty());
    CHECK(reporter.hasErrors());
}

TEST_CASE("full coverage passes validation", "[author-validator]")
{
    TempAuthorsFile authors("jsmith = John Smith <john.smith@example.com>\n"
                            "mmustermann = Max Mustermann <max@example.com>\n"
                            "akowalska = Anna Kowalska <anna@example.com>\n");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const AuthorValidationReport report = validator.validateCoverage();
    CHECK(report.ok);
    CHECK(report.totalSvnAuthors == 3);
    CHECK(report.mappedAuthors == 3);
    CHECK(report.missingAuthors.empty());
}

TEST_CASE("missing authors are detected with suggestions", "[author-validator]")
{
    TempAuthorsFile authors("jsmith = John Smith <john.smith@example.com>\n");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const AuthorValidationReport report = validator.validateCoverage();
    CHECK_FALSE(report.ok);
    REQUIRE(report.missingAuthors.size() == 2);
    CHECK(report.missingAuthors[0].svnUsername == "akowalska");
    CHECK(report.missingAuthors[1].svnUsername == "mmustermann");
    // Each missing author produces a suggestion message.
    REQUIRE(report.messages.size() >= 2);
    CHECK(report.messages[0].find("akowalska") != std::string::npos);
}

TEST_CASE("invalid mapped email fails coverage validation", "[author-validator]")
{
    TempAuthorsFile authors("jsmith = John Smith <not-an-email>\n"
                            "mmustermann = Max Mustermann <max@example.com>\n"
                            "akowalska = Anna Kowalska <anna@example.com>\n");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const AuthorValidationReport report = validator.validateCoverage();
    CHECK_FALSE(report.ok);
    REQUIRE(report.invalidEmails.size() == 1);
    CHECK(report.invalidEmails.front() == "jsmith");
}

TEST_CASE("duplicate author entries fail coverage validation", "[author-validator]")
{
    TempAuthorsFile authors("jsmith = John Smith <john@example.com>\n"
                            "jsmith = Johnny Smith <johnny@example.com>\n"
                            "mmustermann = Max Mustermann <max@example.com>\n"
                            "akowalska = Anna Kowalska <anna@example.com>\n");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const AuthorValidationReport report = validator.validateCoverage();
    CHECK_FALSE(report.ok);
    CHECK(report.duplicateEntries.size() == 1);
}

TEST_CASE("unreadable authors file is reported", "[author-validator]")
{
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", "/nonexistent/authors.txt", reporter,
                              fakeSvn(0, kSvnLogOutput));

    validator.loadAuthorsFile();
    REQUIRE(reporter.hasErrors());
    CHECK(reporter.entries().front().code == ErrorCode::FileAccessError);
}

TEST_CASE("email validation accepts valid and rejects invalid addresses",
          "[author-validator]")
{
    CHECK(AuthorValidator::isValidEmail("john.smith@example.com"));
    CHECK(AuthorValidator::isValidEmail("a+b@sub.domain.org"));
    CHECK(AuthorValidator::isValidEmail("x@y.z"));

    CHECK_FALSE(AuthorValidator::isValidEmail(""));
    CHECK_FALSE(AuthorValidator::isValidEmail("no-at-sign.example.com"));
    CHECK_FALSE(AuthorValidator::isValidEmail("two@@example.com"));
    CHECK_FALSE(AuthorValidator::isValidEmail("@example.com"));
    CHECK_FALSE(AuthorValidator::isValidEmail("user@nodot"));
    CHECK_FALSE(AuthorValidator::isValidEmail("user@.leadingdot.com"));
    CHECK_FALSE(AuthorValidator::isValidEmail("user@trailingdot."));
    CHECK_FALSE(AuthorValidator::isValidEmail("has space@example.com"));
}

TEST_CASE("suggestions title-case structured usernames", "[author-validator]")
{
    CHECK(AuthorValidator::suggestMapping("john.smith")
          == "john.smith = John Smith <john.smith@localhost>");
    CHECK(AuthorValidator::suggestMapping("anna_kowalska")
          == "anna_kowalska = Anna Kowalska <anna_kowalska@localhost>");
    // Unstructured usernames stay as-is.
    CHECK(AuthorValidator::suggestMapping("jsmith")
          == "jsmith = jsmith <jsmith@localhost>");
}

TEST_CASE("auto-mapping preserves existing entries and adds placeholders",
          "[author-validator]")
{
    TempAuthorsFile authors("jsmith = John Smith <john.smith@example.com>\n");
    ErrorReporter reporter;
    AuthorValidator validator("file:///fake", authors.path, reporter,
                              fakeSvn(0, kSvnLogOutput));

    const std::string generated = validator.autoMapAuthors();
    REQUIRE_FALSE(generated.empty());
    CHECK(generated.find("jsmith = John Smith <john.smith@example.com>")
          != std::string::npos);
    CHECK(generated.find("akowalska = akowalska <akowalska@localhost>")
          != std::string::npos);
    CHECK(generated.find("mmustermann = mmustermann <mmustermann@localhost>")
          != std::string::npos);
}
