/*
 *  svn2git enhanced — validation & compliance CLI entry point
 *
 *  Command-line front end for the Phase 1/2 tooling that surrounds the
 *  classic svn-all-fast-export converter:
 *
 *    svn2git-validate [options] <svn-repository-url>
 *
 *  Options:
 *    --authors <file>              authors.txt mapping file (default authors.txt)
 *    --rules <file>                .rules file (default svn2git.rules)
 *    --orchestration <file>        optional orchestration YAML to validate
 *    --validate-authors-only       check author coverage and exit
 *    --auto-map-authors            write authors-generated.txt with placeholders
 *    --dry-run                     validate configs and preview rule matching
 *    --debug-rules                 interactive rule debugger (reads stdin)
 *    --debug                       verbose (debug-level) logging
 *    --log-file <file>             persistent log file (default svn2git.log)
 *    --generate-traceability-map   demo-generate mapping JSON + SQLite from
 *                                  an existing converted repo (see --git-repo)
 *    --git-repo <path>             converted Git repository for post-checks
 *    --push-gitlab <remote-url>    push the converted repository to GitLab
 *    --operator <id>               operator identity for the audit trail
 *    --help                        print usage
 *
 *  Exit codes: 0 success, 1 validation failure, 2 usage error.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/audit_logger.h"
#include "svn2git/author_validator.h"
#include "svn2git/command_runner.h"
#include "svn2git/config_validator.h"
#include "svn2git/error_reporter.h"
#include "svn2git/git_validator.h"
#include "svn2git/logging.h"
#include "svn2git/revision_mapper.h"
#include "svn2git/rules_validator.h"
#include "svn2git/svn_validator.h"

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kExitSuccess = 0;
constexpr int kExitValidationFailure = 1;
constexpr int kExitUsageError = 2;

/// Parsed command-line configuration.
struct Options {
    std::string svnUrl;
    std::string authorsFile = "authors.txt";
    std::string rulesFile = "svn2git.rules";
    std::string orchestrationFile;
    std::string logFile = "svn2git.log";
    std::string gitRepoPath;
    std::string gitlabRemote;
    std::string operatorId;
    bool validateAuthorsOnly = false;
    bool autoMapAuthors = false;
    bool dryRun = false;
    bool debugRules = false;
    bool debug = false;
    bool generateTraceabilityMap = false;
    bool showHelp = false;
};

void printUsage(std::ostream& out)
{
    out << "usage: svn2git-validate [options] <svn-repository-url>\n"
           "\n"
           "Pre/post-migration validation and EN 50128 compliance tooling.\n"
           "\n"
           "options:\n"
           "  --authors <file>             authors mapping file (default: authors.txt)\n"
           "  --rules <file>               rules file (default: svn2git.rules)\n"
           "  --orchestration <file>       orchestration YAML to validate\n"
           "  --validate-authors-only      check author coverage and exit\n"
           "  --auto-map-authors           generate authors-generated.txt placeholders\n"
           "  --dry-run                    validate configuration and preview rules\n"
           "  --debug-rules                interactive rule debugger\n"
           "  --debug                      verbose logging\n"
           "  --log-file <file>            persistent log file (default: svn2git.log)\n"
           "  --generate-traceability-map  build mapping JSON + SQLite from --git-repo\n"
           "  --git-repo <path>            converted Git repository for post-checks\n"
           "  --push-gitlab <remote-url>   push converted repository to GitLab\n"
           "  --operator <id>              operator identity for the audit trail\n"
           "  --help                       show this help\n";
}

/// Parse argv. Returns false (with a message on stderr) on bad usage.
bool parseArguments(int argc, char** argv, Options& options)
{
    const auto needsValue = [&](int& i, const char* flag) -> const char* {
        if (i + 1 >= argc) {
            std::cerr << "error: " << flag << " requires a value\n";
            return nullptr;
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        } else if (arg == "--authors") {
            const char* value = needsValue(i, "--authors");
            if (value == nullptr)
                return false;
            options.authorsFile = value;
        } else if (arg == "--rules") {
            const char* value = needsValue(i, "--rules");
            if (value == nullptr)
                return false;
            options.rulesFile = value;
        } else if (arg == "--orchestration") {
            const char* value = needsValue(i, "--orchestration");
            if (value == nullptr)
                return false;
            options.orchestrationFile = value;
        } else if (arg == "--log-file") {
            const char* value = needsValue(i, "--log-file");
            if (value == nullptr)
                return false;
            options.logFile = value;
        } else if (arg == "--git-repo") {
            const char* value = needsValue(i, "--git-repo");
            if (value == nullptr)
                return false;
            options.gitRepoPath = value;
        } else if (arg == "--push-gitlab") {
            const char* value = needsValue(i, "--push-gitlab");
            if (value == nullptr)
                return false;
            options.gitlabRemote = value;
        } else if (arg == "--operator") {
            const char* value = needsValue(i, "--operator");
            if (value == nullptr)
                return false;
            options.operatorId = value;
        } else if (arg == "--validate-authors-only") {
            options.validateAuthorsOnly = true;
        } else if (arg == "--auto-map-authors") {
            options.autoMapAuthors = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--debug-rules") {
            options.debugRules = true;
        } else if (arg == "--debug") {
            options.debug = true;
        } else if (arg == "--generate-traceability-map") {
            options.generateTraceabilityMap = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return false;
        } else if (options.svnUrl.empty()) {
            options.svnUrl = arg;
        } else {
            std::cerr << "error: unexpected extra argument '" << arg << "'\n";
            return false;
        }
    }
    return true;
}

/// Collect representative SVN paths for the dry run: the changed paths
/// of recent revisions, capped at @p limit.
std::vector<std::string> collectSamplePaths(const std::string& svnUrl, std::size_t limit)
{
    std::vector<std::string> paths;
    const svn2git::CommandResult result
        = svn2git::CommandRunner::run("svn log --verbose --limit 50 --non-interactive "
                                      + svn2git::CommandRunner::shellQuote(svnUrl));
    if (!result.ok())
        return paths;

    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line) && paths.size() < limit) {
        // Changed-path lines look like "   M /trunk/src/main.cpp".
        const std::size_t start = line.find_first_not_of(' ');
        if (start == std::string::npos || start == 0)
            continue;
        if (line.size() < start + 3 || line[start + 1] != ' ')
            continue;
        const char action = line[start];
        if (action != 'A' && action != 'M' && action != 'D' && action != 'R')
            continue;
        std::string path = line.substr(start + 2);
        // Copy-from annotations are appended in parentheses — strip them.
        const std::size_t paren = path.find(" (from ");
        if (paren != std::string::npos)
            path.erase(paren);
        // Rule matching operates on absolute directory-style paths
        // ("/trunk/…"), mirroring the pattern anchors in the rules files.
        if (!path.empty() && path.back() != '/')
            path += '/';
        if (!path.empty() && path.front() != '/')
            path.insert(path.begin(), '/');
        paths.push_back(path);
    }
    return paths;
}

/// Push the converted repository to a GitLab remote (all refs + tags).
bool pushToGitLab(const std::string& gitRepoPath, const std::string& remoteUrl,
                  svn2git::ErrorReporter& reporter, svn2git::AuditLogger& audit)
{
    auto log = svn2git::logging::get("gitlab-push");
    log->info("pushing '{}' to '{}'", gitRepoPath, remoteUrl);
    audit.logEvent("Push", "pushing all refs to " + remoteUrl, "");

    const std::string base = "git -C " + svn2git::CommandRunner::shellQuote(gitRepoPath);
    const svn2git::CommandResult branches = svn2git::CommandRunner::run(
        base + " push --all " + svn2git::CommandRunner::shellQuote(remoteUrl));
    if (!branches.ok()) {
        reporter.report(svn2git::ErrorCode::GitLabConnectionError,
                        "git push --all failed (exit " + std::to_string(branches.exitCode)
                            + "): " + branches.output.substr(0, 300),
                        remoteUrl);
        audit.logEvent("Push", "branch push to " + remoteUrl, "FAILED");
        return false;
    }

    const svn2git::CommandResult tags = svn2git::CommandRunner::run(
        base + " push --tags " + svn2git::CommandRunner::shellQuote(remoteUrl));
    if (!tags.ok()) {
        reporter.report(svn2git::ErrorCode::GitLabConnectionError,
                        "git push --tags failed (exit " + std::to_string(tags.exitCode)
                            + "): " + tags.output.substr(0, 300),
                        remoteUrl);
        audit.logEvent("Push", "tag push to " + remoteUrl, "FAILED");
        return false;
    }

    audit.logEvent("Push", "all refs pushed to " + remoteUrl, "OK");
    return true;
}

/// Extract the SVN revision from converter-embedded commit metadata:
/// git-svn writes a "git-svn-id: <url>@<rev> <uuid>" trailer, and
/// svn-all-fast-export --add-metadata appends "svn path=…; revision=<rev>".
/// @return the revision, or -1 when the body carries no metadata
long extractSvnRevision(const std::string& commitBody)
{
    static const std::regex kGitSvnId(R"(git-svn-id:\s*\S+@(\d+)\s)");
    static const std::regex kSvn2GitMetadata(R"(svn path=[^;]*;\s*revision=(\d+))");

    std::smatch match;
    if (std::regex_search(commitBody, match, kGitSvnId)
        || std::regex_search(commitBody, match, kSvn2GitMetadata)) {
        try {
            return std::stol(match[1].str());
        } catch (const std::exception&) {
            return -1; // overflow — treat as absent
        }
    }
    return -1;
}

/// One commit parsed from the git log record stream.
struct CommitRecord {
    std::string sha;
    std::string author;
    std::string timestamp;
    std::string subject;
    long svnRevision = -1; ///< real revision from metadata, -1 when absent
};

/// Build the traceability artifacts from an already-converted repository.
/// Real SVN revision numbers are taken from per-commit metadata
/// (git-svn-id / svn2git --add-metadata trailers). Only when NO commit
/// carries metadata does the tool fall back to sequential numbering —
/// mixing real and fabricated revisions would corrupt the audit map.
bool generateTraceability(const std::string& gitRepoPath,
                          const std::string& repositoryName,
                          svn2git::ErrorReporter& reporter)
{
    auto log = svn2git::logging::get("traceability");
    const std::string base = "git -C " + svn2git::CommandRunner::shellQuote(gitRepoPath);

    // Record-separated log (\x1e between commits, \x1f before the body)
    // so full commit messages — where the metadata trailers live — can be
    // parsed in a single git invocation.
    const svn2git::CommandResult result = svn2git::CommandRunner::run(
        base + " log --all --reverse --format='%x1e%H|%an|%aI%x1f%B'");
    if (!result.ok()) {
        reporter.report(svn2git::ErrorCode::ExternalToolError,
                        "git log failed while building traceability map", gitRepoPath);
        return false;
    }

    // Parse records (oldest first, thanks to --reverse).
    std::vector<CommitRecord> commits;
    long withMetadata = 0;
    {
        std::istringstream stream(result.output);
        std::string record;
        while (std::getline(stream, record, '\x1e')) {
            const std::size_t headerEnd = record.find('\x1f');
            if (headerEnd == std::string::npos)
                continue; // shell-quoting artifacts around the separators

            CommitRecord commit;
            std::istringstream header(record.substr(0, headerEnd));
            std::getline(header, commit.sha, '|');
            std::getline(header, commit.author, '|');
            std::getline(header, commit.timestamp);
            if (commit.sha.size() != 40)
                continue;

            const std::string body = record.substr(headerEnd + 1);
            commit.subject = body.substr(0, body.find('\n'));
            commit.svnRevision = extractSvnRevision(body);
            if (commit.svnRevision > 0)
                ++withMetadata;
            commits.push_back(std::move(commit));
        }
    }

    if (commits.empty()) {
        reporter.report(svn2git::ErrorCode::ExternalToolError,
                        "no commits found while building traceability map", gitRepoPath);
        return false;
    }

    svn2git::RevisionMapper mapper(repositoryName);
    const bool useMetadata = withMetadata == static_cast<long>(commits.size());
    if (useMetadata) {
        log->info("using embedded SVN revision metadata for all {} commit(s)",
                  commits.size());
        for (const CommitRecord& commit : commits)
            mapper.recordMapping(commit.svnRevision, commit.sha, commit.author,
                                 commit.timestamp, commit.subject);
    } else {
        // Partial metadata cannot be mixed with fabricated numbers; be
        // explicit that the fallback numbering is positional only.
        if (withMetadata > 0)
            reporter.report(
                svn2git::ErrorCode::StructuralRevisionError,
                std::to_string(commits.size() - withMetadata) + " of "
                    + std::to_string(commits.size())
                    + " commit(s) lack SVN revision metadata — falling back to "
                      "sequential numbering; re-convert with --add-metadata for "
                      "authoritative traceability",
                gitRepoPath);
        else
            log->warn("no SVN revision metadata found — using sequential "
                      "numbering (positions, not real revisions)");
        long sequence = 0;
        for (const CommitRecord& commit : commits)
            mapper.recordMapping(++sequence, commit.sha, commit.author, commit.timestamp,
                                 commit.subject);
    }

    const bool jsonOk = mapper.generateMappingFile("svn_to_git_mapping.json");
    const bool dbOk = mapper.generateMappingDatabase("traceability.db");
    log->info("traceability artifacts: JSON {}, SQLite {}", jsonOk ? "OK" : "FAILED",
              dbOk ? "OK" : "FAILED");
    return jsonOk && dbOk;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    if (!parseArguments(argc, argv, options)) {
        printUsage(std::cerr);
        return kExitUsageError;
    }
    if (options.showHelp) {
        printUsage(std::cout);
        return kExitSuccess;
    }

    // Rule debugging and traceability generation work without an SVN URL;
    // everything else requires one.
    const bool needsSvnUrl = !options.debugRules
        && !(options.generateTraceabilityMap && !options.gitRepoPath.empty());
    if (needsSvnUrl && options.svnUrl.empty()) {
        std::cerr << "error: missing <svn-repository-url>\n";
        printUsage(std::cerr);
        return kExitUsageError;
    }

    svn2git::logging::initialize(options.debug ? svn2git::logging::Level::Debug
                                               : svn2git::logging::Level::Info,
                                 options.logFile);
    auto log = svn2git::logging::get("svn2git");
    log->info("svn2git validation tool starting (url='{}')", options.svnUrl);

    svn2git::ErrorReporter reporter;

    // Derive a repository display name from the URL's last path segment.
    std::string repositoryName = options.svnUrl;
    while (!repositoryName.empty() && repositoryName.back() == '/')
        repositoryName.pop_back();
    const std::size_t lastSlash = repositoryName.find_last_of('/');
    if (lastSlash != std::string::npos)
        repositoryName.erase(0, lastSlash + 1);
    if (repositoryName.empty())
        repositoryName = "svn-migration";

    svn2git::AuditLogger audit(repositoryName, options.operatorId);
    bool success = true;

    if (options.debugRules) {
        svn2git::RulesValidator rules(options.rulesFile, reporter);
        rules.interactiveDebug(std::cin, std::cout);
        svn2git::logging::shutdown();
        return kExitSuccess;
    }

    if (options.autoMapAuthors) {
        svn2git::AuthorValidator authors(options.svnUrl, options.authorsFile, reporter);
        const std::string generated = authors.autoMapAuthors();
        if (generated.empty()) {
            success = false;
            audit.logEvent("Authors", "auto-mapping generation", "FAILED");
        } else {
            const std::string outputFile = "authors-generated.txt";
            std::ofstream out(outputFile, std::ios::trunc);
            out << generated;
            out.flush();
            if (!out.good()) {
                reporter.report(svn2git::ErrorCode::FileAccessError,
                                "cannot write generated authors file", outputFile);
                success = false;
            } else {
                std::cout << "generated author mapping written to " << outputFile
                          << " — review placeholder identities before migrating\n";
                audit.logEvent("Authors", "auto-mapping written to " + outputFile, "OK");
            }
        }
    }

    if (options.validateAuthorsOnly || options.dryRun) {
        svn2git::AuthorValidator authors(options.svnUrl, options.authorsFile, reporter);
        const svn2git::AuthorValidationReport report = authors.validateCoverage();
        audit.logEvent("Validation",
                       "author coverage (" + std::to_string(report.mappedAuthors) + "/"
                           + std::to_string(report.totalSvnAuthors) + " mapped)",
                       report.ok ? "OK" : "FAILED");
        for (const std::string& message : report.messages)
            std::cout << message << '\n';
        if (!report.ok)
            success = false;

        if (options.validateAuthorsOnly) {
            audit.setOutcome(success);
            audit.generateAuditTrail("audit.log");
            if (!success)
                reporter.generateReport();
            svn2git::logging::shutdown();
            return success ? kExitSuccess : kExitValidationFailure;
        }
    }

    if (options.dryRun) {
        svn2git::ConfigValidator config(options.authorsFile, options.rulesFile,
                                        options.orchestrationFile, reporter);
        const svn2git::ConfigReport configReport = config.validateAll();
        std::cout << configReport.toText();
        audit.logEvent("Validation", "pre-flight configuration",
                       configReport.ok ? "OK" : "FAILED");
        if (!configReport.ok)
            success = false;

        svn2git::RulesValidator rules(options.rulesFile, reporter);
        const std::vector<std::string> samplePaths
            = collectSamplePaths(options.svnUrl, 50);
        const svn2git::RulesValidationResult dryRunResult = rules.dryRun(samplePaths);
        std::cout << "\nDRY RUN — " << samplePaths.size()
                  << " sample path(s) from recent history:\n";
        for (const std::string& line : dryRunResult.dryRunLines)
            std::cout << "  " << line << '\n';
        for (const std::string& warning : dryRunResult.warnings)
            std::cout << "  warning: " << warning << '\n';
        audit.logEvent("Validation",
                       "rules dry-run over " + std::to_string(samplePaths.size())
                           + " path(s)",
                       dryRunResult.valid ? "OK" : "FAILED");
        if (!dryRunResult.valid)
            success = false;

        svn2git::SVNValidator svn(options.svnUrl, reporter);
        const svn2git::SVNReport svnReport = svn.generatePreMigrationReport();
        std::cout << '\n' << svnReport.toText();
        audit.logEvent("Validation", "pre-migration SVN analysis",
                       svnReport.ok ? "OK" : "FAILED");
        if (!svnReport.ok)
            success = false;
    }

    if (options.generateTraceabilityMap) {
        if (options.gitRepoPath.empty()) {
            std::cerr << "error: --generate-traceability-map requires --git-repo\n";
            svn2git::logging::shutdown();
            return kExitUsageError;
        }
        const bool ok
            = generateTraceability(options.gitRepoPath, repositoryName, reporter);
        audit.logEvent("Traceability", "mapping JSON + SQLite generation",
                       ok ? "OK" : "FAILED");
        if (!ok)
            success = false;
    }

    if (!options.gitlabRemote.empty()) {
        if (options.gitRepoPath.empty()) {
            std::cerr << "error: --push-gitlab requires --git-repo\n";
            svn2git::logging::shutdown();
            return kExitUsageError;
        }
        // Never push a repository that fails integrity checks.
        svn2git::GitValidator gitValidator(options.gitRepoPath, reporter);
        if (!gitValidator.validateRepositoryIntegrity()) {
            audit.logEvent("Push", "aborted: integrity check failed", "FAILED");
            success = false;
        } else if (!pushToGitLab(options.gitRepoPath, options.gitlabRemote, reporter,
                                 audit)) {
            success = false;
        }
    }

    audit.setOutcome(success,
                     success ? "all requested operations completed"
                             : std::to_string(reporter.errorCount())
                             + " error(s) — see error_report.txt");
    if (!audit.generateAuditTrail("audit.log"))
        success = false;
    if (reporter.hasErrors())
        reporter.generateReport();

    log->info("done — {}", success ? "SUCCESS" : "FAILURE");
    svn2git::logging::shutdown();
    return success ? kExitSuccess : kExitValidationFailure;
}
