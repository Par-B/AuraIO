/**
 * @file engine.hpp
 * @brief Main Engine class for AuraIO C++ bindings
 */

#ifndef AURAIO_ENGINE_HPP
#define AURAIO_ENGINE_HPP

#include <auraio.h>
#include <auraio/fwd.hpp>
#include <auraio/error.hpp>
#include <auraio/options.hpp>
#include <auraio/buffer.hpp>
#include <auraio/request.hpp>
#include <auraio/stats.hpp>
#include <auraio/detail/callback_storage.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>

namespace auraio {

// Forward declarations for coroutine support
class IoAwaitable;
class FsyncAwaitable;

/**
 * Callback concept for I/O completion handlers
 */
template<typename F>
concept Callback = std::invocable<F, Request&, ssize_t>;

/**
 * Main AuraIO engine class
 *
 * Manages io_uring rings and provides async I/O operations.
 * Move-only (cannot be copied).
 *
 * @par Destruction Behavior
 * The destructor waits for all pending I/O operations to complete before
 * returning. All callbacks (including I/O completion callbacks) will be
 * invoked during destruction. This means:
 * - It is safe to capture references to stack variables in callbacks, as long
 *   as those variables remain in scope until the Engine is destroyed
 * - Callbacks may still run during Engine destruction - do not access the
 *   Engine from callbacks if it might be in the process of being destroyed
 * - New I/O submissions from callbacks during shutdown will fail with ESHUTDOWN
 *
 * Example:
 * @code
 * auraio::Engine engine;
 * auto buf = engine.allocate_buffer(4096);
 *
 * engine.read(fd, buf, 4096, 0, [](auto& req, ssize_t result) {
 *     if (result > 0) {
 *         std::cout << "Read " << result << " bytes\n";
 *     }
 * });
 *
 * engine.wait();  // Wait for completion
 * @endcode
 */
class Engine {
public:
    /**
     * Create engine with default options
     * @throws Error on failure
     */
    Engine() {
        handle_ = auraio_create();
        if (!handle_) {
            throw Error(errno, "auraio_create");
        }
        pool_ = std::make_unique<detail::CallbackPool>(handle_);
    }

    /**
     * Create engine with custom options
     * @param opts Configuration options
     * @throws Error on failure
     */
    explicit Engine(const Options& opts) {
        handle_ = auraio_create_with_options(&opts.c_options());
        if (!handle_) {
            throw Error(errno, "auraio_create_with_options");
        }
        pool_ = std::make_unique<detail::CallbackPool>(handle_);
    }

    /**
     * Move constructor
     */
    Engine(Engine&& other) noexcept
        : handle_(other.handle_)
        , pool_(std::move(other.pool_))
    {
        other.handle_ = nullptr;
    }

    /**
     * Move assignment
     */
    Engine& operator=(Engine&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = other.handle_;
            pool_ = std::move(other.pool_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    // Non-copyable
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    /**
     * Destructor
     */
    ~Engine() {
        destroy();
    }

    // =========================================================================
    // Core I/O Operations
    // =========================================================================

    /**
     * Submit async read operation
     *
     * @tparam F Callback type (must satisfy Callback concept)
     * @param fd File descriptor
     * @param buf Buffer to read into
     * @param len Number of bytes to read
     * @param offset File offset
     * @param callback Completion callback
     * @return Request handle (valid until callback begins)
     * @throws Error on submission failure
     */
    template<Callback F>
    Request* read(int fd, BufferRef buf, size_t len, off_t offset, F&& callback) {
        auto* ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auraio_request_t* req = auraio_read(
            handle_, fd, buf.c_buf(), len, offset,
            detail::callback_trampoline, ctx
        );

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, "auraio_read");
        }

        // Set up O(1) cleanup via on_complete callback
        ctx->on_complete = [this, ctx]() {
            pool_->release(ctx);
        };

        ctx->request = Request(req);
        return &ctx->request;
    }

    /**
     * Submit async write operation
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param buf Buffer to write from
     * @param len Number of bytes to write
     * @param offset File offset
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template<Callback F>
    Request* write(int fd, BufferRef buf, size_t len, off_t offset, F&& callback) {
        auto* ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auraio_request_t* req = auraio_write(
            handle_, fd, buf.c_buf(), len, offset,
            detail::callback_trampoline, ctx
        );

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, "auraio_write");
        }

        ctx->on_complete = [this, ctx]() {
            pool_->release(ctx);
        };
        ctx->request = Request(req);
        return &ctx->request;
    }

    /**
     * Submit async vectored read operation
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param iov IO vector array
     * @param offset File offset
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template<Callback F>
    Request* readv(int fd, std::span<const iovec> iov, off_t offset, F&& callback) {
        auto* ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auraio_request_t* req = auraio_readv(
            handle_, fd, iov.data(), static_cast<int>(iov.size()), offset,
            detail::callback_trampoline, ctx
        );

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, "auraio_readv");
        }

        ctx->on_complete = [this, ctx]() {
            pool_->release(ctx);
        };
        ctx->request = Request(req);
        return &ctx->request;
    }

    /**
     * Submit async vectored write operation
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param iov IO vector array
     * @param offset File offset
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template<Callback F>
    Request* writev(int fd, std::span<const iovec> iov, off_t offset, F&& callback) {
        auto* ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auraio_request_t* req = auraio_writev(
            handle_, fd, iov.data(), static_cast<int>(iov.size()), offset,
            detail::callback_trampoline, ctx
        );

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, "auraio_writev");
        }

        ctx->on_complete = [this, ctx]() {
            pool_->release(ctx);
        };
        ctx->request = Request(req);
        return &ctx->request;
    }

    /**
     * Submit async fsync operation
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template<Callback F>
    Request* fsync(int fd, F&& callback) {
        auto* ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auraio_request_t* req = auraio_fsync(
            handle_, fd, detail::callback_trampoline, ctx
        );

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, "auraio_fsync");
        }

        ctx->on_complete = [this, ctx]() {
            pool_->release(ctx);
        };
        ctx->request = Request(req);
        return &ctx->request;
    }

    /**
     * Submit async fdatasync operation
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template<Callback F>
    Request* fdatasync(int fd, F&& callback) {
        auto* ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auraio_request_t* req = auraio_fsync_ex(
            handle_, fd, AURAIO_FSYNC_DATASYNC,
            detail::callback_trampoline, ctx
        );

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, "auraio_fsync_ex");
        }

        ctx->on_complete = [this, ctx]() {
            pool_->release(ctx);
        };
        ctx->request = Request(req);
        return &ctx->request;
    }

    // =========================================================================
    // Coroutine I/O Operations
    // =========================================================================

    /**
     * Async read operation for coroutines
     *
     * @param fd File descriptor
     * @param buf Buffer to read into
     * @param len Number of bytes to read
     * @param offset File offset
     * @return Awaitable that yields ssize_t (bytes read or negative error)
     */
    inline IoAwaitable async_read(int fd, BufferRef buf, size_t len, off_t offset);

    /**
     * Async write operation for coroutines
     *
     * @param fd File descriptor
     * @param buf Buffer to write from
     * @param len Number of bytes to write
     * @param offset File offset
     * @return Awaitable that yields ssize_t (bytes written or negative error)
     */
    inline IoAwaitable async_write(int fd, BufferRef buf, size_t len, off_t offset);

    /**
     * Async fsync operation for coroutines
     *
     * @param fd File descriptor
     * @return Awaitable that completes when fsync is done
     */
    inline FsyncAwaitable async_fsync(int fd);

    /**
     * Async fdatasync operation for coroutines
     *
     * @param fd File descriptor
     * @return Awaitable that completes when fdatasync is done
     */
    inline FsyncAwaitable async_fdatasync(int fd);

    // =========================================================================
    // Cancellation
    // =========================================================================

    /**
     * Attempt to cancel a pending request
     *
     * @param req Request to cancel
     * @return True if cancellation was submitted
     */
    bool cancel(Request* req) noexcept {
        if (!req || !req->handle()) {
            return false;
        }
        return auraio_cancel(handle_, req->handle()) == 0;
    }

    // =========================================================================
    // Event Processing
    // =========================================================================

    /**
     * Get file descriptor for poll/epoll integration
     *
     * @return Pollable file descriptor
     * @throws Error on failure
     */
    [[nodiscard]] int poll_fd() const {
        int fd = auraio_get_poll_fd(handle_);
        if (fd < 0) {
            throw Error(errno, "auraio_get_poll_fd");
        }
        return fd;
    }

    /**
     * Process completions (non-blocking)
     *
     * @return Number of completions processed
     */
    int poll() {
        return auraio_poll(handle_);
    }

    /**
     * Wait for completions
     *
     * @param timeout_ms Maximum wait time (-1 = forever, 0 = don't block)
     * @return Number of completions processed
     * @throws Error on failure
     */
    int wait(int timeout_ms = -1) {
        int n = auraio_wait(handle_, timeout_ms);
        if (n < 0) {
            throw Error(errno, "auraio_wait");
        }
        return n;
    }

    /**
     * Run event loop until stop() is called
     *
     * Blocks the calling thread. Call stop() from a callback
     * or another thread to exit.
     */
    void run() {
        auraio_run(handle_);
    }

    /**
     * Signal event loop to stop
     *
     * Thread-safe. Can be called from callbacks.
     */
    void stop() noexcept {
        auraio_stop(handle_);
    }

    // =========================================================================
    // Buffer Management
    // =========================================================================

    /**
     * Allocate page-aligned buffer from pool
     *
     * @param size Buffer size
     * @return RAII buffer object
     * @throws Error on allocation failure
     */
    [[nodiscard]] Buffer allocate_buffer(size_t size) {
        void* ptr = auraio_buffer_alloc(handle_, size);
        if (!ptr) {
            throw Error(errno, "auraio_buffer_alloc");
        }
        return Buffer(handle_, ptr, size);
    }

    /**
     * Register buffers with kernel for zero-copy I/O
     *
     * @param bufs Buffer descriptors
     * @throws Error on failure
     */
    void register_buffers(std::span<const iovec> bufs) {
        if (auraio_register_buffers(handle_, bufs.data(), static_cast<int>(bufs.size())) != 0) {
            throw Error(errno, "auraio_register_buffers");
        }
    }

    /**
     * Unregister buffers
     *
     * @throws Error on failure
     */
    void unregister_buffers() {
        if (auraio_unregister_buffers(handle_) != 0) {
            throw Error(errno, "auraio_unregister_buffers");
        }
    }

    // =========================================================================
    // File Registration
    // =========================================================================

    /**
     * Register file descriptors with kernel
     *
     * @param fds File descriptors to register
     * @throws Error on failure
     */
    void register_files(std::span<const int> fds) {
        if (auraio_register_files(handle_, fds.data(), static_cast<int>(fds.size())) != 0) {
            throw Error(errno, "auraio_register_files");
        }
    }

    /**
     * Update registered file descriptor
     *
     * @param index Slot index
     * @param fd New file descriptor (-1 to unregister slot)
     * @throws Error on failure
     */
    void update_file(int index, int fd) {
        if (auraio_update_file(handle_, index, fd) != 0) {
            throw Error(errno, "auraio_update_file");
        }
    }

    /**
     * Unregister all files
     *
     * @throws Error on failure
     */
    void unregister_files() {
        if (auraio_unregister_files(handle_) != 0) {
            throw Error(errno, "auraio_unregister_files");
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * Get engine statistics snapshot
     *
     * @return Stats object with current metrics
     */
    [[nodiscard]] Stats get_stats() const {
        Stats stats;
        auraio_get_stats(handle_, &stats.stats_);
        return stats;
    }

    /**
     * Get the number of io_uring rings
     * @return Number of rings
     */
    [[nodiscard]] int ring_count() const noexcept {
        return auraio_get_ring_count(handle_);
    }

    /**
     * Get per-ring statistics
     * @param ring_idx Ring index (0 to ring_count()-1)
     * @return RingStats snapshot
     * @throws std::out_of_range if ring_idx is invalid
     */
    [[nodiscard]] RingStats get_ring_stats(int ring_idx) const {
        RingStats rs;
        if (auraio_get_ring_stats(handle_, ring_idx, &rs.stats_) != 0)
            throw std::out_of_range("ring_idx out of range");
        rs.ring_idx_ = ring_idx;
        return rs;
    }

    /**
     * Get latency histogram snapshot for a ring
     * @param ring_idx Ring index (0 to ring_count()-1)
     * @return Histogram snapshot
     * @throws std::out_of_range if ring_idx is invalid
     */
    [[nodiscard]] Histogram get_histogram(int ring_idx) const {
        Histogram h;
        if (auraio_get_histogram(handle_, ring_idx, &h.hist_) != 0)
            throw std::out_of_range("ring_idx out of range");
        return h;
    }

    /**
     * Get buffer pool statistics
     * @return BufferStats snapshot
     */
    [[nodiscard]] BufferStats get_buffer_stats() const {
        BufferStats bs;
        auraio_get_buffer_stats(handle_, &bs.stats_);
        return bs;
    }

    // =========================================================================
    // Raw Access
    // =========================================================================

    /**
     * Get underlying C engine handle
     * @return Pointer to auraio_engine_t
     */
    [[nodiscard]] auraio_engine_t* handle() noexcept { return handle_; }

    /**
     * Get underlying C engine handle (const)
     * @return Pointer to const auraio_engine_t
     */
    [[nodiscard]] const auraio_engine_t* handle() const noexcept { return handle_; }

    /**
     * Check if engine is valid
     * @return True if handle is non-null
     */
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    void destroy() noexcept {
        if (handle_) {
            auraio_destroy(handle_);
            handle_ = nullptr;
        }
        pool_.reset();
    }

    auraio_engine_t* handle_ = nullptr;
    std::unique_ptr<detail::CallbackPool> pool_;
};

} // namespace auraio

#endif // AURAIO_ENGINE_HPP
