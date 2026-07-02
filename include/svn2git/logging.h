/*
 *  svn2git enhanced — logging framework (spdlog wrapper)
 *
 *  Central logging facility for all svn2git enhanced components.
 *  Provides a single initialization point and a named-logger factory so
 *  that every module logs through a uniform sink configuration
 *  (colored console + optional rotating file sink).
 *
 *  EN 50128 note: all migration-relevant operations MUST be logged.
 *  Components obtain their logger via svn2git::logging::get() and never
 *  construct sinks themselves, guaranteeing consistent formatting and
 *  persistence of the log trail.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#ifndef SVN2GIT_LOGGING_H
#define SVN2GIT_LOGGING_H

#include <spdlog/spdlog.h>

#include <cstdint>
#include <memory>
#include <string>

namespace svn2git {
namespace logging {

/// Log verbosity levels exposed to the CLI (maps onto spdlog levels).
enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error, Critical, Off };

/// Convert a CLI string ("trace", "debug", "info", "warn", "error",
/// "critical", "off") to a Level. Unknown strings yield Level::Info and
/// are reported through the returned flag.
/// @param text     the textual level, case-insensitive
/// @param ok       set to false when the text was not recognized
/// @return the parsed level (Level::Info on failure)
Level levelFromString(const std::string& text, bool& ok);

/// Initialize the global logging configuration.
///
/// Installs a colored stderr sink and, when @p logFilePath is non-empty,
/// an additional file sink so the complete migration log survives the
/// process (required for audit purposes).
///
/// Safe to call more than once: subsequent calls reconfigure the level
/// and (if requested) add the file sink. Never throws — a failure to
/// open the log file is reported on stderr and console-only logging
/// continues, because losing console logging entirely would be worse.
///
/// @param level        global minimum severity to emit
/// @param logFilePath  optional path of a persistent log file ("" = none)
/// @return true when fully configured, false when the file sink failed
bool initialize(Level level, const std::string& logFilePath = std::string());

/// Obtain (or lazily create) a named logger sharing the global sinks.
/// Never returns nullptr: if initialize() was not called yet, a default
/// console-only configuration is installed on first use.
/// @param name  component name, e.g. "author-validator"
std::shared_ptr<spdlog::logger> get(const std::string& name);

/// Change the global log level at runtime (affects all existing loggers).
void setLevel(Level level);

/// Flush all sinks — call before process exit on error paths so the
/// audit-relevant tail of the log is never lost.
void shutdown();

} // namespace logging
} // namespace svn2git

#endif // SVN2GIT_LOGGING_H
