/*
 *  svn2git enhanced — logging framework implementation
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 */

#include "svn2git/logging.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <vector>

namespace svn2git {
namespace logging {

namespace {

// Shared sink list: every named logger created through get() attaches to
// these sinks so console and file output stay in lockstep.
std::mutex g_mutex;
std::vector<spdlog::sink_ptr> g_sinks;
spdlog::level::level_enum g_level = spdlog::level::info;
bool g_initialized = false;

spdlog::level::level_enum toSpdlog(Level level)
{
    switch (level) {
    case Level::Trace:
        return spdlog::level::trace;
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    case Level::Critical:
        return spdlog::level::critical;
    case Level::Off:
        return spdlog::level::off;
    }
    // Unreachable with a valid enum; defensive default for safety builds.
    return spdlog::level::info;
}

// Install the default console sink. Caller must hold g_mutex.
void ensureConsoleSinkLocked()
{
    if (g_sinks.empty()) {
        auto console = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
        console->set_pattern("[%Y-%m-%dT%H:%M:%S%z] [%n] [%^%l%$] %v");
        g_sinks.push_back(console);
    }
}

} // namespace

Level levelFromString(const std::string& text, bool& ok)
{
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    ok = true;
    if (lowered == "trace")
        return Level::Trace;
    if (lowered == "debug")
        return Level::Debug;
    if (lowered == "info")
        return Level::Info;
    if (lowered == "warn" || lowered == "warning")
        return Level::Warn;
    if (lowered == "error")
        return Level::Error;
    if (lowered == "critical")
        return Level::Critical;
    if (lowered == "off")
        return Level::Off;

    ok = false;
    return Level::Info;
}

bool initialize(Level level, const std::string& logFilePath)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    g_level = toSpdlog(level);
    ensureConsoleSinkLocked();

    bool fileSinkOk = true;
    if (!logFilePath.empty()) {
        try {
            auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                logFilePath, /*truncate=*/false);
            file->set_pattern("[%Y-%m-%dT%H:%M:%S%z] [%n] [%l] %v");
            g_sinks.push_back(file);
        } catch (const spdlog::spdlog_ex& ex) {
            // Console logging must survive a bad log-file path: report and
            // continue rather than aborting the whole migration.
            std::fprintf(stderr,
                         "svn2git: cannot open log file '%s': %s — continuing with "
                         "console logging only\n",
                         logFilePath.c_str(), ex.what());
            fileSinkOk = false;
        }
    }

    // Reconfigure any loggers created before initialize() was called so
    // they pick up the file sink and the requested level.
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
        logger->sinks() = g_sinks;
        logger->set_level(g_level);
    });
    spdlog::set_level(g_level);

    g_initialized = true;
    return fileSinkOk;
}

std::shared_ptr<spdlog::logger> get(const std::string& name)
{
    // Fast path: already registered.
    if (auto existing = spdlog::get(name))
        return existing;

    std::lock_guard<std::mutex> lock(g_mutex);
    // Re-check under the lock — another thread may have registered it.
    if (auto existing = spdlog::get(name))
        return existing;

    ensureConsoleSinkLocked();
    auto logger = std::make_shared<spdlog::logger>(name, g_sinks.begin(), g_sinks.end());
    logger->set_level(g_level);
    logger->flush_on(spdlog::level::err);
    spdlog::register_logger(logger);
    return logger;
}

void setLevel(Level level)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_level = toSpdlog(level);
    spdlog::set_level(g_level);
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& logger) {
        logger->set_level(g_level);
    });
}

void shutdown()
{
    spdlog::apply_all(
        [](const std::shared_ptr<spdlog::logger>& logger) { logger->flush(); });
    spdlog::shutdown();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_sinks.clear();
    g_initialized = false;
}

} // namespace logging
} // namespace svn2git
