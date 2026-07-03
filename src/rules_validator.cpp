/*
 *  svn2git enhanced — rules validation implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/rules_validator.h"

#include "svn2git/logging.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <istream>
#include <ostream>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

namespace svn2git {

namespace {

std::string trim(const std::string& text)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin = std::find_if_not(text.begin(), text.end(), isSpace);
    auto end = std::find_if_not(text.rbegin(), text.rend(), isSpace).base();
    return (begin < end) ? std::string(begin, end) : std::string();
}

/// Split "keyword rest" on the first run of whitespace.
std::pair<std::string, std::string> splitKeyword(const std::string& line)
{
    const std::size_t space = line.find_first_of(" \t");
    if (space == std::string::npos)
        return {line, std::string()};
    return {line.substr(0, space), trim(line.substr(space + 1))};
}

/// Parse a non-negative revision number; returns -1 on failure.
long parseRevision(const std::string& text)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        }))
        return -1;
    try {
        return std::stol(text);
    } catch (const std::exception&) {
        return -1; // overflow
    }
}

} // namespace

RulesValidator::RulesValidator(std::string rulesFilePath, ErrorReporter& reporter)
    : m_rulesFilePath(std::move(rulesFilePath))
    , m_reporter(reporter)
{
}

bool RulesValidator::validateRegex(const std::string& pattern)
{
    try {
        const std::regex compiled(pattern, std::regex::ECMAScript);
        (void)compiled;
        return true;
    } catch (const std::regex_error&) {
        return false;
    }
}

bool RulesValidator::parse(RulesValidationResult& result)
{
    m_matchRules.clear();
    m_repositories.clear();
    std::vector<std::string> visited;
    return parseFile(m_rulesFilePath, result, 0, visited);
}

bool RulesValidator::parseFile(const std::string& filePath, RulesValidationResult& result,
                               int depth, std::vector<std::string>& visited)
{
    // Guard against runaway include chains (cycles are caught separately,
    // this bounds pathological but acyclic nesting).
    constexpr int kMaxIncludeDepth = 16;

    auto log = logging::get("rules-validator");
    visited.push_back(filePath);

    std::ifstream file(filePath);
    if (!file.is_open()) {
        const std::string msg = "cannot open rules file '" + filePath + "'";
        result.errors.push_back(msg);
        m_reporter.report(ErrorCode::FileAccessError, msg, filePath);
        return false;
    }

    // Block-structured format:
    //   create repository <name> … end repository
    //   match <regex>            … end match
    // with per-match actions: repository/branch/prefix/min revision/max revision.
    enum class Block : std::uint8_t { None, Repository, Match };
    Block block = Block::None;
    MatchRule currentMatch;
    RepositoryRule currentRepo;

    std::string line;
    long lineNumber = 0;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos)
            line.erase(comment);
        line = trim(line);
        if (line.empty())
            continue;

        const std::string locus = filePath + ":" + std::to_string(lineNumber);
        auto [keyword, rest] = splitKeyword(line);

        if (block == Block::None) {
            if (keyword == "create") {
                auto [sub, name] = splitKeyword(rest);
                if (sub != "repository" || name.empty()) {
                    result.errors.push_back(locus
                                            + ": expected 'create repository <name>'");
                    continue;
                }
                currentRepo = RepositoryRule {name, lineNumber};
                block = Block::Repository;
            } else if (keyword == "match") {
                if (rest.empty()) {
                    result.errors.push_back(locus + ": 'match' requires a pattern");
                    continue;
                }
                currentMatch = MatchRule {};
                currentMatch.pattern = rest;
                currentMatch.lineNumber = lineNumber;
                block = Block::Match;
            } else if (keyword == "include") {
                // Follow includes recursively, resolving relative paths
                // against the including file — mirroring the converter's
                // parser so validation sees the complete effective rule set.
                if (rest.empty()) {
                    result.errors.push_back(locus + ": 'include' requires a file name");
                    continue;
                }
                std::filesystem::path includePath(rest);
                if (includePath.is_relative())
                    includePath
                        = std::filesystem::path(filePath).parent_path() / includePath;
                const std::string resolved = includePath.lexically_normal().string();
                if (depth + 1 >= kMaxIncludeDepth) {
                    result.errors.push_back(locus + ": include nesting deeper than "
                                            + std::to_string(kMaxIncludeDepth)
                                            + " levels");
                    continue;
                }
                if (std::find(visited.begin(), visited.end(), resolved)
                    != visited.end()) {
                    result.errors.push_back(locus + ": circular include of '" + resolved
                                            + "'");
                    continue;
                }
                parseFile(resolved, result, depth + 1, visited);
            } else if (keyword == "declare") {
                // 'declare VAR=value' — variable substitution handled by
                // the converter; nothing to statically validate here.
                if (rest.find('=') == std::string::npos)
                    result.errors.push_back(locus + ": expected 'declare NAME=value'");
            } else {
                result.errors.push_back(locus + ": unknown top-level keyword '" + keyword
                                        + "'");
            }
            continue;
        }

        if (block == Block::Repository) {
            if (keyword == "end" && rest == "repository") {
                m_repositories.push_back(currentRepo);
                block = Block::None;
            }
            // Repository blocks may carry description lines; anything
            // other than the terminator is accepted verbatim.
            continue;
        }

        // block == Block::Match
        if (keyword == "end" && rest == "match") {
            m_matchRules.push_back(currentMatch);
            block = Block::None;
        } else if (keyword == "repository") {
            currentMatch.repository = rest;
        } else if (keyword == "branch") {
            currentMatch.branch = rest;
        } else if (keyword == "prefix") {
            currentMatch.prefix = rest;
        } else if (keyword == "min") {
            auto [sub, value] = splitKeyword(rest);
            currentMatch.minRevision = (sub == "revision") ? parseRevision(value) : -1;
            if (currentMatch.minRevision < 0)
                result.errors.push_back(locus + ": expected 'min revision <number>'");
        } else if (keyword == "max") {
            auto [sub, value] = splitKeyword(rest);
            currentMatch.maxRevision = (sub == "revision") ? parseRevision(value) : -1;
            if (currentMatch.maxRevision < 0)
                result.errors.push_back(locus + ": expected 'max revision <number>'");
        } else if (keyword == "action") {
            // 'action ignore/export/recurse' accepted by some forks.
            if (rest != "ignore" && rest != "export" && rest != "recurse")
                result.errors.push_back(locus + ": unknown action '" + rest + "'");
        } else {
            result.errors.push_back(locus + ": unknown keyword '" + keyword
                                    + "' inside match block");
        }
    }

    if (block != Block::None)
        result.errors.push_back(filePath + ": unterminated "
                                + (block == Block::Match
                                       ? std::string("'match'")
                                       : std::string("'create repository'"))
                                + " block at end of file");

    log->debug("parsed '{}': {} repositories, {} match rules, {} error(s)", filePath,
               m_repositories.size(), m_matchRules.size(), result.errors.size());
    return true;
}

void RulesValidator::analyze(RulesValidationResult& result) const
{
    std::set<std::string> repoNames;
    for (const RepositoryRule& repo : m_repositories) {
        if (!repoNames.insert(repo.name).second)
            result.errors.push_back(
                m_rulesFilePath + ":" + std::to_string(repo.lineNumber)
                + ": duplicate repository declaration '" + repo.name + "'");
    }

    std::set<std::string> seenPatterns;
    for (const MatchRule& rule : m_matchRules) {
        const std::string locus = m_rulesFilePath + ":" + std::to_string(rule.lineNumber);

        if (!validateRegex(rule.pattern)) {
            result.errors.push_back(locus + ": invalid regular expression '"
                                    + rule.pattern + "'");
            m_reporter.report(ErrorCode::InvalidRegexPattern,
                              "invalid regex '" + rule.pattern + "'", locus);
        }

        // The converter matches directory paths, so patterns must end in
        // '/' (documented requirement in the sample rules).
        if (!rule.pattern.empty() && rule.pattern.back() != '/')
            result.warnings.push_back(
                locus + ": pattern '" + rule.pattern
                + "' does not end in '/' — svn2git rules must match directories");

        if (!rule.repository.empty() && repoNames.count(rule.repository) == 0
            && rule.repository.find('\\') == std::string::npos) {
            // Repository names containing backreferences (\1) are
            // resolved dynamically and cannot be checked statically.
            result.errors.push_back(locus + ": rule targets undeclared repository '"
                                    + rule.repository + "'");
        }

        if (rule.minRevision >= 0 && rule.maxRevision >= 0
            && rule.minRevision > rule.maxRevision)
            result.errors.push_back(
                locus + ": min revision " + std::to_string(rule.minRevision)
                + " exceeds max revision " + std::to_string(rule.maxRevision));

        // Exact duplicate patterns: the second rule is unreachable when
        // revision ranges are unbounded (first match wins).
        if (rule.minRevision < 0 && rule.maxRevision < 0
            && !seenPatterns.insert(rule.pattern).second)
            result.warnings.push_back(
                locus + ": pattern '" + rule.pattern
                + "' duplicates an earlier rule — this rule is unreachable"
                  " (first match wins)");
    }

    if (m_matchRules.empty())
        result.errors.push_back(
            m_rulesFilePath + ": no match rules defined — nothing would be converted");
    if (m_repositories.empty())
        result.warnings.push_back(m_rulesFilePath
                                  + ": no repositories declared in this file");
}

RulesValidationResult RulesValidator::validate()
{
    auto log = logging::get("rules-validator");
    log->info("validating rules file '{}'", m_rulesFilePath);

    RulesValidationResult result;
    if (parse(result))
        analyze(result);

    result.ruleCount = m_matchRules.size();
    result.repositoryCount = m_repositories.size();
    result.valid = result.errors.empty();

    for (const std::string& error : result.errors)
        m_reporter.report(ErrorCode::InvalidRulesSyntax, error, m_rulesFilePath);

    log->info("rules validation: {} rule(s), {} error(s), {} warning(s) — {}",
              result.ruleCount, result.errors.size(), result.warnings.size(),
              result.valid ? "PASS" : "FAIL");
    return result;
}

int RulesValidator::firstMatch(const std::string& path) const
{
    for (std::size_t i = 0; i < m_matchRules.size(); ++i) {
        const MatchRule& rule = m_matchRules[i];
        if (!validateRegex(rule.pattern))
            continue; // invalid patterns already reported by validate()
        // The converter anchors patterns at the start of the SVN path.
        const std::regex re(rule.pattern, std::regex::ECMAScript);
        if (std::regex_search(path, re, std::regex_constants::match_continuous))
            return static_cast<int>(i);
    }
    return -1;
}

RulesValidationResult RulesValidator::dryRun(const std::vector<std::string>& samplePaths)
{
    auto log = logging::get("rules-validator");
    RulesValidationResult result = validate();

    log->info("dry-run over {} sample path(s)", samplePaths.size());
    std::size_t unmatched = 0;
    for (const std::string& path : samplePaths) {
        const int index = firstMatch(path);
        std::ostringstream lineOut;
        if (index < 0) {
            ++unmatched;
            lineOut << path << " -> NO MATCH (history would be dropped)";
            result.warnings.push_back("dry-run: path '" + path + "' matches no rule");
        } else {
            const MatchRule& rule = m_matchRules[static_cast<std::size_t>(index)];
            lineOut << path << " -> ";
            if (rule.repository.empty()) {
                lineOut << "IGNORED (rule at line " << rule.lineNumber << ")";
            } else {
                lineOut << "repository '" << rule.repository << "'";
                if (!rule.branch.empty())
                    lineOut << ", branch '" << rule.branch << "'";
                if (!rule.prefix.empty())
                    lineOut << ", prefix '" << rule.prefix << "'";
                lineOut << " (rule at line " << rule.lineNumber << ")";
            }
        }
        result.dryRunLines.push_back(lineOut.str());
    }

    log->info("dry-run complete: {}/{} path(s) matched", samplePaths.size() - unmatched,
              samplePaths.size());
    return result;
}

namespace {

/// Expand QRegExp-style backreferences (\1…\9) from @p match into
/// @p pattern — the substitution grammar used in svn2git rules files.
std::string expandBackreferences(const std::string& pattern, const std::smatch& match)
{
    std::string expanded;
    expanded.reserve(pattern.size());
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '\\' && i + 1 < pattern.size()
            && std::isdigit(static_cast<unsigned char>(pattern[i + 1])) != 0) {
            const std::size_t group = static_cast<std::size_t>(pattern[i + 1] - '0');
            if (group < match.size())
                expanded += match[group].str();
            ++i;
        } else {
            expanded += pattern[i];
        }
    }
    return expanded;
}

} // namespace

RulesValidator::Resolution RulesValidator::resolveTarget(const std::string& path,
                                                         std::string& repository,
                                                         std::string& branch) const
{
    for (const MatchRule& rule : m_matchRules) {
        if (!validateRegex(rule.pattern))
            continue; // invalid patterns already reported by validate()
        const std::regex re(rule.pattern, std::regex::ECMAScript);
        std::smatch match;
        if (!std::regex_search(path, match, re, std::regex_constants::match_continuous))
            continue;
        if (rule.repository.empty())
            return Resolution::Ignored;
        repository = expandBackreferences(rule.repository, match);
        branch = rule.branch.empty() ? std::string("master")
                                     : expandBackreferences(rule.branch, match);
        return Resolution::Mapped;
    }
    return Resolution::Unmapped;
}

void RulesValidator::interactiveDebug(std::istream& in, std::ostream& out)
{
    auto log = logging::get("rules-validator");
    RulesValidationResult result;
    if (!parse(result) || !result.errors.empty()) {
        // Debugging against a broken rule set would produce misleading
        // match results — refuse until the file parses cleanly.
        out << "rules file has errors — fix them before debugging:\n";
        for (const std::string& error : result.errors)
            out << "  " << error << '\n';
        return;
    }

    out << "svn2git rule debugger — enter SVN paths (empty line or EOF quits)\n";
    std::string path;
    while (out << "> " << std::flush, std::getline(in, path)) {
        path = trim(path);
        if (path.empty())
            break;
        const int index = firstMatch(path);
        if (index < 0) {
            out << "  no rule matches '" << path << "'\n";
        } else {
            const MatchRule& rule = m_matchRules[static_cast<std::size_t>(index)];
            out << "  matched rule at line " << rule.lineNumber << " (pattern '"
                << rule.pattern << "')";
            if (rule.repository.empty())
                out << " -> IGNORED\n";
            else
                out << " -> repository '" << rule.repository << "' branch '"
                    << (rule.branch.empty() ? std::string("<default>") : rule.branch)
                    << "'\n";
        }
        log->debug("debug query '{}' -> rule index {}", path, index);
    }
    out << "bye\n";
}

} // namespace svn2git
