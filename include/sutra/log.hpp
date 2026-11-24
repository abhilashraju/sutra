/**
 * @file log.hpp
 * @brief Minimal logging used by sutra (no iostreams).
 *
 * Output goes to stderr. Define SUTRA_LOG_LEVEL (0 = debug ... 3 = error,
 * 4 = off) at compile time to filter. Applications can replace the sink with
 * `setLogSink()`.
 */
#pragma once

#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>

#ifndef SUTRA_LOG_LEVEL
#define SUTRA_LOG_LEVEL 1
#endif

namespace sutra
{

enum class LogLevel
{
    debug = 0,
    info = 1,
    warning = 2,
    error = 3,
};

using LogSink = std::function<void(LogLevel, std::string_view)>;

namespace details
{
inline LogSink& logSink()
{
    static LogSink sink;
    return sink;
}

inline const char* levelName(LogLevel level)
{
    switch (level)
    {
        case LogLevel::debug:
            return "DEBUG";
        case LogLevel::info:
            return "INFO";
        case LogLevel::warning:
            return "WARN";
        case LogLevel::error:
            return "ERROR";
    }
    return "?";
}
} // namespace details

/** @brief Replaces the default stderr sink. Pass nullptr to restore it. */
inline void setLogSink(LogSink sink)
{
    details::logSink() = std::move(sink);
}

template <class... Args>
void log(LogLevel level, std::format_string<Args...> fmt, Args&&... args)
{
    if (static_cast<int>(level) < SUTRA_LOG_LEVEL)
    {
        return;
    }
    std::string message = std::format(fmt, std::forward<Args>(args)...);
    if (auto& sink = details::logSink(); sink)
    {
        sink(level, message);
        return;
    }
    std::fprintf(stderr, "[sutra %s] %s\n", details::levelName(level),
                 message.c_str());
}

template <class... Args>
void logDebug(std::format_string<Args...> fmt, Args&&... args)
{
    log(LogLevel::debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args)
{
    log(LogLevel::info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void logWarning(std::format_string<Args...> fmt, Args&&... args)
{
    log(LogLevel::warning, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void logError(std::format_string<Args...> fmt, Args&&... args)
{
    log(LogLevel::error, fmt, std::forward<Args>(args)...);
}

} // namespace sutra
