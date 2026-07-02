/*
 *  svn2git enhanced — post-migration Git validation implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/git_validator.h"

#include "svn2git/logging.h"

#include <algorithm>
#include <cctype>
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

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        if (!line.empty())
            lines.push_back(line);
    }
    return lines;
}

} // namespace

std::string GitReport::toText() const
{
    std::ostringstream out;
    out << "POST-MIGRATION GIT REPORT — " << (ok ? "OK" : "INCOMPLETE") << '\n'
        << "  Total commits   : " << totalCommits << '\n'
        << "  Distinct authors: " << authorCount << '\n'
        << "  Branches (" << branches.size() << "): ";
    for (std::size_t i = 0; i < branches.size(); ++i)
        out << (i != 0 ? ", " : "") << branches[i];
    out << "\n  Tags (" << tags.size() << "): ";
    for (std::size_t i = 0; i < tags.size(); ++i)
        out << (i != 0 ? ", " : "") << tags[i];
    out << '\n';
    return out.str();
}

GitValidator::GitValidator(std::string gitRepoPath, ErrorReporter& reporter,
                           Runner runner)
    : m_gitRepoPath(std::move(gitRepoPath))
    , m_reporter(reporter)
    , m_runner(std::move(runner))
{
}

CommandResult GitValidator::git(const std::string& arguments) const
{
    return m_runner("git -C " + CommandRunner::shellQuote(m_gitRepoPath) + " "
                    + arguments);
}

GitReport GitValidator::generatePostMigrationReport()
{
    auto log = logging::get("git-validator");
    log->info("generating post-migration report for '{}'", m_gitRepoPath);

    GitReport report;

    // Commit count across every ref (branches and tags).
    {
        const CommandResult result = git("rev-list --all --count");
        if (!result.ok()) {
            m_reporter.report(ErrorCode::ExternalToolError,
                              "git rev-list failed (exit "
                                  + std::to_string(result.exitCode)
                                  + "): " + trim(result.output).substr(0, 300),
                              m_gitRepoPath);
            return report;
        }
        try {
            report.totalCommits = std::stol(trim(result.output));
        } catch (const std::exception&) {
            m_reporter.report(ErrorCode::ExternalToolError,
                              "unparsable commit count '" + trim(result.output) + "'",
                              m_gitRepoPath);
            return report;
        }
    }

    // Branch inventory.
    {
        const CommandResult result
            = git("for-each-ref --format='%(refname:short)' refs/heads");
        if (!result.ok()) {
            m_reporter.report(ErrorCode::ExternalToolError,
                              "git for-each-ref (branches) failed", m_gitRepoPath);
            return report;
        }
        report.branches = splitLines(result.output);
        // Strip the quotes the --format may leave depending on shell.
        for (std::string& branch : report.branches) {
            branch.erase(std::remove(branch.begin(), branch.end(), '\''), branch.end());
        }
        std::sort(report.branches.begin(), report.branches.end());
    }

    // Tag inventory.
    {
        const CommandResult result = git("tag --list");
        if (!result.ok()) {
            m_reporter.report(ErrorCode::ExternalToolError, "git tag --list failed",
                              m_gitRepoPath);
            return report;
        }
        report.tags = splitLines(result.output);
        std::sort(report.tags.begin(), report.tags.end());
    }

    // Distinct authors across all refs.
    {
        const CommandResult result = git("log --all --format=%an");
        if (!result.ok()) {
            m_reporter.report(ErrorCode::ExternalToolError, "git log (authors) failed",
                              m_gitRepoPath);
            return report;
        }
        const std::vector<std::string> lines = splitLines(result.output);
        const std::set<std::string> authors(lines.begin(), lines.end());
        report.authorCount = static_cast<long>(authors.size());
    }

    report.ok = true;
    log->info("post-migration report: {} commit(s), {} author(s), "
              "{} branch(es), {} tag(s)",
              report.totalCommits, report.authorCount, report.branches.size(),
              report.tags.size());
    return report;
}

GitValidationStatus GitValidator::validateAgainstExpected(const SVNReport& expected)
{
    auto log = logging::get("git-validator");
    log->info("validating '{}' against pre-migration expectations", m_gitRepoPath);

    GitValidationStatus status;
    const GitReport actual = generatePostMigrationReport();
    if (!actual.ok) {
        status.discrepancies.push_back(
            "could not analyze the converted repository — see error report");
        return status;
    }

    // Branch inventory: every SVN branch must exist in Git. Exact name
    // equality is expected because the rules map branches 1:1 by default;
    // 'trunk' conventionally becomes 'main' or 'master', so accept those.
    for (const std::string& branch : expected.branches) {
        const auto matches = [&](const std::string& name) {
            if (name == branch)
                return true;
            return branch == "trunk"
                && (name == "main" || name == "master" || name == "trunk");
        };
        if (std::none_of(actual.branches.begin(), actual.branches.end(), matches))
            status.discrepancies.push_back("SVN branch '" + branch
                                           + "' has no counterpart in Git");
    }

    for (const std::string& tag : expected.tags) {
        // Tags may be converted as annotated tags or as refs/heads
        // depending on the rules; check both inventories.
        const bool inTags
            = std::find(actual.tags.begin(), actual.tags.end(), tag) != actual.tags.end();
        const bool inBranches
            = std::find(actual.branches.begin(), actual.branches.end(), tag)
            != actual.branches.end();
        if (!inTags && !inBranches)
            status.discrepancies.push_back("SVN tag '" + tag
                                           + "' has no counterpart in Git");
    }

    // Commit count plausibility: an empty result is always wrong when the
    // source had revisions. More Git commits than SVN revisions is NOT a
    // defect — one SVN revision touching several branches legitimately
    // produces one Git commit per branch (see src/repository.cpp) — so
    // that case is only logged for the migration dossier.
    if (expected.totalRevisions > 0 && actual.totalCommits == 0)
        status.discrepancies.push_back(
            "converted repository has no commits while SVN had "
            + std::to_string(expected.totalRevisions) + " revision(s)");
    if (actual.totalCommits > expected.totalRevisions)
        log->info("converted repository has more commits ({}) than SVN revisions ({}) — "
                  "expected when revisions touch multiple branches",
                  actual.totalCommits, expected.totalRevisions);

    if (actual.authorCount > expected.authorCount)
        status.discrepancies.push_back(
            "converted repository has more authors (" + std::to_string(actual.authorCount)
            + ") than SVN (" + std::to_string(expected.authorCount) + ")");

    for (const std::string& discrepancy : status.discrepancies)
        m_reporter.report(ErrorCode::CommitCountMismatch, discrepancy, m_gitRepoPath);

    status.passed = status.discrepancies.empty();
    log->info("expectation validation: {} discrepancy(ies) — {}",
              status.discrepancies.size(), status.passed ? "PASS" : "FAIL");
    return status;
}

bool GitValidator::validateRepositoryIntegrity()
{
    auto log = logging::get("git-validator");
    log->info("running integrity checks on '{}'", m_gitRepoPath);

    const CommandResult fsck = git("fsck --full --strict");
    if (!fsck.ok()) {
        m_reporter.report(ErrorCode::ExternalToolError,
                          "git fsck reported corruption (exit "
                              + std::to_string(fsck.exitCode)
                              + "): " + trim(fsck.output).substr(0, 300),
                          m_gitRepoPath);
        return false;
    }

    // Dangling commits indicate history that is not reachable from any
    // ref — in a fresh conversion that means lost commits.
    bool clean = true;
    for (const std::string& line : splitLines(fsck.output)) {
        if (line.rfind("dangling commit", 0) == 0) {
            m_reporter.report(ErrorCode::StructuralRevisionError,
                              "orphaned commit after conversion: " + line, m_gitRepoPath);
            clean = false;
        }
    }

    log->info("integrity check: {}", clean ? "PASS" : "FAIL");
    return clean;
}

} // namespace svn2git
