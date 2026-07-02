/*
 *  svn2git enhanced — pre-flight configuration validation implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/config_validator.h"

#include "svn2git/author_validator.h"
#include "svn2git/logging.h"
#include "svn2git/rules_validator.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <utility>

namespace svn2git {

std::string ConfigReport::toText() const
{
    std::ostringstream out;
    out << "PRE-FLIGHT CONFIGURATION REPORT — " << (ok ? "PASS" : "FAIL") << '\n';
    for (const ConfigCheck& check : checks) {
        out << "  [" << (check.passed ? "OK " : "FAIL") << "] " << check.name << '\n';
        for (const std::string& finding : check.findings)
            out << "        - " << finding << '\n';
    }
    return out.str();
}

ConfigValidator::ConfigValidator(std::string authorsFilePath, std::string rulesFilePath,
                                 std::string yamlFilePath, ErrorReporter& reporter)
    : m_authorsFilePath(std::move(authorsFilePath))
    , m_rulesFilePath(std::move(rulesFilePath))
    , m_yamlFilePath(std::move(yamlFilePath))
    , m_reporter(reporter)
{
}

ConfigCheck ConfigValidator::validateAuthorsFile()
{
    auto log = logging::get("config-validator");
    ConfigCheck check;
    check.name = "authors.txt format (" + m_authorsFilePath + ")";

    // Reuse AuthorValidator's parser: the SVN URL is irrelevant for a
    // pure file-format check, so an empty URL is fine — loadAuthorsFile()
    // never touches the network.
    const std::size_t errorsBefore = m_reporter.errorCount();
    AuthorValidator authorValidator(std::string(), m_authorsFilePath, m_reporter);
    const auto mappings = authorValidator.loadAuthorsFile();

    for (std::size_t i = errorsBefore; i < m_reporter.errorCount(); ++i) {
        const ErrorEntry& entry = m_reporter.entries()[i];
        check.findings.push_back(ErrorReporter::codeName(entry.code) + ": "
                                 + entry.message);
    }

    if (mappings.empty() && check.findings.empty())
        check.findings.push_back("authors file contains no mappings");

    check.passed = check.findings.empty();
    log->info("authors.txt check: {} mapping(s), {} finding(s) — {}", mappings.size(),
              check.findings.size(), check.passed ? "PASS" : "FAIL");
    return check;
}

ConfigCheck ConfigValidator::validateRulesFile()
{
    auto log = logging::get("config-validator");
    ConfigCheck check;
    check.name = ".rules syntax (" + m_rulesFilePath + ")";

    RulesValidator rulesValidator(m_rulesFilePath, m_reporter);
    const RulesValidationResult result = rulesValidator.validate();

    for (const std::string& error : result.errors)
        check.findings.push_back("error: " + error);
    for (const std::string& warning : result.warnings)
        check.findings.push_back("warning: " + warning);

    // Warnings do not fail the pre-flight check; errors do.
    check.passed = result.valid;
    log->info(".rules check: {} rule(s) — {}", result.ruleCount,
              check.passed ? "PASS" : "FAIL");
    return check;
}

ConfigCheck ConfigValidator::validateYamlFile()
{
    auto log = logging::get("config-validator");
    ConfigCheck check;
    check.name = "orchestration YAML (" + m_yamlFilePath + ")";

    std::ifstream file(m_yamlFilePath);
    if (!file.is_open()) {
        check.findings.push_back("cannot open file");
        m_reporter.report(ErrorCode::FileAccessError, "cannot open orchestration YAML",
                          m_yamlFilePath);
        check.passed = false;
        return check;
    }

    // Structural sanity checks only (see header for the rationale):
    //  - YAML forbids tab characters in indentation
    //  - non-comment content lines should be list items, mapping keys,
    //    or document markers
    //  - quotes should be balanced per line
    std::string line;
    long lineNumber = 0;
    bool sawContent = false;
    while (std::getline(file, line)) {
        ++lineNumber;
        const std::string locus = "line " + std::to_string(lineNumber);

        const std::size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos)
            continue; // blank line
        if (line.find('\t') != std::string::npos && line.find('\t') < firstNonSpace + 1) {
            check.findings.push_back(locus + ": tab character in indentation");
            continue;
        }

        const std::string content = line.substr(firstNonSpace);
        if (content[0] == '#')
            continue; // comment
        sawContent = true;

        if (content == "---" || content == "...")
            continue; // document markers

        const bool isListItem = content.rfind("- ", 0) == 0 || content == "-";
        const bool hasColon = content.find(':') != std::string::npos;
        if (!isListItem && !hasColon)
            check.findings.push_back(locus + ": expected 'key: value' or '- item', got '"
                                     + content.substr(0, 40) + "'");

        const auto quoteCount = [&content](char q) {
            return std::count(content.begin(), content.end(), q);
        };
        if (quoteCount('"') % 2 != 0)
            check.findings.push_back(locus + ": unbalanced double quote");
    }

    if (!sawContent)
        check.findings.push_back("file contains no YAML content");

    check.passed = check.findings.empty();
    log->info("YAML check '{}': {} finding(s) — {}", m_yamlFilePath,
              check.findings.size(), check.passed ? "PASS" : "FAIL");
    return check;
}

ConfigReport ConfigValidator::validateAll()
{
    auto log = logging::get("config-validator");
    log->info("running pre-flight configuration validation");

    ConfigReport report;
    if (!m_authorsFilePath.empty())
        report.checks.push_back(validateAuthorsFile());
    if (!m_rulesFilePath.empty())
        report.checks.push_back(validateRulesFile());
    if (!m_yamlFilePath.empty())
        report.checks.push_back(validateYamlFile());

    if (report.checks.empty()) {
        // Nothing to validate is itself a configuration error: the
        // migration would run without any vetted configuration.
        ConfigCheck none;
        none.name = "configuration presence";
        none.passed = false;
        none.findings.push_back("no configuration files were provided to validate");
        report.checks.push_back(none);
        m_reporter.report(ErrorCode::FileAccessError,
                          "pre-flight validation invoked with no configuration files");
    }

    report.ok = std::all_of(report.checks.begin(), report.checks.end(),
                            [](const ConfigCheck& c) { return c.passed; });

    log->info("pre-flight validation: {} check(s) — {}", report.checks.size(),
              report.ok ? "PASS" : "FAIL");
    return report;
}

} // namespace svn2git
