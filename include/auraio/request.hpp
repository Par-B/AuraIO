/**
 * @file request.hpp
 * @brief Request wrapper class for AuraIO C++ bindings
 */

#ifndef AURAIO_REQUEST_HPP
#define AURAIO_REQUEST_HPP

#include <auraio.h>

namespace auraio {

/**
 * Non-owning reference to an in-flight I/O request
 *
 * Valid from submission until the callback begins execution.
 * After the callback starts, the request handle becomes invalid.
 *
 * This class does not manage the request's lifetime - it is managed
 * by the Engine internally.
 */
class Request {
public:
    /**
     * Check if request is still pending
     * @return True if request is still in-flight
     */
    [[nodiscard]] bool pending() const noexcept {
        return handle_ && auraio_request_pending(handle_);
    }

    /**
     * Get file descriptor associated with request
     * @return File descriptor, or -1 if invalid
     */
    [[nodiscard]] int fd() const noexcept {
        return handle_ ? auraio_request_fd(handle_) : -1;
    }

    /**
     * Get underlying C request handle
     * @return Pointer to auraio_request_t
     */
    [[nodiscard]] auraio_request_t* handle() noexcept { return handle_; }

    /**
     * Get underlying C request handle (const)
     * @return Pointer to const auraio_request_t
     */
    [[nodiscard]] const auraio_request_t* handle() const noexcept { return handle_; }

    /**
     * Check if request handle is valid
     * @return True if handle is non-null
     */
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

    /**
     * Construct from C request handle
     *
     * Typically you don't construct Request objects directly - they are
     * returned by Engine I/O methods and passed to callbacks.
     *
     * @param h C request handle
     */
    explicit Request(auraio_request_t* h) noexcept : handle_(h) {}

private:
    friend class Engine;

    auraio_request_t* handle_ = nullptr;
};

} // namespace auraio

#endif // AURAIO_REQUEST_HPP
