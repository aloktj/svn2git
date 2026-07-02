/*
 *  svn2git enhanced — pre-migration SVN analysis implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/svn_validator.h"

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

} // namespace

std::string SVNReport::toText() const
{
    std::ostringstream out;
    out << "PRE-MIGRATION SVN REPORT — " << (ok ? "OK" : "INCOMPLETE") << '\n'
        << "  Total revisions : " << totalRevisions << '\n'
        << "  Distinct authors: " << authorCount << '\n'
        << "  Repository size : " << repositorySizeMB << " MB\n"
        << "  Branches (" << branches.size() << "):\n";
    for (const std::string& branch : branches) {
        const auto count = commitsByBranch.find(branch);
        out << "    " << branch << " — "
            << (count != commitsByBranch.end() ? count->second : 0) << " commit(s)\n";
    }
    out << "  Tags (" << tags.size() << "):\n";
    for (const std::string& tag : tags) {
        const auto count = commitsByTag.find(tag);
        out << "    " << tag << " — " << (count != commitsByTag.end() ? count->second : 0)
            << " commit(s)\n";
    }
    if (!largestFiles.empty()) {
        out << "  Largest files:\n";
        for (const SvnFileInfo& info : largestFiles)
            out << "    " << info.path << " (" << info.sizeBytes << " bytes)\n";
    }
    return out.str();
}

SVNValidator::SVNValidator(std::string svnRepoUrl, ErrorReporter& reporter, Runner runner)
    : m_svnRepoUrl(std::move(svnRepoUrl))
    , m_reporter(reporter)
    , m_runner(std::move(runner))
{
}

std::vector<std::string> SVNValidator::listChildren(const std::string& directory)
{
    const std::string command = "svn list --non-interactive "
        + CommandRunner::shellQuote(m_svnRepoUrl + "/" + directory);
    const CommandResult result = m_runner(command);
    if (!result.ok()) {
        // A missing branches/ or tags/ directory is a layout property,
        // not an error — log at debug level and return empty.
        logging::get("svn-validator")
            ->debug("'{}' listing unavailable (exit {})", directory, result.exitCode);
        return {};
    }

    std::vector<std::string> names;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        // Directories are listed with a trailing slash.
        if (line.size() > 1 && line.back() == '/')
            names.push_back(line.substr(0, line.size() - 1));
    }
    std::sort(names.begin(), names.end());
    return names;
}

long SVNValidator::countCommitsOnPath(const std::string& path)
{
    const std::string url = path.empty() ? m_svnRepoUrl : (m_svnRepoUrl + "/" + path);
    const std::string command
        = "svn log --quiet --non-interactive " + CommandRunner::shellQuote(url);
    const CommandResult result = m_runner(command);
    if (!result.ok()) {
        m_reporter.report(ErrorCode::ExternalToolError,
                          "svn log failed for path '" + path + "' (exit "
                              + std::to_string(result.exitCode) + ")",
                          url);
        return -1;
    }

    long count = 0;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.size() >= 2 && line[0] == 'r'
            && std::isdigit(static_cast<unsigned char>(line[1])) != 0)
            ++count;
    }
    return count;
}

std::map<std::string, long> SVNValidator::countCommitsByBranch()
{
    auto log = logging::get("svn-validator");
    std::map<std::string, long> counts;

    // trunk is treated as a branch for accounting purposes.
    const long trunkCommits = countCommitsOnPath("trunk");
    if (trunkCommits >= 0)
        counts["trunk"] = trunkCommits;

    for (const std::string& branch : listChildren("branches")) {
        const long commits = countCommitsOnPath("branches/" + branch);
        if (commits >= 0)
            counts[branch] = commits;
    }

    log->info("counted commits across {} branch(es)", counts.size());
    return counts;
}

std::map<std::string, long> SVNValidator::countCommitsByTag()
{
    auto log = logging::get("svn-validator");
    std::map<std::string, long> counts;
    for (const std::string& tag : listChildren("tags")) {
        const long commits = countCommitsOnPath("tags/" + tag);
        if (commits >= 0)
            counts[tag] = commits;
    }
    log->info("counted commits across {} tag(s)", counts.size());
    return counts;
}

double SVNValidator::calculateRepositorySize()
{
    auto log = logging::get("svn-validator");

    // `svn list -R -v` prints: "<rev> <author> <size> <date> <path>"
    // for files; directories have no size column and end in '/'.
    const std::string command = "svn list --recursive --verbose --non-interactive "
        + CommandRunner::shellQuote(m_svnRepoUrl);
    const CommandResult result = m_runner(command);
    if (!result.ok()) {
        m_reporter.report(ErrorCode::ExternalToolError,
                          "svn list -R failed (exit " + std::to_string(result.exitCode)
                              + ")",
                          m_svnRepoUrl);
        return -1.0;
    }

    long long totalBytes = 0;
    m_largestFiles.clear();

    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty() || line.back() == '/')
            continue; // directory entry

        // Tokenize: rev, author, size, then date tokens, path last.
        std::istringstream fields(line);
        std::string revision, author, sizeText;
        if (!(fields >> revision >> author >> sizeText))
            continue;
        if (!std::all_of(sizeText.begin(), sizeText.end(),
                         [](unsigned char c) { return std::isdigit(c) != 0; }))
            continue; // locked/atypical listing — skip defensively

        long long sizeBytes = 0;
        try {
            sizeBytes = std::stoll(sizeText);
        } catch (const std::exception&) {
            continue;
        }
        totalBytes += sizeBytes;

        // Path = last whitespace-separated token of the line. SVN paths
        // with spaces are rare; this heuristic keeps parsing dependency-free.
        const std::size_t lastSpace = line.find_last_of(" \t");
        const std::string path
            = (lastSpace == std::string::npos) ? line : line.substr(lastSpace + 1);

        m_largestFiles.push_back(SvnFileInfo {path, sizeBytes});
        std::sort(m_largestFiles.begin(), m_largestFiles.end(),
                  [](const SvnFileInfo& a, const SvnFileInfo& b) {
                      return a.sizeBytes > b.sizeBytes;
                  });
        if (m_largestFiles.size() > 10)
            m_largestFiles.resize(10);
    }

    const double sizeMB = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
    log->info("repository size at HEAD: {:.2f} MB ({} bytes)", sizeMB, totalBytes);
    return sizeMB;
}

SVNReport SVNValidator::generatePreMigrationReport()
{
    auto log = logging::get("svn-validator");
    log->info("generating pre-migration report for '{}'", m_svnRepoUrl);

    SVNReport report;

    // Total revisions: parse "Revision: N" from `svn info`.
    {
        const std::string command
            = "svn info --non-interactive " + CommandRunner::shellQuote(m_svnRepoUrl);
        const CommandResult result = m_runner(command);
        if (!result.ok()) {
            m_reporter.report(ErrorCode::ExternalToolError,
                              "svn info failed (exit " + std::to_string(result.exitCode)
                                  + "): " + trim(result.output).substr(0, 300),
                              m_svnRepoUrl);
            return report; // repository unreachable — nothing else can work
        }
        std::istringstream stream(result.output);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.rfind("Revision:", 0) == 0) {
                try {
                    report.totalRevisions = std::stol(trim(line.substr(9)));
                } catch (const std::exception&) {
                    report.totalRevisions = 0;
                }
                break;
            }
        }
    }

    // Distinct authors via `svn log --quiet` over the whole history.
    {
        const std::string command = "svn log --quiet --non-interactive "
            + CommandRunner::shellQuote(m_svnRepoUrl);
        const CommandResult result = m_runner(command);
        if (!result.ok()) {
            m_reporter.report(ErrorCode::ExternalToolError,
                              "svn log failed while counting authors (exit "
                                  + std::to_string(result.exitCode) + ")",
                              m_svnRepoUrl);
            return report;
        }
        std::set<std::string> authors;
        std::istringstream stream(result.output);
        std::string line;
        while (std::getline(stream, line)) {
            if (line.size() < 2 || line[0] != 'r'
                || std::isdigit(static_cast<unsigned char>(line[1])) == 0)
                continue;
            const std::size_t firstPipe = line.find('|');
            const std::size_t secondPipe = (firstPipe == std::string::npos)
                ? std::string::npos
                : line.find('|', firstPipe + 1);
            if (secondPipe == std::string::npos)
                continue;
            const std::string author
                = trim(line.substr(firstPipe + 1, secondPipe - firstPipe - 1));
            if (!author.empty())
                authors.insert(author);
        }
        report.authorCount = static_cast<long>(authors.size());
    }

    report.commitsByBranch = countCommitsByBranch();
    report.commitsByTag = countCommitsByTag();
    for (const auto& [branch, count] : report.commitsByBranch) {
        (void)count;
        report.branches.push_back(branch);
    }
    for (const auto& [tag, count] : report.commitsByTag) {
        (void)count;
        report.tags.push_back(tag);
    }

    const double sizeMB = calculateRepositorySize();
    report.repositorySizeMB = (sizeMB < 0.0) ? 0.0 : sizeMB;
    report.largestFiles = m_largestFiles;

    report.ok = report.totalRevisions > 0 && report.authorCount > 0;
    log->info("pre-migration report: {} revision(s), {} author(s), "
              "{} branch(es), {} tag(s) — {}",
              report.totalRevisions, report.authorCount, report.branches.size(),
              report.tags.size(), report.ok ? "OK" : "INCOMPLETE");
    return report;
}

} // namespace svn2git
