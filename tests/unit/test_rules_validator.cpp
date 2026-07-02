/*
 *  svn2git enhanced — RulesValidator unit tests (Catch2)
 */

#include "svn2git/rules_validator.h"

#include "unit_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using svn2git::ErrorReporter;
using svn2git::RulesValidationResult;
using svn2git::RulesValidator;

namespace {

/// Temporary rules file with a unique per-test path (safe under
/// parallel test execution), deleted on scope exit.
struct TempRulesFile {
    std::string path;
    explicit TempRulesFile(const std::string& content)
        : path(testhelpers::uniqueTempPath("svn2git-rules", ".rules"))
    {
        std::ofstream file(path, std::ios::trunc);
        file << content;
    }
    ~TempRulesFile() { std::remove(path.c_str()); }
};

const char* kValidRules = "create repository myproject\n"
                          "end repository\n"
                          "\n"
                          "match /trunk/\n"
                          "  repository myproject\n"
                          "  branch main\n"
                          "end match\n"
                          "\n"
                          "match /branches/([^/]+)/\n"
                          "  repository myproject\n"
                          "  branch \\1\n"
                          "end match\n"
                          "\n"
                          "match /tags/([^/]+)/\n"
                          "  repository myproject\n"
                          "  branch refs/tags/\\1\n"
                          "end match\n";

} // namespace

TEST_CASE("valid rules file parses with correct counts", "[rules-validator]")
{
    TempRulesFile rules(kValidRules);
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK(result.valid);
    CHECK(result.errors.empty());
    CHECK(result.ruleCount == 3);
    CHECK(result.repositoryCount == 1);

    REQUIRE(validator.matchRules().size() == 3);
    CHECK(validator.matchRules()[0].pattern == "/trunk/");
    CHECK(validator.matchRules()[0].repository == "myproject");
    CHECK(validator.matchRules()[0].branch == "main");
}

TEST_CASE("min/max revision bounds are parsed", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /trunk/\n"
                        "  repository p\n"
                        "  min revision 10\n"
                        "  max revision 20\n"
                        "end match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK(result.valid);
    REQUIRE(validator.matchRules().size() == 1);
    CHECK(validator.matchRules()[0].minRevision == 10);
    CHECK(validator.matchRules()[0].maxRevision == 20);
}

TEST_CASE("inverted revision bounds are an error", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /trunk/\n"
                        "  repository p\n"
                        "  min revision 20\n"
                        "  max revision 10\n"
                        "end match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK_FALSE(result.valid);
}

TEST_CASE("invalid regex patterns are detected", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /branches/([^/+/\n" // unbalanced bracket/paren
                        "  repository p\n"
                        "end match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK_FALSE(result.valid);
    REQUIRE_FALSE(result.errors.empty());
    bool foundRegexError = false;
    for (const std::string& error : result.errors)
        if (error.find("invalid regular expression") != std::string::npos)
            foundRegexError = true;
    CHECK(foundRegexError);
}

TEST_CASE("validateRegex distinguishes good from bad patterns", "[rules-validator]")
{
    CHECK(RulesValidator::validateRegex("/trunk/"));
    CHECK(RulesValidator::validateRegex("/branches/([^/]+)/"));
    CHECK(RulesValidator::validateRegex("/(branches|tags)/([^/]+)/"));

    CHECK_FALSE(RulesValidator::validateRegex("/branches/([^/+/"));
    CHECK_FALSE(RulesValidator::validateRegex("*invalid"));
}

TEST_CASE("unterminated match block is an error", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /trunk/\n"
                        "  repository p\n"); // no 'end match'
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK_FALSE(result.valid);
}

TEST_CASE("undeclared repository reference is an error", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /trunk/\n"
                        "  repository ghost\n"
                        "end match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK_FALSE(result.valid);
}

TEST_CASE("backreference repository targets are not flagged", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /projects/([^/]+)/trunk/\n"
                        "  repository \\1\n"
                        "end match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    CHECK(validator.validate().valid);
}

TEST_CASE("duplicate patterns produce an unreachable-rule warning", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /trunk/\n  repository p\n  branch main\nend match\n"
                        "match /trunk/\n  repository p\n  branch other\nend match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK(result.valid); // warning, not error
    bool foundShadowWarning = false;
    for (const std::string& warning : result.warnings)
        if (warning.find("unreachable") != std::string::npos)
            foundShadowWarning = true;
    CHECK(foundShadowWarning);
}

TEST_CASE("pattern without trailing slash warns", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /trunk\n  repository p\nend match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK(result.valid);
    REQUIRE_FALSE(result.warnings.empty());
    CHECK(result.warnings.front().find("does not end in '/'") != std::string::npos);
}

TEST_CASE("missing rules file is an error", "[rules-validator]")
{
    ErrorReporter reporter;
    RulesValidator validator("/nonexistent/file.rules", reporter);

    const RulesValidationResult result = validator.validate();
    CHECK_FALSE(result.valid);
    CHECK(reporter.hasErrors());
}

TEST_CASE("empty rules file is an error", "[rules-validator]")
{
    TempRulesFile rules("");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    CHECK_FALSE(validator.validate().valid);
}

TEST_CASE("dry run maps paths first-match-wins", "[rules-validator]")
{
    TempRulesFile rules(kValidRules);
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.dryRun({
        "/trunk/src/main.c/",
        "/branches/release-1.0/",
        "/tags/v1.0.0/",
        "/unrelated/path/",
    });

    REQUIRE(result.dryRunLines.size() == 4);
    CHECK(result.dryRunLines[0].find("repository 'myproject'") != std::string::npos);
    CHECK(result.dryRunLines[0].find("branch 'main'") != std::string::npos);
    CHECK(result.dryRunLines[1].find("repository 'myproject'") != std::string::npos);
    CHECK(result.dryRunLines[2].find("repository 'myproject'") != std::string::npos);
    CHECK(result.dryRunLines[3].find("NO MATCH") != std::string::npos);

    // The unmatched path also produces a warning.
    bool warned = false;
    for (const std::string& warning : result.warnings)
        if (warning.find("/unrelated/path/") != std::string::npos)
            warned = true;
    CHECK(warned);
}

TEST_CASE("dry run routes ignore rules to IGNORED", "[rules-validator]")
{
    TempRulesFile rules("create repository p\nend repository\n"
                        "match /vendor/\nend match\n" // no repository = ignore
                        "match /trunk/\n  repository p\nend match\n");
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    const RulesValidationResult result = validator.dryRun({"/vendor/lib/"});
    REQUIRE(result.dryRunLines.size() == 1);
    CHECK(result.dryRunLines[0].find("IGNORED") != std::string::npos);
}

TEST_CASE("include directives are followed recursively", "[rules-validator]")
{
    // The included file declares the repository; the including file only
    // has the match rules — validation must see the merged rule set.
    TempRulesFile included("create repository incproject\nend repository\n");
    const std::string includedName
        = std::filesystem::path(included.path).filename().string();
    TempRulesFile main("include " + includedName
                       + "\n"
                         "match /trunk/\n  repository incproject\n  branch main\n"
                         "end match\n");

    ErrorReporter reporter;
    RulesValidator validator(main.path, reporter);
    const RulesValidationResult result = validator.validate();

    CHECK(result.valid);
    CHECK(result.repositoryCount == 1);
    CHECK(result.ruleCount == 1);
}

TEST_CASE("missing include target is an error", "[rules-validator]")
{
    TempRulesFile main("include does-not-exist.rules\n"
                       "create repository p\nend repository\n"
                       "match /trunk/\n  repository p\nend match\n");
    ErrorReporter reporter;
    RulesValidator validator(main.path, reporter);

    CHECK_FALSE(validator.validate().valid);
}

TEST_CASE("circular includes are detected", "[rules-validator]")
{
    // A file that includes itself must error out instead of recursing.
    TempRulesFile main("");
    {
        std::ofstream file(main.path, std::ios::trunc);
        file << "include " << std::filesystem::path(main.path).filename().string()
             << "\n";
    }
    ErrorReporter reporter;
    RulesValidator validator(main.path, reporter);

    const RulesValidationResult result = validator.validate();
    CHECK_FALSE(result.valid);
    bool foundCycleError = false;
    for (const std::string& error : result.errors)
        if (error.find("circular include") != std::string::npos)
            foundCycleError = true;
    CHECK(foundCycleError);
}

TEST_CASE("interactive debugger refuses a broken rules file", "[rules-validator]")
{
    TempRulesFile rules("match /trunk/\n"); // unterminated block
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    std::istringstream in("/trunk/file.c/\n");
    std::ostringstream out;
    validator.interactiveDebug(in, out);

    const std::string text = out.str();
    CHECK(text.find("fix them before debugging") != std::string::npos);
    // The debugger must not enter the query loop on a broken rule set.
    CHECK(text.find("matched rule") == std::string::npos);
    CHECK(text.find("bye") == std::string::npos);
}

TEST_CASE("interactive debugger resolves paths and terminates on empty line",
          "[rules-validator]")
{
    TempRulesFile rules(kValidRules);
    ErrorReporter reporter;
    RulesValidator validator(rules.path, reporter);

    std::istringstream in("/trunk/file.c/\n/nowhere/\n\n");
    std::ostringstream out;
    validator.interactiveDebug(in, out);

    const std::string text = out.str();
    CHECK(text.find("matched rule at line") != std::string::npos);
    CHECK(text.find("repository 'myproject'") != std::string::npos);
    CHECK(text.find("no rule matches '/nowhere/'") != std::string::npos);
    CHECK(text.find("bye") != std::string::npos);
}
