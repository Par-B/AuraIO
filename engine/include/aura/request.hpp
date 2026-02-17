/**
 * @file request.hpp
 * @brief Request wrapper class for AuraIO C++ bindings
 */

#ifndef AURA_REQUEST_HPP
#define AURA_REQUEST_HPP

#include <aura.h>

namespace aura {

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
        return handle_ && aura_request_pending(handle_);
    }

    /**
     * Get file descriptor associated with request
     * @return File descriptor, or -1 if invalid
     */
    [[nodiscard]] int fd() const noexcept {
        return handle_ ? aura_request_fd(handle_) : -1;
    }

    /**
     * Get operation type of request
     * @return Operation type (AURA_OP_READ, AURA_OP_WRITE, etc.), or -1 if invalid
     */
    [[nodiscard]] int op_type() const noexcept {
        return handle_ ? aura_request_op_type(handle_) : -1;
    }

    /**
     * Get underlying C request handle
     * @return Pointer to aura_request_t
     */
    [[nodiscard]] aura_request_t* handle() noexcept { return handle_; }

    /**
     * Get underlying C request handle (const)
     * @return Pointer to const aura_request_t
     */
    [[nodiscard]] const aura_request_t* handle() const noexcept { return handle_; }

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
    explicit Request(aura_request_t* h) noexcept : handle_(h) {}

private:
    friend class Engine;

    aura_request_t* handle_ = nullptr;
};

} // namespace aura

#endif // AURA_REQUEST_HPP
