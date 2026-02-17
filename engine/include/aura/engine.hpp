/**
 * @file engine.hpp
 * @brief Main Engine class for AuraIO C++ bindings
 */

#ifndef AURA_ENGINE_HPP
#define AURA_ENGINE_HPP

#include <aura.h>
#include <aura/fwd.hpp>
#include <aura/error.hpp>
#include <aura/options.hpp>
#include <aura/buffer.hpp>
#include <aura/request.hpp>
#include <aura/stats.hpp>
#include <aura/detail/callback_storage.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <utility>
#include <atomic>

namespace aura {

// Forward declarations for coroutine support
class IoAwaitable;
class FsyncAwaitable;

/**
 * Callback concept for I/O completion handlers
 */
template <typename F>
concept Callback = std::invocable<F, Request &, ssize_t>;

/**
 * Main AuraIO engine class
 *
 * Manages io_uring rings and provides async I/O operations.
 * Move-only (cannot be copied). Movable when no event loop methods
 * (poll/wait/run/drain) are executing on another thread.
 *
 * @par Exception Safety in Callbacks
 * Exceptions thrown from completion callbacks will call std::terminate().
 * The C FFI trampoline cannot propagate exceptions across the C boundary.
 * Always catch all exceptions inside callbacks.
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
 * aura::Engine engine;
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
        handle_ = aura_create();
        if (!handle_) {
            throw Error(errno, "aura_create");
        }
        try {
            engine_alive_ = std::make_shared<std::atomic<bool>>(true);
            pool_ = std::make_unique<detail::CallbackPool>(handle_);
            event_loop_mutex_ = std::make_unique<std::mutex>();
        } catch (...) {
            aura_destroy(handle_);
            handle_ = nullptr;
            engine_alive_.reset();
            throw;
        }
    }

    /**
     * Create engine with custom options
     * @param opts Configuration options
     * @throws Error on failure
     */
    explicit Engine(const Options &opts) {
        handle_ = aura_create_with_options(&opts.c_options());
        if (!handle_) {
            throw Error(errno, "aura_create_with_options");
        }
        try {
            engine_alive_ = std::make_shared<std::atomic<bool>>(true);
            pool_ = std::make_unique<detail::CallbackPool>(handle_);
            event_loop_mutex_ = std::make_unique<std::mutex>();
        } catch (...) {
            aura_destroy(handle_);
            handle_ = nullptr;
            engine_alive_.reset();
            throw;
        }
    }

    // Non-copyable.
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    /**
     * Move constructor
     *
     * The source engine is left in a valid but empty state (handle_ == nullptr).
     * Must NOT be called while event loop methods (poll/wait/run/drain) are
     * executing on another thread — the moved-from mutex becomes null.
     */
    Engine(Engine &&other) noexcept
        : handle_(other.handle_), pool_(std::move(other.pool_)),
          engine_alive_(std::move(other.engine_alive_)),
          event_loop_mutex_(std::move(other.event_loop_mutex_)) {
        other.handle_ = nullptr;
    }

    /**
     * Move assignment operator
     *
     * Destroys the current engine (draining pending I/O) before taking
     * ownership of the source. Same threading constraint as move constructor.
     */
    Engine &operator=(Engine &&other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = other.handle_;
            pool_ = std::move(other.pool_);
            engine_alive_ = std::move(other.engine_alive_);
            event_loop_mutex_ = std::move(other.event_loop_mutex_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    /**
     * Destructor
     */
    ~Engine() { destroy(); }

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
    template <Callback F>
    [[nodiscard]] Request read(int fd, BufferRef buf, size_t len, off_t offset, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_read", [&](auto *ctx) {
            return aura_read(handle_, fd, buf.c_buf(), len, offset, aura_detail_callback_trampoline,
                             ctx);
        });
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
    template <Callback F>
    [[nodiscard]] Request write(int fd, BufferRef buf, size_t len, off_t offset, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_write", [&](auto *ctx) {
            return aura_write(handle_, fd, buf.c_buf(), len, offset,
                              aura_detail_callback_trampoline, ctx);
        });
    }

    /**
     * Submit async vectored read operation
     *
     * @warning The iovec array AND the buffers it points to must remain
     *          valid until the completion callback fires.  The kernel
     *          reads the iovec at submission time, but writes into the
     *          buffers asynchronously.
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param iov IO vector array
     * @param offset File offset
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F>
    [[nodiscard]] Request readv(int fd, std::span<const iovec> iov, off_t offset, F &&callback) {
        if (iov.size() > static_cast<size_t>(INT_MAX)) {
            throw Error(EINVAL, "iov count exceeds INT_MAX");
        }
        return submit_io(std::forward<F>(callback), "aura_readv", [&](auto *ctx) {
            return aura_readv(handle_, fd, iov.data(), static_cast<int>(iov.size()), offset,
                              aura_detail_callback_trampoline, ctx);
        });
    }

    /**
     * Submit async vectored write operation
     *
     * @warning The iovec array AND the buffers it points to must remain
     *          valid until the completion callback fires.  The kernel
     *          reads from the buffers asynchronously.
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param iov IO vector array
     * @param offset File offset
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F>
    [[nodiscard]] Request writev(int fd, std::span<const iovec> iov, off_t offset, F &&callback) {
        if (iov.size() > static_cast<size_t>(INT_MAX)) {
            throw Error(EINVAL, "iov count exceeds INT_MAX");
        }
        return submit_io(std::forward<F>(callback), "aura_writev", [&](auto *ctx) {
            return aura_writev(handle_, fd, iov.data(), static_cast<int>(iov.size()), offset,
                               aura_detail_callback_trampoline, ctx);
        });
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
    template <Callback F> [[nodiscard]] Request fsync(int fd, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_fsync", [&](auto *ctx) {
            return aura_fsync(handle_, fd, AURA_FSYNC_DEFAULT, aura_detail_callback_trampoline,
                              ctx);
        });
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
    template <Callback F> [[nodiscard]] Request fdatasync(int fd, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_fsync", [&](auto *ctx) {
            return aura_fsync(handle_, fd, AURA_FSYNC_DATASYNC, aura_detail_callback_trampoline,
                              ctx);
        });
    }

    // =========================================================================
    // Lifecycle Metadata Operations
    // =========================================================================

    /**
     * Submit async openat operation
     *
     * Opens a file relative to a directory fd. The callback receives the
     * new file descriptor as the result (>= 0 on success, negative errno
     * on failure). The pathname must remain valid until the callback fires.
     *
     * @tparam F Callback type
     * @param dirfd Directory fd (AT_FDCWD for current directory)
     * @param pathname Path to file (relative to dirfd)
     * @param flags Open flags (O_RDONLY, O_WRONLY, O_CREAT, etc.)
     * @param mode File mode (used when O_CREAT is set)
     * @param callback Completion callback (result is new fd or negative errno)
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F>
    [[nodiscard]] Request openat(int dirfd, const char *pathname, int flags, mode_t mode,
                                 F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_openat", [&](auto *ctx) {
            return aura_openat(handle_, dirfd, pathname, flags, mode,
                               aura_detail_callback_trampoline, ctx);
        });
    }

    /**
     * Submit async close operation
     *
     * @tparam F Callback type
     * @param fd File descriptor to close
     * @param callback Completion callback (result is 0 or negative errno)
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F> [[nodiscard]] Request close(int fd, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_close", [&](auto *ctx) {
            return aura_close(handle_, fd, aura_detail_callback_trampoline, ctx);
        });
    }

#ifdef __linux__
    /**
     * Submit async statx operation
     *
     * Retrieves file metadata. Both pathname and statxbuf must remain
     * valid until the callback fires.
     *
     * @tparam F Callback type
     * @param dirfd Directory fd (AT_FDCWD for current directory)
     * @param pathname Path (relative to dirfd)
     * @param flags Lookup flags (AURA_STATX_EMPTY_PATH, AURA_STATX_SYMLINK_NOFOLLOW)
     * @param mask Requested fields (AURA_STATX_SIZE, AURA_STATX_MTIME, etc.)
     * @param statxbuf Output buffer
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F>
    [[nodiscard]] Request statx(int dirfd, const char *pathname, int flags, unsigned int mask,
                                struct statx *statxbuf, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_statx", [&](auto *ctx) {
            return aura_statx(handle_, dirfd, pathname, flags, mask, statxbuf,
                              aura_detail_callback_trampoline, ctx);
        });
    }
#endif

    /**
     * Submit async fallocate operation
     *
     * Preallocates or deallocates file space.
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param mode Allocation mode (0, FALLOC_FL_KEEP_SIZE, etc.)
     * @param offset Starting byte offset
     * @param len Length of region
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F>
    [[nodiscard]] Request fallocate(int fd, int mode, off_t offset, off_t len, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_fallocate", [&](auto *ctx) {
            return aura_fallocate(handle_, fd, mode, offset, len, aura_detail_callback_trampoline,
                                  ctx);
        });
    }

    /**
     * Submit async ftruncate operation
     *
     * Truncates a file to the specified length. Requires kernel 6.9+.
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param length New file length
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F> [[nodiscard]] Request ftruncate(int fd, off_t length, F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_ftruncate", [&](auto *ctx) {
            return aura_ftruncate(handle_, fd, length, aura_detail_callback_trampoline, ctx);
        });
    }

    /**
     * Submit async sync_file_range operation
     *
     * Syncs a byte range without flushing metadata.
     *
     * @tparam F Callback type
     * @param fd File descriptor
     * @param offset Starting byte offset
     * @param nbytes Number of bytes to sync (0 = to end of file)
     * @param flags AURA_SYNC_RANGE_WAIT_BEFORE, _WRITE, _WAIT_AFTER
     * @param callback Completion callback
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F>
    [[nodiscard]] Request sync_file_range(int fd, off_t offset, off_t nbytes, unsigned int flags,
                                          F &&callback) {
        return submit_io(std::forward<F>(callback), "aura_sync_file_range", [&](auto *ctx) {
            return aura_sync_file_range(handle_, fd, offset, nbytes, flags,
                                        aura_detail_callback_trampoline, ctx);
        });
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
     * @return Awaitable (co_await yields ssize_t bytes read, or throws aura::Error on failure)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline IoAwaitable async_read(int fd, BufferRef buf, size_t len, off_t offset);

    /**
     * Async write operation for coroutines
     *
     * @param fd File descriptor
     * @param buf Buffer to write from
     * @param len Number of bytes to write
     * @param offset File offset
     * @return Awaitable (co_await yields ssize_t bytes written, or throws aura::Error on failure)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline IoAwaitable async_write(int fd, BufferRef buf, size_t len, off_t offset);

    /**
     * Async fsync operation for coroutines
     *
     * @param fd File descriptor
     * @return Awaitable (co_await returns void, or throws aura::Error on failure)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline FsyncAwaitable async_fsync(int fd);

    /**
     * Async fdatasync operation for coroutines
     *
     * @param fd File descriptor
     * @return Awaitable (co_await returns void, or throws aura::Error on failure)
     */
    inline FsyncAwaitable async_fdatasync(int fd);

    // =========================================================================
    // Coroutine Lifecycle Operations
    // =========================================================================

    /**
     * Async openat for coroutines
     * @return Awaitable (co_await yields int fd, or throws aura::Error)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline OpenatAwaitable async_openat(int dirfd, const char *pathname, int flags, mode_t mode);

    /**
     * Async close for coroutines
     * @return Awaitable (co_await returns void, or throws aura::Error)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline MetadataAwaitable async_close(int fd);

#ifdef __linux__
    /**
     * Async statx for coroutines
     * @return Awaitable (co_await returns void, fills statxbuf, or throws aura::Error)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline StatxAwaitable async_statx(int dirfd, const char *pathname, int flags, unsigned int mask,
                                      struct statx *statxbuf);
#endif

    /**
     * Async fallocate for coroutines
     * @return Awaitable (co_await returns void, or throws aura::Error)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline MetadataAwaitable async_fallocate(int fd, int mode, off_t offset, off_t len);

    /**
     * Async ftruncate for coroutines
     * @return Awaitable (co_await returns void, or throws aura::Error)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline MetadataAwaitable async_ftruncate(int fd, off_t length);

    /**
     * Async sync_file_range for coroutines
     * @return Awaitable (co_await returns void, or throws aura::Error)
     */
    [[nodiscard("must be immediately co_await-ed")]]
    inline MetadataAwaitable async_sync_file_range(int fd, off_t offset, off_t nbytes,
                                                   unsigned int flags);

    // =========================================================================
    // Cancellation
    // =========================================================================

    /**
     * Attempt to cancel a pending request
     *
     * @param req Request to cancel
     * @return True if cancellation was submitted
     */
    bool cancel(Request &req) noexcept {
        if (!req.handle()) {
            return false;
        }
        return aura_cancel(handle_, req.handle()) == 0;
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
        int fd = aura_get_poll_fd(handle_);
        if (fd < 0) {
            throw Error(errno, "aura_get_poll_fd");
        }
        return fd;
    }

    /**
     * Process completions (non-blocking)
     *
     * @return Number of completions processed
     */
    int poll() {
        std::lock_guard<std::mutex> lock(*event_loop_mutex_);
        int n = aura_poll(handle_);
        if (n < 0) {
            throw Error(errno, "aura_poll");
        }
        return n;
    }

    /**
     * Wait for completions
     *
     * @param timeout_ms Maximum wait time (-1 = forever, 0 = don't block)
     * @return Number of completions processed
     * @throws Error on failure
     */
    int wait(int timeout_ms = -1) {
        std::lock_guard<std::mutex> lock(*event_loop_mutex_);
        int n = aura_wait(handle_, timeout_ms);
        if (n < 0) {
            throw Error(errno, "aura_wait");
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
        std::unique_lock<std::mutex> lock(*event_loop_mutex_);
        aura_run(handle_);
    }

    /**
     * Signal event loop to stop
     *
     * Thread-safe. Can be called from callbacks.
     */
    void stop() noexcept { aura_stop(handle_); }

    /**
     * Drain all pending I/O operations
     *
     * Waits until all in-flight operations have completed.
     *
     * @param timeout_ms Maximum wait time (-1 = forever, 0 = non-blocking)
     * @return Total completions processed
     * @throws Error on timeout or failure
     */
    int drain(int timeout_ms = -1) {
        std::lock_guard<std::mutex> lock(*event_loop_mutex_);
        int n = aura_drain(handle_, timeout_ms);
        if (n < 0) {
            throw Error(errno, "aura_drain");
        }
        return n;
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
        void *ptr = aura_buffer_alloc(handle_, size);
        if (!ptr) {
            throw Error(errno, "aura_buffer_alloc");
        }
        return Buffer(handle_, engine_alive_, ptr, size);
    }

    /**
     * Register buffers with kernel for zero-copy I/O
     *
     * @param bufs Buffer descriptors
     * @throws Error on failure
     */
    void register_buffers(std::span<const iovec> bufs) {
        if (bufs.size() > static_cast<size_t>(INT_MAX)) {
            throw Error(EINVAL, "buffer count exceeds INT_MAX");
        }
        if (aura_register_buffers(handle_, bufs.data(), static_cast<int>(bufs.size())) != 0) {
            throw Error(errno, "aura_register_buffers");
        }
    }

    /**
     * Unregister previously registered buffers or files (synchronous)
     *
     * For non-callback callers, waits until in-flight operations using
     * registered resources drain and unregister completes. If called from
     * a completion callback, automatically degrades to the deferred
     * (non-blocking) path.
     *
     * @param type Resource type (AURA_REG_BUFFERS or AURA_REG_FILES)
     * @throws Error on failure
     */
    void unregister(aura_reg_type_t type) {
        if (aura_unregister(handle_, type) != 0) {
            throw Error(errno, "aura_unregister");
        }
    }

    /**
     * Request deferred unregister (callback-safe, non-blocking)
     *
     * Marks registered resources as draining and returns immediately.
     * For buffers: new fixed-buffer submissions fail with EBUSY while
     * draining. Final unregister completes lazily once in-flight
     * operations reach zero.
     *
     * @param type Resource type (AURA_REG_BUFFERS or AURA_REG_FILES)
     * @throws Error on failure
     */
    void request_unregister(aura_reg_type_t type) {
        if (aura_request_unregister(handle_, type) != 0) {
            throw Error(errno, "aura_request_unregister");
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
        if (fds.size() > static_cast<size_t>(INT_MAX)) {
            throw Error(EINVAL, "file count exceeds INT_MAX");
        }
        if (aura_register_files(handle_, fds.data(), static_cast<int>(fds.size())) != 0) {
            throw Error(errno, "aura_register_files");
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
        if (aura_update_file(handle_, index, fd) != 0) {
            throw Error(errno, "aura_update_file");
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * Get engine statistics snapshot
     *
     * @return Stats object with current metrics
     * @throws Error on failure (NULL engine)
     */
    [[nodiscard]] Stats get_stats() const {
        Stats stats;
        if (aura_get_stats(handle_, &stats.stats_, sizeof(stats.stats_)) != 0) {
            throw Error(errno, "aura_get_stats");
        }
        return stats;
    }

    /**
     * Get the number of io_uring rings
     * @return Number of rings
     */
    [[nodiscard]] int ring_count() const noexcept { return aura_get_ring_count(handle_); }

    /**
     * Get per-ring statistics
     * @param ring_idx Ring index (0 to ring_count()-1)
     * @return RingStats snapshot
     * @throws std::out_of_range if ring_idx is invalid
     */
    [[nodiscard]] RingStats get_ring_stats(int ring_idx) const {
        RingStats rs;
        if (aura_get_ring_stats(handle_, ring_idx, &rs.stats_, sizeof(rs.stats_)) != 0)
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
        if (aura_get_histogram(handle_, ring_idx, &h.hist_, sizeof(h.hist_)) != 0)
            throw std::out_of_range("ring_idx out of range");
        return h;
    }

    /**
     * Get buffer pool statistics
     * @return BufferStats snapshot
     */
    [[nodiscard]] BufferStats get_buffer_stats() const noexcept {
        BufferStats bs;
        aura_get_buffer_stats(handle_, &bs.stats_);
        return bs;
    }

    // =========================================================================
    // Diagnostics
    // =========================================================================

    /**
     * Check if the engine has a fatal error
     *
     * Once a fatal error is latched, all subsequent submissions fail with
     * ESHUTDOWN. Use this to distinguish a permanently broken engine from
     * transient EAGAIN.
     *
     * @return 0 if healthy, positive errno value if fatally broken
     * @throws Error if engine handle is NULL
     */
    [[nodiscard]] int get_fatal_error() const {
        int err = aura_get_fatal_error(handle_);
        if (err < 0) {
            throw Error(errno, "aura_get_fatal_error");
        }
        return err;
    }

    /**
     * Check if the current thread is inside a completion callback
     *
     * Useful for choosing between synchronous and deferred code paths
     * (e.g., unregister vs request_unregister).
     *
     * @return true if inside a completion callback, false otherwise
     */
    [[nodiscard]] static bool in_callback_context() noexcept { return aura_in_callback_context(); }

    // =========================================================================
    // Raw Access
    // =========================================================================

    /**
     * Get underlying C engine handle
     * @return Pointer to aura_engine_t
     */
    [[nodiscard]] aura_engine_t *handle() noexcept { return handle_; }

    /**
     * Get underlying C engine handle (const)
     * @return Pointer to const aura_engine_t
     */
    [[nodiscard]] const aura_engine_t *handle() const noexcept { return handle_; }

    /**
     * Check if engine is valid
     * @return True if handle is non-null
     */
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  private:
    /**
     * Generic I/O submission helper - eliminates callback boilerplate
     *
     * @tparam F Callback type
     * @tparam CApiCall Callable that invokes C API and returns aura_request_t*
     * @param callback User callback
     * @param op_name Operation name for error messages (e.g., "aura_read")
     * @param c_api_call Lambda that calls C API with callback context
     * @return Request handle
     * @throws Error on submission failure
     */
    template <Callback F, typename CApiCall>
    [[nodiscard]] Request submit_io(F &&callback, const char *op_name, CApiCall &&c_api_call) {
        auto *ctx = pool_->allocate();
        ctx->callback = std::forward<F>(callback);

        auto *pool_ptr = pool_.get();
        ctx->on_complete = [pool_ptr, ctx]() { pool_ptr->release(ctx); };

        aura_request_t *req = std::forward<CApiCall>(c_api_call)(ctx);

        if (!req) {
            pool_->release(ctx);
            throw Error(errno, op_name);
        }

        return Request(req);
    }

    void destroy() noexcept {
        if (handle_) {
            /* Mark engine as dead BEFORE destroying the C handle.
             * This prevents a race where a concurrent Buffer destructor on
             * another thread sees engine_alive_==true and calls
             * aura_buffer_free() on an already-freed handle.
             * Drain callbacks that free Buffers will use the free() fallback
             * instead of aura_buffer_free() — safe since the pool is about
             * to be destroyed anyway. */
            if (engine_alive_) {
                engine_alive_->store(false, std::memory_order_release);
            }
            aura_destroy(handle_);
            handle_ = nullptr;
        }
        pool_.reset();
        engine_alive_.reset();
    }

    aura_engine_t *handle_ = nullptr;
    std::unique_ptr<detail::CallbackPool> pool_;
    std::shared_ptr<std::atomic<bool>> engine_alive_;
    std::unique_ptr<std::mutex> event_loop_mutex_;
};

// =========================================================================
// Free Functions
// =========================================================================

/**
 * Get library version string
 * @return Version string (e.g., "0.4.0")
 */
[[nodiscard]] inline const char *version() noexcept {
    return aura_version();
}

/**
 * Get library version as integer
 * @return Version integer (major * 10000 + minor * 100 + patch)
 */
[[nodiscard]] inline int version_int() noexcept {
    return aura_version_int();
}

} // namespace aura

#endif // AURA_ENGINE_HPP
