// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file error.hpp
 * @brief Error exception class for AuraIO C++ bindings
 */

#ifndef AURA_ERROR_HPP
#define AURA_ERROR_HPP

#include <string>
#include <string_view>
#include <system_error>

namespace aura {

/**
 * Exception class for AuraIO errors
 *
 * Inherits from std::system_error so callers can catch either
 * aura::Error or std::system_error. Uses std::generic_category
 * for POSIX errno values.
 */
class Error : public std::system_error {
  public:
    /**
     * Construct error from errno value
     *
     * @param err Error code (positive errno value)
     * @param context Optional context message
     */
    explicit Error(int err, std::string_view context = {})
        : std::system_error(err, std::generic_category(), std::string(context)) {}

    /**
     * Get the error code
     * @return Positive errno value
     */
    [[nodiscard]] int code() const noexcept { return std::system_error::code().value(); }

    // Convenience predicates
    [[nodiscard]] bool is_invalid() const noexcept { return code() == EINVAL; }
    [[nodiscard]] bool is_again() const noexcept { return code() == EAGAIN; }
    [[nodiscard]] bool is_shutdown() const noexcept { return code() == ESHUTDOWN; }
    [[nodiscard]] bool is_cancelled() const noexcept { return code() == ECANCELED; }
    [[nodiscard]] bool is_busy() const noexcept { return code() == EBUSY; }
    [[nodiscard]] bool is_timeout() const noexcept { return code() == ETIMEDOUT; }
    [[nodiscard]] bool is_not_found() const noexcept { return code() == ENOENT; }
};

/**
 * Throw Error if condition is false
 *
 * @param condition Condition to check
 * @param context Error context message
 * @throws Error with current errno if condition is false
 */
inline void check(bool condition, std::string_view context = {}) {
    if (!condition) {
        throw Error(errno, context);
    }
}

/**
 * Throw Error from current errno
 *
 * @param context Error context message
 * @throws Error with current errno
 */
[[noreturn]] inline void throw_errno(std::string_view context = {}) {
    throw Error(errno, context);
}

} // namespace aura

#endif // AURA_ERROR_HPP
