/*
 *  svn2git enhanced — .rules file validation and dry-run engine
 *
 *  Parses the svn2git rules format (create repository / match blocks,
 *  see the samples directory), verifies every match pattern is a valid
 *  expression, detects unreachable ("shadowed") and conflicting rules,
 *  and offers a dry-run mode that shows how sample SVN paths would map
 *  to repositories/branches before any migration is attempted.
 *
 *  Simplification note: patterns are validated with std::regex
 *  (ECMAScript grammar) whereas the legacy Qt engine uses QRegExp.
 *  The rule syntax used in practice (character classes, groups,
 *  alternation, anchors) is identical in both grammars.
 *
 *  Related classes: reports through ErrorReporter; used by
 *  ConfigValidator and the CLI (--dry-run / --debug-rules).
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_RULES_VALIDATOR_H
#define SVN2GIT_RULES_VALIDATOR_H

#include "svn2git/error_reporter.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace svn2git {

/// One parsed `match … end match` block.
struct MatchRule {
    std::string pattern; ///< raw regex text from the rules file
    std::string repository; ///< target repository ("" = ignore rule)
    std::string branch; ///< target branch ("" = repository default)
    std::string prefix; ///< subdirectory prefix inside the branch
    long minRevision = -1; ///< first revision the rule applies to (-1 = any)
    long maxRevision = -1; ///< last revision the rule applies to (-1 = any)
    long lineNumber = 0; ///< 1-based line of the `match` keyword
};

/// One parsed `create repository … end repository` block.
struct RepositoryRule {
    std::string name; ///< repository name
    long lineNumber = 0; ///< 1-based line of the declaration
};

/// Outcome of validating or dry-running a rules file.
struct RulesValidationResult {
    bool valid = false; ///< no errors found
    std::vector<std::string> errors; ///< blocking problems
    std::vector<std::string> warnings; ///< suspicious but non-blocking
    std::size_t ruleCount = 0; ///< number of match rules parsed
    std::size_t repositoryCount = 0; ///< number of repositories declared
    std::vector<std::string> dryRunLines; ///< per-path outcome (dry-run only)
};

/// Validates svn2git .rules files and previews their effect.
class RulesValidator {
public:
    /// @param rulesFilePath  path to the .rules file
    /// @param reporter       shared error sink for structured errors
    RulesValidator(std::string rulesFilePath, ErrorReporter& reporter);

    /// Parse the file and run all static checks: syntax, regex validity,
    /// unknown repository references, duplicate/shadowed rules, patterns
    /// not ending in '/'.
    RulesValidationResult validate();

    /// True when @p pattern compiles as an ECMAScript regex.
    static bool validateRegex(const std::string& pattern);

    /// Validate, then map each path in @p samplePaths through the rule
    /// list exactly like the converter would (first match wins) and
    /// record the outcome in dryRunLines. Unmatched paths produce
    /// warnings because their history would be silently dropped.
    RulesValidationResult dryRun(const std::vector<std::string>& samplePaths);

    /// Interactive rule debugger: reads SVN paths from @p in one per
    /// line, writes the matching rule (or "no match") to @p out.
    /// Terminates on EOF or an empty line. Defaults to stdin/stdout in
    /// the CLI; tests pass string streams.
    void interactiveDebug(std::istream& in, std::ostream& out);

    /// Rules parsed by the last validate()/dryRun() call.
    const std::vector<MatchRule>& matchRules() const { return m_matchRules; }
    const std::vector<RepositoryRule>& repositories() const { return m_repositories; }

private:
    /// Load and tokenize the rules file, following `include` directives
    /// recursively (like the converter's parser). Populates m_matchRules /
    /// m_repositories and appends findings to @p result.
    bool parse(RulesValidationResult& result);

    /// Parse one file; called by parse() and recursively for includes.
    /// @param filePath  file to read (include paths resolve relative to it)
    /// @param depth     current include nesting depth (cycle/depth guard)
    /// @param visited   files already being parsed on this include chain
    bool parseFile(const std::string& filePath, RulesValidationResult& result, int depth,
                   std::vector<std::string>& visited);

    /// Cross-rule semantic checks (needs parse() to have succeeded).
    void analyze(RulesValidationResult& result) const;

    /// Index of the first rule matching @p path (first-match-wins), or
    /// -1 when none matches. Applies substring search semantics at the
    /// start of the path, mirroring the converter's behavior.
    int firstMatch(const std::string& path) const;

    std::string m_rulesFilePath;
    ErrorReporter& m_reporter;
    std::vector<MatchRule> m_matchRules;
    std::vector<RepositoryRule> m_repositories;
};

} // namespace svn2git

#endif // SVN2GIT_RULES_VALIDATOR_H
