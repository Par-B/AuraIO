/**
 * @file error.hpp
 * @brief Error exception class for AuraIO C++ bindings
 */

#ifndef AURAIO_ERROR_HPP
#define AURAIO_ERROR_HPP

#include <exception>
#include <string>
#include <string_view>
#include <system_error>

namespace auraio {

/**
 * Exception class for AuraIO errors
 *
 * Wraps errno values with optional context message.
 */
class Error : public std::exception {
public:
    /**
     * Construct error from errno value
     *
     * @param err Error code (positive errno value)
     * @param context Optional context message
     */
    explicit Error(int err, std::string_view context = {})
        : code_(err)
    {
        // Use std::generic_category().message() instead of strerror()
        // for thread-safety (strerror may use a static buffer)
        std::string errmsg = std::generic_category().message(err);
        if (context.empty()) {
            message_ = std::move(errmsg);
        } else {
            message_ = std::string(context) + ": " + errmsg;
        }
    }

    /**
     * Get the error code
     * @return Positive errno value
     */
    [[nodiscard]] int code() const noexcept { return code_; }

    /**
     * Get human-readable error message
     * @return Error message string
     */
    [[nodiscard]] const char* what() const noexcept override {
        return message_.c_str();
    }

    // Convenience predicates
    [[nodiscard]] bool is_invalid() const noexcept { return code_ == EINVAL; }
    [[nodiscard]] bool is_again() const noexcept { return code_ == EAGAIN; }
    [[nodiscard]] bool is_shutdown() const noexcept { return code_ == ESHUTDOWN; }
    [[nodiscard]] bool is_cancelled() const noexcept { return code_ == ECANCELED; }
    [[nodiscard]] bool is_busy() const noexcept { return code_ == EBUSY; }
    [[nodiscard]] bool is_not_found() const noexcept { return code_ == ENOENT; }

private:
    int code_;
    std::string message_;
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

} // namespace auraio

#endif // AURAIO_ERROR_HPP
