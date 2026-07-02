/*
 *  svn2git enhanced — ConfigValidator unit tests (Catch2)
 */

#include "svn2git/config_validator.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>

using svn2git::ConfigCheck;
using svn2git::ConfigReport;
using svn2git::ConfigValidator;
using svn2git::ErrorReporter;

namespace {

struct TempFile {
    std::string path;
    TempFile(std::string name, const std::string& content)
        : path(std::move(name))
    {
        std::ofstream file(path, std::ios::trunc);
        file << content;
    }
    ~TempFile() { std::remove(path.c_str()); }
};

const char* kGoodAuthors = "jsmith = John Smith <john.smith@example.com>\n"
                           "akowalska = Anna Kowalska <anna@example.com>\n";

const char* kGoodRules = "create repository p\nend repository\n"
                         "match /trunk/\n  repository p\n  branch main\nend match\n";

} // namespace

TEST_CASE("valid authors file passes the check", "[config-validator]")
{
    TempFile authors("cfg_authors_ok.txt", kGoodAuthors);
    ErrorReporter reporter;
    ConfigValidator validator(authors.path, "", "", reporter);

    const ConfigCheck check = validator.validateAuthorsFile();
    CHECK(check.passed);
    CHECK(check.findings.empty());
}

TEST_CASE("malformed authors line fails the check", "[config-validator]")
{
    TempFile authors("cfg_authors_bad.txt",
                     "jsmith John Smith john.smith@example.com\n"); // no '='
    ErrorReporter reporter;
    ConfigValidator validator(authors.path, "", "", reporter);

    const ConfigCheck check = validator.validateAuthorsFile();
    CHECK_FALSE(check.passed);
    REQUIRE_FALSE(check.findings.empty());
}

TEST_CASE("invalid email in authors file fails the check", "[config-validator]")
{
    TempFile authors("cfg_authors_email.txt", "jsmith = John Smith <not-an-email>\n");
    ErrorReporter reporter;
    ConfigValidator validator(authors.path, "", "", reporter);

    const ConfigCheck check = validator.validateAuthorsFile();
    CHECK_FALSE(check.passed);
}

TEST_CASE("empty authors file fails the check", "[config-validator]")
{
    TempFile authors("cfg_authors_empty.txt", "# only comments\n");
    ErrorReporter reporter;
    ConfigValidator validator(authors.path, "", "", reporter);

    CHECK_FALSE(validator.validateAuthorsFile().passed);
}

TEST_CASE("valid rules file passes the check", "[config-validator]")
{
    TempFile rules("cfg_rules_ok.rules", kGoodRules);
    ErrorReporter reporter;
    ConfigValidator validator("", rules.path, "", reporter);

    CHECK(validator.validateRulesFile().passed);
}

TEST_CASE("broken rules file fails the check", "[config-validator]")
{
    TempFile rules("cfg_rules_bad.rules",
                   "match /trunk/\n"); // unterminated, no repositories
    ErrorReporter reporter;
    ConfigValidator validator("", rules.path, "", reporter);

    const ConfigCheck check = validator.validateRulesFile();
    CHECK_FALSE(check.passed);
    REQUIRE_FALSE(check.findings.empty());
}

TEST_CASE("well-formed YAML passes the light check", "[config-validator]")
{
    TempFile yaml("cfg_orch_ok.yml",
                  "---\n"
                  "migration:\n"
                  "  repository: testproject\n"
                  "  steps:\n"
                  "    - validate\n"
                  "    - convert\n");
    ErrorReporter reporter;
    ConfigValidator validator("", "", yaml.path, reporter);

    CHECK(validator.validateYamlFile().passed);
}

TEST_CASE("tab-indented YAML fails the light check", "[config-validator]")
{
    TempFile yaml("cfg_orch_tabs.yml", "migration:\n\trepository: x\n");
    ErrorReporter reporter;
    ConfigValidator validator("", "", yaml.path, reporter);

    CHECK_FALSE(validator.validateYamlFile().passed);
}

TEST_CASE("missing YAML file fails the check", "[config-validator]")
{
    ErrorReporter reporter;
    ConfigValidator validator("", "", "/nonexistent/orchestration.yml", reporter);

    CHECK_FALSE(validator.validateYamlFile().passed);
    CHECK(reporter.hasErrors());
}

TEST_CASE("validateAll aggregates all requested checks", "[config-validator]")
{
    TempFile authors("cfg_all_authors.txt", kGoodAuthors);
    TempFile rules("cfg_all_rules.rules", kGoodRules);
    ErrorReporter reporter;
    ConfigValidator validator(authors.path, rules.path, "", reporter);

    const ConfigReport report = validator.validateAll();
    CHECK(report.ok);
    REQUIRE(report.checks.size() == 2); // YAML skipped when not provided
    CHECK(report.checks[0].passed);
    CHECK(report.checks[1].passed);

    const std::string text = report.toText();
    CHECK(text.find("PASS") != std::string::npos);
    CHECK(text.find("authors.txt format") != std::string::npos);
    CHECK(text.find(".rules syntax") != std::string::npos);
}

TEST_CASE("validateAll fails when any check fails", "[config-validator]")
{
    TempFile authors("cfg_mixed_authors.txt", kGoodAuthors);
    TempFile rules("cfg_mixed_rules.rules", "match /broken\n");
    ErrorReporter reporter;
    ConfigValidator validator(authors.path, rules.path, "", reporter);

    const ConfigReport report = validator.validateAll();
    CHECK_FALSE(report.ok);
    CHECK(report.toText().find("FAIL") != std::string::npos);
}

TEST_CASE("validateAll with nothing to validate is a failure", "[config-validator]")
{
    ErrorReporter reporter;
    ConfigValidator validator("", "", "", reporter);

    const ConfigReport report = validator.validateAll();
    CHECK_FALSE(report.ok);
    CHECK(reporter.hasErrors());
}
