/*
 *  svn2git enhanced — author validation implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/author_validator.h"

#include "svn2git/logging.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace svn2git {

namespace {

/// Trim ASCII whitespace from both ends of @p text.
std::string trim(const std::string& text)
{
    const auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    auto begin = std::find_if_not(text.begin(), text.end(), isSpace);
    auto end = std::find_if_not(text.rbegin(), text.rend(), isSpace).base();
    return (begin < end) ? std::string(begin, end) : std::string();
}

} // namespace

AuthorValidator::AuthorValidator(std::string svnRepoUrl, std::string authorsFilePath,
                                 ErrorReporter& reporter, Runner runner)
    : m_svnRepoUrl(std::move(svnRepoUrl))
    , m_authorsFilePath(std::move(authorsFilePath))
    , m_reporter(reporter)
    , m_runner(std::move(runner))
{
}

std::vector<AuthorMapping> AuthorValidator::extractSVNAuthors()
{
    auto log = logging::get("author-validator");
    log->info("extracting authors from SVN repository '{}'", m_svnRepoUrl);

    // `svn log --quiet` prints one header line per revision:
    //   r123 | jsmith | 2024-01-15 10:30:00 +0000 (Mon, 15 Jan 2024)
    // separated by dashed lines. This is far cheaper to parse than XML
    // and locale-independent for the fields we need.
    const std::string command
        = "svn log --quiet --non-interactive " + CommandRunner::shellQuote(m_svnRepoUrl);
    const CommandResult result = m_runner(command);

    if (!result.ok()) {
        m_reporter.report(ErrorCode::ExternalToolError,
                          "svn log failed with exit code "
                              + std::to_string(result.exitCode) + ": "
                              + trim(result.output).substr(0, 300),
                          m_svnRepoUrl);
        return {};
    }

    std::map<std::string, long> commitCounts;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        // Revision header lines start with 'r<digits> |'.
        if (line.size() < 2 || line[0] != 'r'
            || std::isdigit(static_cast<unsigned char>(line[1])) == 0)
            continue;
        const std::size_t firstPipe = line.find('|');
        if (firstPipe == std::string::npos)
            continue;
        const std::size_t secondPipe = line.find('|', firstPipe + 1);
        if (secondPipe == std::string::npos)
            continue;
        const std::string author
            = trim(line.substr(firstPipe + 1, secondPipe - firstPipe - 1));
        if (!author.empty())
            ++commitCounts[author];
    }

    if (commitCounts.empty()) {
        m_reporter.report(ErrorCode::ExternalToolError,
                          "no authors found in SVN history — repository empty "
                          "or log output unparsable",
                          m_svnRepoUrl);
        return {};
    }

    // Fill in mapped identities where authors.txt provides them.
    const auto mappings = loadAuthorsFile();

    std::vector<AuthorMapping> authors;
    authors.reserve(commitCounts.size());
    for (const auto& [username, count] : commitCounts) {
        AuthorMapping mapping;
        mapping.svnUsername = username;
        mapping.commitCount = count;
        const auto found = mappings.find(username);
        if (found != mappings.end()) {
            mapping.fullName = found->second.first;
            mapping.email = found->second.second;
        }
        authors.push_back(std::move(mapping));
    }

    log->info("found {} distinct author(s) across SVN history", authors.size());
    return authors;
}

std::map<std::string, std::pair<std::string, std::string>>
AuthorValidator::loadAuthorsFile()
{
    auto log = logging::get("author-validator");
    std::map<std::string, std::pair<std::string, std::string>> mappings;

    std::ifstream file(m_authorsFilePath);
    if (!file.is_open()) {
        m_reporter.report(ErrorCode::FileAccessError, "cannot open authors file",
                          m_authorsFilePath);
        return mappings;
    }

    // Expected line format (same as the classic identity-map):
    //   svnuser = Full Name <email@example.com>
    // '#' starts a comment; blank lines are ignored.
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

        const std::string locus = m_authorsFilePath + ":" + std::to_string(lineNumber);

        const std::size_t equals = line.find('=');
        if (equals == std::string::npos) {
            m_reporter.report(ErrorCode::MissingAuthors,
                              "malformed authors line (missing '='): " + line, locus);
            continue;
        }

        const std::string username = trim(line.substr(0, equals));
        const std::string identity = trim(line.substr(equals + 1));
        const std::size_t open = identity.find('<');
        const std::size_t close = identity.rfind('>');
        if (username.empty() || open == std::string::npos || close == std::string::npos
            || close < open) {
            m_reporter.report(ErrorCode::MissingAuthors,
                              "malformed authors line (expected 'user = Name <email>'): "
                                  + line,
                              locus);
            continue;
        }

        const std::string fullName = trim(identity.substr(0, open));
        const std::string email = trim(identity.substr(open + 1, close - open - 1));

        if (mappings.count(username) != 0) {
            m_reporter.report(ErrorCode::DuplicateAuthorEntry,
                              "duplicate mapping for SVN username '" + username + "'",
                              locus);
            continue; // first entry wins, matching classic svn2git behavior
        }

        if (!isValidEmail(email)) {
            m_reporter.report(ErrorCode::InvalidEmail,
                              "invalid email '" + email + "' for user '" + username + "'",
                              locus);
            // Still record the mapping: coverage and email validity are
            // reported independently so one fix pass can address both.
        }

        mappings.emplace(username, std::make_pair(fullName, email));
    }

    log->debug("loaded {} author mapping(s) from '{}'", mappings.size(),
               m_authorsFilePath);
    return mappings;
}

AuthorValidationReport AuthorValidator::validateCoverage()
{
    auto log = logging::get("author-validator");
    log->info("validating author coverage: repo='{}' authors-file='{}'", m_svnRepoUrl,
              m_authorsFilePath);

    AuthorValidationReport report;

    // Track duplicates/invalid emails discovered while loading the file:
    // loadAuthorsFile() reports them via ErrorReporter; we snapshot the
    // reporter growth to attribute findings to this validation run.
    const std::size_t errorsBefore = m_reporter.errorCount();
    const std::vector<AuthorMapping> authors = extractSVNAuthors();
    if (authors.empty()) {
        report.messages.push_back(
            "author extraction failed — see error report for details");
        return report; // ok stays false
    }

    report.totalSvnAuthors = static_cast<long>(authors.size());

    for (const AuthorMapping& author : authors) {
        if (author.email.empty() && author.fullName.empty()) {
            report.missingAuthors.push_back(author);
            std::ostringstream msg;
            msg << "SVN author '" << author.svnUsername << "' (" << author.commitCount
                << " commit(s)) is not mapped; suggestion: "
                << suggestMapping(author.svnUsername);
            report.messages.push_back(msg.str());
        } else {
            ++report.mappedAuthors;
            if (!isValidEmail(author.email))
                report.invalidEmails.push_back(author.svnUsername);
        }
    }

    // Fold in structured findings recorded during file loading.
    for (std::size_t i = errorsBefore; i < m_reporter.errorCount(); ++i) {
        const ErrorEntry& entry = m_reporter.entries()[i];
        if (entry.code == ErrorCode::DuplicateAuthorEntry)
            report.duplicateEntries.push_back(entry.message);
    }

    if (!report.missingAuthors.empty()) {
        m_reporter.report(ErrorCode::MissingAuthors,
                          std::to_string(report.missingAuthors.size())
                              + " SVN author(s) missing from authors file",
                          m_authorsFilePath);
    }

    report.ok = report.missingAuthors.empty() && report.invalidEmails.empty()
        && report.duplicateEntries.empty();

    log->info("author coverage: {}/{} mapped, {} invalid email(s), {} duplicate(s) — {}",
              report.mappedAuthors, report.totalSvnAuthors, report.invalidEmails.size(),
              report.duplicateEntries.size(), report.ok ? "PASS" : "FAIL");
    return report;
}

std::string AuthorValidator::autoMapAuthors()
{
    auto log = logging::get("author-validator");
    const std::vector<AuthorMapping> authors = extractSVNAuthors();
    if (authors.empty())
        return std::string();

    std::ostringstream out;
    out << "# authors.txt generated by svn2git --auto-map-authors on "
        << currentIso8601Utc() << '\n'
        << "# Review every placeholder identity before running the migration.\n";

    long placeholders = 0;
    for (const AuthorMapping& author : authors) {
        if (!author.email.empty() || !author.fullName.empty()) {
            // Preserve the existing mapping verbatim.
            out << author.svnUsername << " = " << author.fullName << " <" << author.email
                << ">\n";
        } else {
            out << suggestMapping(author.svnUsername) << '\n';
            ++placeholders;
        }
    }

    log->info("auto-mapped {} author(s), {} placeholder(s) require review",
              authors.size(), placeholders);
    return out.str();
}

bool AuthorValidator::isValidEmail(const std::string& email)
{
    // Pragmatic RFC 5322 subset (full RFC parsing is deliberately out of
    // scope): exactly one '@', non-empty local part, dotted domain, no
    // whitespace/control characters, no leading/trailing dot in domain.
    if (email.empty())
        return false;

    const std::size_t at = email.find('@');
    if (at == std::string::npos || at == 0
        || email.find('@', at + 1) != std::string::npos)
        return false;

    const std::string domain = email.substr(at + 1);
    const std::size_t dot = domain.find('.');
    if (domain.empty() || dot == std::string::npos || domain.front() == '.'
        || domain.back() == '.')
        return false;

    return std::none_of(email.begin(), email.end(), [](unsigned char c) {
        return std::isspace(c) != 0 || std::iscntrl(c) != 0;
    });
}

std::string AuthorValidator::suggestMapping(const std::string& svnUsername)
{
    // Heuristic: dotted or underscored usernames are usually
    // "first.last" — title-case the parts into a display name.
    std::string displayName;
    std::string part;
    bool structured = false;
    for (const char c : svnUsername) {
        if (c == '.' || c == '_' || c == '-') {
            structured = true;
            if (!part.empty()) {
                if (!displayName.empty())
                    displayName += ' ';
                displayName += part;
                part.clear();
            }
        } else {
            part += (part.empty()
                         ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                         : c);
        }
    }
    if (!part.empty()) {
        if (!displayName.empty())
            displayName += ' ';
        displayName += part;
    }
    if (!structured || displayName.empty())
        displayName = svnUsername;

    return svnUsername + " = " + displayName + " <" + svnUsername + "@localhost>";
}

} // namespace svn2git
