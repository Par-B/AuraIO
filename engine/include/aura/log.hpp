/**
 * @file log.hpp
 * @brief Logging interface for AuraIO C++ bindings
 *
 * Provides type-safe wrappers around the C log handler API.
 * The log handler is process-wide and thread-safe.
 *
 * Example:
 * @code
 *   aura::set_log_handler([](aura::LogLevel level, std::string_view msg) {
 *       std::cerr << "[" << aura::log_level_name(level) << "] " << msg << "\n";
 *   });
 *
 *   aura::Engine engine;
 *   aura::log_emit(aura::LogLevel::Info, "engine started");
 *   // ... library and app logs dispatched to the handler ...
 *
 *   aura::clear_log_handler();
 * @endcode
 */

#ifndef AURA_LOG_HPP
#define AURA_LOG_HPP

#include <aura.h>

#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace aura {

/// Log severity levels (match syslog priorities 1:1)
enum class LogLevel {
    Error = AURA_LOG_ERR,     ///< Error condition
    Warning = AURA_LOG_WARN,  ///< Warning condition
    Notice = AURA_LOG_NOTICE, ///< Normal but significant
    Info = AURA_LOG_INFO,     ///< Informational
    Debug = AURA_LOG_DEBUG    ///< Debug-level
};

/// Return a short name for the given log level ("ERR", "WARN", etc.)
[[nodiscard]] inline const char *log_level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Error:
        return "ERR";
    case LogLevel::Warning:
        return "WARN";
    case LogLevel::Notice:
        return "NOTICE";
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Debug:
        return "DEBUG";
    default:
        return "???";
    }
}

/// Log handler callback type
using LogHandler = std::function<void(LogLevel, std::string_view)>;

namespace detail {

/// Global handler state — inline variables for header-only ODR safety (C++17)
inline std::mutex log_mutex;
inline LogHandler log_handler_fn;

} // namespace detail

} // namespace aura

/// C trampoline for the log handler.  extern "C" + inline avoids
/// duplicate-symbol errors when this header is included from multiple TUs.
extern "C" inline void aura_detail_log_trampoline(int level, const char *msg,
                                                    void * /*userdata*/) {
    // Fast path: avoid locking if no message
    if (!msg) return;

    try {
        std::lock_guard<std::mutex> lock(aura::detail::log_mutex);
        if (aura::detail::log_handler_fn) {
            aura::detail::log_handler_fn(static_cast<aura::LogLevel>(level),
                                           std::string_view(msg));
        }
    } catch (...) {
        // Exceptions must not propagate through extern "C"
        std::terminate();
    }
}

namespace aura {

/// Install a process-wide log handler.
///
/// Replaces any previously installed handler.  The handler is called from
/// whichever thread emits the log message; it must be thread-safe.
inline void set_log_handler(LogHandler handler) {
    {
        std::lock_guard<std::mutex> lock(detail::log_mutex);
        detail::log_handler_fn = std::move(handler);
    }
    aura_set_log_handler(aura_detail_log_trampoline, nullptr);
}

/// Remove the current log handler.
///
/// After this call the library is silent (default state).
inline void clear_log_handler() noexcept {
    aura_set_log_handler(nullptr, nullptr);
    std::lock_guard<std::mutex> lock(detail::log_mutex);
    detail::log_handler_fn = nullptr;
}

/// Emit a log message through the registered handler (if any).
///
/// No-op when no handler is installed.  Thread-safe.
inline void log_emit(LogLevel level, std::string_view msg) {
    // Build a NUL-terminated copy for the C API
    std::string tmp(msg);
    aura_log_emit(static_cast<int>(level), "%s", tmp.c_str());
}

} // namespace aura

#endif // AURA_LOG_HPP
