/*
 *  svn2git enhanced — pre-flight configuration validation
 *
 *  Runs every static configuration check before a migration is allowed
 *  to start: authors.txt format, .rules syntax/semantics, and (when
 *  present) the orchestration YAML. Aggregates the results into one
 *  comprehensive report so operators fix everything in a single pass.
 *
 *  Related classes: delegates to AuthorValidator and RulesValidator;
 *  reports through ErrorReporter; invoked first by the CLI workflow.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_CONFIG_VALIDATOR_H
#define SVN2GIT_CONFIG_VALIDATOR_H

#include "svn2git/error_reporter.h"

#include <string>
#include <vector>

namespace svn2git {

/// Result of one configuration artifact check.
struct ConfigCheck {
    std::string name; ///< e.g. "authors.txt format"
    bool passed = false; ///< outcome
    std::vector<std::string> findings; ///< errors/warnings for this check
};

/// Aggregated pre-flight validation outcome.
struct ConfigReport {
    bool ok = false; ///< all mandatory checks passed
    std::vector<ConfigCheck> checks; ///< individual check results

    /// Render the report as human-readable text (one block per check).
    std::string toText() const;
};

/// Pre-flight validator for all migration configuration files.
class ConfigValidator {
public:
    /// @param authorsFilePath  path to authors.txt ("" = skip check)
    /// @param rulesFilePath    path to the .rules file ("" = skip check)
    /// @param yamlFilePath     optional orchestration YAML ("" = skip)
    /// @param reporter         shared error sink
    ConfigValidator(std::string authorsFilePath, std::string rulesFilePath,
                    std::string yamlFilePath, ErrorReporter& reporter);

    /// Run every applicable check and aggregate the outcome.
    ConfigReport validateAll();

    /// authors.txt structural check: parseable lines, valid emails,
    /// no duplicate usernames. Does NOT need SVN access (coverage
    /// against actual SVN history is AuthorValidator::validateCoverage).
    ConfigCheck validateAuthorsFile();

    /// .rules structural/semantic check via RulesValidator.
    ConfigCheck validateRulesFile();

    /// Light structural sanity check of an orchestration YAML file.
    ///
    /// Simplification: svn2git does not link a YAML parser; this check
    /// catches the frequent failure modes (tabs used for indentation,
    /// unbalanced quotes, missing ':' on mapping lines) rather than
    /// performing a full YAML 1.2 parse.
    ConfigCheck validateYamlFile();

private:
    std::string m_authorsFilePath;
    std::string m_rulesFilePath;
    std::string m_yamlFilePath;
    ErrorReporter& m_reporter;
};

} // namespace svn2git

#endif // SVN2GIT_CONFIG_VALIDATOR_H
