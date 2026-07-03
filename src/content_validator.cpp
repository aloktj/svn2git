/*
 *  svn2git enhanced — post-migration file content validation implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/content_validator.h"

#include "svn2git/logging.h"

#include <unistd.h> // getpid

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
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

/// Append up to @p limit items of @p paths to @p out, then an ellipsis.
std::string previewList(const std::vector<std::string>& paths, std::size_t limit = 10)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < paths.size() && i < limit; ++i)
        out << (i != 0 ? ", " : "") << paths[i];
    if (paths.size() > limit)
        out << ", … (" << paths.size() - limit << " more)";
    return out.str();
}

} // namespace

std::string ContentReport::toText() const
{
    std::ostringstream out;
    out << "POST-MIGRATION CONTENT VALIDATION — " << (ok ? "PASS" : "FAIL") << '\n';
    for (const RefContentResult& ref : refs) {
        out << "  " << ref.svnPath << " -> " << ref.gitRef << " : ";
        if (ref.refMissing) {
            out << "GIT REF MISSING (branch/tag not converted)\n";
            continue;
        }
        out << ref.filesInSvn << " file(s), " << ref.filesHashed << " hashed — "
            << (ref.ok() ? "OK" : "FAIL") << '\n';
        if (!ref.missingInGit.empty()) {
            out << "    missing in git (" << ref.missingInGit.size() << "):\n";
            for (const std::string& file : ref.missingInGit)
                out << "      " << file << '\n';
        }
        if (!ref.contentMismatches.empty()) {
            out << "    content mismatch (" << ref.contentMismatches.size() << "):\n";
            for (const std::string& file : ref.contentMismatches)
                out << "      " << file << '\n';
        }
        if (!ref.toolFailures.empty()) {
            out << "    tool failure (" << ref.toolFailures.size() << "):\n";
            for (const std::string& detail : ref.toolFailures)
                out << "      " << detail << '\n';
        }
    }
    return out.str();
}

ContentValidator::ContentValidator(std::string svnRepoUrl, std::string gitRepoPath,
                                   ErrorReporter& reporter, Runner runner)
    : m_svnRepoUrl(std::move(svnRepoUrl))
    , m_gitRepoPath(std::move(gitRepoPath))
    , m_scratchFile((std::filesystem::temp_directory_path()
                     / ("svn2git-content-" + std::to_string(getpid()) + ".tmp"))
                        .string())
    , m_reporter(reporter)
    , m_runner(std::move(runner))
{
}

std::vector<RefMapping>
ContentValidator::standardLayoutMappings(const std::vector<std::string>& branches,
                                         const std::vector<std::string>& tags)
{
    std::vector<RefMapping> mappings;
    for (const std::string& branch : branches) {
        if (branch == "trunk")
            mappings.push_back(RefMapping {"trunk", "master"});
        else
            mappings.push_back(RefMapping {"branches/" + branch, branch});
    }
    for (const std::string& tag : tags)
        mappings.push_back(RefMapping {"tags/" + tag, "tags/" + tag});
    return mappings;
}

std::vector<RefMapping> ContentValidator::mapWithRules(
    const RulesValidator& rules, const std::vector<std::string>& branches,
    const std::vector<std::string>& tags, std::vector<std::string>& unmapped,
    std::vector<std::string>& ignored)
{
    std::vector<RefMapping> mappings;

    const auto resolve = [&](const std::string& svnPath) {
        // Rule patterns anchor at absolute directory paths ("/trunk/…").
        const std::string anchored = "/" + svnPath + "/";
        std::string repository;
        std::string branch;
        switch (rules.resolveTarget(anchored, repository, branch)) {
        case RulesValidator::Resolution::Mapped:
            mappings.push_back(RefMapping {svnPath, branch});
            break;
        case RulesValidator::Resolution::Ignored:
            ignored.push_back(svnPath);
            break;
        case RulesValidator::Resolution::Unmapped:
            unmapped.push_back(svnPath);
            break;
        }
    };

    for (const std::string& branch : branches)
        resolve(branch == "trunk" ? branch : "branches/" + branch);
    for (const std::string& tag : tags)
        resolve("tags/" + tag);
    return mappings;
}

bool ContentValidator::listSvnFiles(const std::string& svnPath,
                                    std::vector<std::string>& files)
{
    const std::string command = "svn list --recursive --non-interactive "
        + CommandRunner::shellQuote(m_svnRepoUrl + "/" + svnPath);
    const CommandResult result = m_runner(command);
    if (!result.ok())
        return false;

    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        line = trim(line);
        // Directories are listed with a trailing slash — only files carry
        // content to compare.
        if (!line.empty() && line.back() != '/')
            files.push_back(line);
    }
    std::sort(files.begin(), files.end());
    return true;
}

bool ContentValidator::listGitBlobs(
    const std::string& gitRef, std::vector<std::pair<std::string, std::string>>& blobs)
{
    // NUL-separated output so paths with spaces or metacharacters parse
    // exactly (git quotes such paths in the default line format).
    const std::string command = "git -C " + CommandRunner::shellQuote(m_gitRepoPath)
        + " ls-tree -r -z " + CommandRunner::shellQuote(gitRef);
    const CommandResult result = m_runner(command);
    if (!result.ok())
        return false;

    std::istringstream stream(result.output);
    std::string entry;
    while (std::getline(stream, entry, '\0')) {
        // Entry format: "<mode> <type> <sha>\t<path>"
        const std::size_t tab = entry.find('\t');
        if (tab == std::string::npos)
            continue;
        const std::string path = entry.substr(tab + 1);
        std::istringstream header(entry.substr(0, tab));
        std::string mode, type, sha;
        if (!(header >> mode >> type >> sha))
            continue;
        if (type == "blob")
            blobs.emplace_back(path, sha);
    }
    std::sort(blobs.begin(), blobs.end());
    return true;
}

std::string ContentValidator::svnContentHash(const std::string& svnPath,
                                             const std::string& file)
{
    // Two steps via a scratch file instead of a pipe: with a pipe the
    // exit status of `svn cat` is lost and a partial download would be
    // hashed silently, turning a transport error into a bogus mismatch.
    const std::string url = m_svnRepoUrl + "/" + svnPath + "/" + file;
    const CommandResult fetch
        = m_runner("svn cat --non-interactive " + CommandRunner::shellQuote(url) + " > "
                   + CommandRunner::shellQuote(m_scratchFile));
    if (!fetch.ok())
        return std::string();

    const CommandResult hash
        = m_runner("git -C " + CommandRunner::shellQuote(m_gitRepoPath) + " hash-object "
                   + CommandRunner::shellQuote(m_scratchFile));
    if (!hash.ok())
        return std::string();
    return trim(hash.output);
}

ContentReport ContentValidator::verify(const std::vector<RefMapping>& mappings,
                                       long sampleLimit)
{
    auto log = logging::get("content-validator");
    log->info("verifying file contents of '{}' against '{}' across {} ref(s)",
              m_gitRepoPath, m_svnRepoUrl, mappings.size());

    ContentReport report;
    report.ok = true;

    for (const RefMapping& mapping : mappings) {
        RefContentResult ref;
        ref.svnPath = mapping.svnPath;
        ref.gitRef = mapping.gitRef;

        std::vector<std::string> svnFiles;
        if (!listSvnFiles(mapping.svnPath, svnFiles)) {
            ref.toolFailures.push_back("svn list failed for '" + mapping.svnPath + "'");
            m_reporter.report(ErrorCode::ExternalToolError,
                              "svn list failed while validating content of '"
                                  + mapping.svnPath + "'",
                              m_svnRepoUrl + "/" + mapping.svnPath);
            report.refs.push_back(std::move(ref));
            report.ok = false;
            continue;
        }
        ref.filesInSvn = static_cast<long>(svnFiles.size());

        std::vector<std::pair<std::string, std::string>> gitBlobs;
        if (!listGitBlobs(mapping.gitRef, gitBlobs)) {
            ref.refMissing = true;
            m_reporter.report(ErrorCode::FileMissingInGit,
                              "branch/tag '" + mapping.svnPath
                                  + "' has no corresponding git ref '" + mapping.gitRef
                                  + "' in the converted repository",
                              m_gitRepoPath);
            report.refs.push_back(std::move(ref));
            report.ok = false;
            continue;
        }
        std::map<std::string, std::string> blobByPath(gitBlobs.begin(), gitBlobs.end());

        // 1. Complete inventory comparison — every SVN file must exist.
        std::vector<std::string> present;
        for (const std::string& file : svnFiles) {
            if (blobByPath.count(file) != 0)
                present.push_back(file);
            else
                ref.missingInGit.push_back(file);
        }
        if (!ref.missingInGit.empty())
            m_reporter.report(ErrorCode::FileMissingInGit,
                              std::to_string(ref.missingInGit.size()) + " file(s) of '"
                                  + mapping.svnPath + "' missing from git ref '"
                                  + mapping.gitRef
                                  + "': " + previewList(ref.missingInGit),
                              m_gitRepoPath);

        // 2. Exact content comparison, evenly sampled when capped.
        std::vector<std::string> toHash;
        if (sampleLimit > 0 && present.size() > static_cast<std::size_t>(sampleLimit)) {
            for (long i = 0; i < sampleLimit; ++i)
                toHash.push_back(present[static_cast<std::size_t>(i) * present.size()
                                         / static_cast<std::size_t>(sampleLimit)]);
        } else {
            toHash = present;
        }

        for (const std::string& file : toHash) {
            const std::string svnHash = svnContentHash(mapping.svnPath, file);
            if (svnHash.empty()) {
                ref.toolFailures.push_back("cannot hash SVN content of '" + file + "'");
                m_reporter.report(ErrorCode::ExternalToolError,
                                  "cannot hash SVN content of '" + mapping.svnPath + "/"
                                      + file + "'",
                                  m_svnRepoUrl);
                continue;
            }
            ++ref.filesHashed;
            if (svnHash != blobByPath[file])
                ref.contentMismatches.push_back(file);
        }
        if (!ref.contentMismatches.empty())
            m_reporter.report(ErrorCode::ContentMismatch,
                              std::to_string(ref.contentMismatches.size())
                                  + " file(s) of '" + mapping.svnPath
                                  + "' differ from git ref '" + mapping.gitRef
                                  + "': " + previewList(ref.contentMismatches),
                              m_gitRepoPath);

        log->info("ref '{}' -> '{}': {} file(s), {} hashed, {} missing, {} mismatched",
                  mapping.svnPath, mapping.gitRef, ref.filesInSvn, ref.filesHashed,
                  ref.missingInGit.size(), ref.contentMismatches.size());
        if (!ref.ok())
            report.ok = false;
        report.refs.push_back(std::move(ref));
    }

    std::remove(m_scratchFile.c_str());
    log->info("content validation {}", report.ok ? "PASSED" : "FAILED");
    return report;
}

} // namespace svn2git
