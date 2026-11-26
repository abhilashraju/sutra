/**
 * @file log.hpp
 * @brief Minimal logging used by sutra (no iostreams).
 *
 * Output goes to the systemd journal through sd_journal_send, with the
 * matching priority and the program name as SYSLOG_IDENTIFIER, so
 * `journalctl -t <program> -p warning` works. If the journal cannot be
 * reached the line is written to stderr as `[sutra LEVEL] message`.
 *
 * Debug lines are sent with the INFO priority: OpenBMC configures journald
 * with MaxLevelStore/MaxLevelSyslog=info, so debug-priority entries would be
 * dropped. Define SUTRA_LOG_LEVEL (0 = debug ... 3 = error, 4 = off) at compile
 * time to filter. Applications can replace the sink with `setLogSink()`.
 */
#pragma once

#include <errno.h>
#include <systemd/sd-journal.h>

#include <concepts>
#include <cstdio>
#include <format>
#include <functional>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>

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

/**
 * @return the syslog priority for `level` (numeric, so <syslog.h>'s LOG_*
 *         macros are not pulled into every includer).
 */
inline int journalPriority(LogLevel level)
{
    switch (level)
    {
        case LogLevel::debug:
            return 6; // INFO: journald on OpenBMC drops anything below it
        case LogLevel::info:
            return 6; // LOG_INFO
        case LogLevel::warning:
            return 4; // LOG_WARNING
        case LogLevel::error:
            return 3; // LOG_ERR
    }
    return 6;
}

/** @brief Default sink: the journal, with stderr as the fallback. */
inline void journalSink(LogLevel level, std::string_view message)
{
    if (sd_journal_send("MESSAGE=%.*s", static_cast<int>(message.size()),
                        message.data(), "PRIORITY=%i", journalPriority(level),
                        "SYSLOG_IDENTIFIER=%s", program_invocation_short_name,
                        nullptr) < 0)
    {
        std::fprintf(stderr, "[sutra %s] %.*s\n", levelName(level),
                     static_cast<int>(message.size()), message.data());
    }
}
} // namespace details

/** @brief Replaces the default journal sink. Pass nullptr to restore it. */
inline void setLogSink(LogSink sink)
{
    details::logSink() = std::move(sink);
}

/**
 * @brief A format string that also captures the caller's source location.
 *
 * Implicitly constructible from a string literal, so call sites keep the
 * `logWarning("fmt {}", x)` form while the file name and line of that call are
 * recorded automatically.
 */
template <class... Args>
struct LogFormat
{
    std::format_string<Args...> fmt;
    std::source_location loc;

    template <class T>
        requires std::convertible_to<const T&, std::string_view>
    consteval LogFormat(
        const T& s, std::source_location l = std::source_location::current()) :
        fmt(s), loc(l)
    {}
};

template <class... Args>
void log(LogLevel level, LogFormat<std::type_identity_t<Args>...> fmt,
         Args&&... args)
{
    if (static_cast<int>(level) < SUTRA_LOG_LEVEL)
    {
        return;
    }
    std::string_view file = fmt.loc.file_name();
    if (auto slash = file.find_last_of('/'); slash != std::string_view::npos)
    {
        file.remove_prefix(slash + 1);
    }
    std::string message = std::format("{}:{}: ", file, fmt.loc.line());
    std::format_to(std::back_inserter(message), fmt.fmt,
                   std::forward<Args>(args)...);
    if (auto& sink = details::logSink(); sink)
    {
        sink(level, message);
        return;
    }
    details::journalSink(level, message);
}

template <class... Args>
void logDebug(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    log(LogLevel::debug, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void logInfo(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    log(LogLevel::info, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void logWarning(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    log(LogLevel::warning, fmt, std::forward<Args>(args)...);
}

template <class... Args>
void logError(LogFormat<std::type_identity_t<Args>...> fmt, Args&&... args)
{
    log(LogLevel::error, fmt, std::forward<Args>(args)...);
}

} // namespace sutra
