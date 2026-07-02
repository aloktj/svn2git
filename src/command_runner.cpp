/*
 *  svn2git enhanced — external command execution implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/command_runner.h"

#include "svn2git/logging.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

#include <sys/wait.h>

namespace svn2git {

CommandResult CommandRunner::run(const std::string& command)
{
    auto log = logging::get("command-runner");
    log->debug("executing: {}", command);

    CommandResult result;

    // Redirect stderr into the pipe so diagnostics from svn/git are
    // captured alongside regular output.
    const std::string wrapped = command + " 2>&1";

    // RAII guard for the popen stream; pclose() also reaps the child.
    struct PipeCloser {
        void operator()(FILE* f) const noexcept
        {
            if (f != nullptr)
                pclose(f);
        }
    };

    errno = 0;
    FILE* raw = popen(wrapped.c_str(), "r");
    if (raw == nullptr) {
        result.exitCode = -1;
        result.output = std::string("failed to spawn command: ") + std::strerror(errno);
        log->error("popen failed for '{}': {}", command, result.output);
        return result;
    }

    std::array<char, 4096> buffer {};
    {
        // Scope ensures we read everything before extracting the status.
        std::unique_ptr<FILE, PipeCloser> pipe(raw);
        size_t n = 0;
        while ((n = std::fread(buffer.data(), 1, buffer.size(), pipe.get())) > 0)
            result.output.append(buffer.data(), n);

        // pclose (via deleter) returns the wait status; we need it, so
        // release and close manually instead.
        FILE* toClose = pipe.release();
        const int status = pclose(toClose);
        if (status == -1) {
            result.exitCode = -1;
            log->error("pclose failed for '{}': {}", command, std::strerror(errno));
            return result;
        }
        if (WIFEXITED(status))
            result.exitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            result.exitCode = 128 + WTERMSIG(status); // shell convention
        else
            result.exitCode = -1;
    }

    log->debug("command exited with {} ({} bytes of output)", result.exitCode,
               result.output.size());
    return result;
}

std::string CommandRunner::shellQuote(const std::string& value)
{
    // POSIX single-quote quoting: close quote, emit \' , reopen quote.
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted.push_back('\'');
    for (const char c : value) {
        if (c == '\'')
            quoted += "'\\''";
        else
            quoted.push_back(c);
    }
    quoted.push_back('\'');
    return quoted;
}

} // namespace svn2git
