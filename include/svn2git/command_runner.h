/*
 *  svn2git enhanced — external command execution utility
 *
 *  Thin RAII wrapper around popen(3) used by the validators to invoke
 *  `svn`, `git` and `gpg`. Centralizing subprocess handling here keeps
 *  the validators unit-testable: each validator accepts a Runner
 *  std::function and tests inject a fake that returns canned output.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_COMMAND_RUNNER_H
#define SVN2GIT_COMMAND_RUNNER_H

#include <functional>
#include <string>

namespace svn2git {

/// Result of running an external command.
struct CommandResult {
    int exitCode = -1; ///< process exit code; -1 when spawn failed
    std::string output; ///< captured stdout (stderr redirected into it)

    /// True when the process was spawned and exited with status 0.
    bool ok() const { return exitCode == 0; }
};

/// Callable used by validators to execute a shell command.
/// Production code passes CommandRunner::run; unit tests pass a fake.
using Runner = std::function<CommandResult(const std::string& command)>;

/// Executes shell commands, capturing combined stdout+stderr.
class CommandRunner {
public:
    /// Run @p command through /bin/sh, capturing combined output.
    ///
    /// Never throws. A failure to spawn is reported as exitCode == -1
    /// with a diagnostic in @c output, so callers always have an explicit
    /// error path to handle (no silent failures).
    static CommandResult run(const std::string& command);

    /// Quote a string for safe interpolation into a shell command line.
    /// Wraps in single quotes and escapes embedded single quotes, which
    /// neutralizes all shell metacharacters (POSIX sh semantics).
    static std::string shellQuote(const std::string& value);
};

} // namespace svn2git

#endif // SVN2GIT_COMMAND_RUNNER_H
