// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file coro.hpp
 * @brief C++20 coroutine support for AuraIO
 *
 * @warning COROUTINE LIFETIME REQUIREMENT
 * The Task object must remain alive until the coroutine completes. Destroying
 * a Task while an async I/O operation is pending results in undefined behavior
 * (the callback will attempt to resume a destroyed coroutine handle).
 *
 * Safe usage:
 * @code
 * Task<int> my_task = do_async_io(engine);
 * while (!my_task.done()) {
 *     engine.wait();  // Process completions
 *     my_task.resume();
 * }
 * int result = my_task.get();
 * @endcode
 *
 * Unsafe - DO NOT DO THIS:
 * @code
 * {
 *     Task<int> task = do_async_io(engine);
 *     task.resume();  // Starts I/O
 * }  // Task destroyed while I/O still pending - UNDEFINED BEHAVIOR
 * @endcode
 */

#ifndef AURA_CORO_HPP
#define AURA_CORO_HPP

#include <aura/fwd.hpp>
#include <aura/error.hpp>
#include <aura/buffer.hpp>
#include <aura/request.hpp>

#include <atomic>
#include <coroutine>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

namespace aura {

/**
 * Simple task type for coroutines
 *
 * Represents a lazy coroutine that produces a value of type T.
 * For void tasks, use Task<void> or just Task<>.
 *
 * Example:
 * @code
 * Task<int> read_file(Engine& engine, int fd) {
 *     auto buf = engine.allocate_buffer(4096);
 *     ssize_t n = co_await engine.async_read(fd, buf, 4096, 0);
 *     co_return n;
 * }
 * @endcode
 */
template <typename T> class Task {
  public:
    struct promise_type {
        std::variant<std::monostate, T, std::exception_ptr> result;
        std::coroutine_handle<> continuation_; /**< Caller to resume on completion */

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation_) {
                    return h.promise().continuation_;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_value(T value) { result = std::move(value); }

        void unhandled_exception() { result = std::current_exception(); }
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    Task(Task &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                if (!handle_.done()) {
                    std::terminate();
                }
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) {
            if (!handle_.done()) {
                // Fatal: destroying a Task with pending I/O causes use-after-free
                // when the callback tries to resume the destroyed coroutine.
                std::terminate();
            }
            handle_.destroy();
        }
    }

    // Non-copyable
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    /**
     * Resume the coroutine
     * @return True if coroutine is still running
     */
    bool resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
            return !handle_.done();
        }
        return false;
    }

    /**
     * Check if coroutine is done
     */
    [[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }

    /**
     * Get the result (after coroutine completes)
     *
     * This is a destructive operation: the result is moved out and
     * subsequent calls will throw std::logic_error("Task result already consumed").
     *
     * @return The value produced by co_return (moved)
     * @throws std::logic_error if the task is not complete or result was already consumed
     * @throws Any exception that was thrown in the coroutine
     */
    T get() {
        if (!handle_ || !handle_.done()) {
            throw std::logic_error("Task not complete");
        }

        auto &result = handle_.promise().result;
        if (std::holds_alternative<std::exception_ptr>(result)) {
            auto ex = std::get<std::exception_ptr>(result);
            result = std::monostate{};
            std::rethrow_exception(ex);
        }
        if (!std::holds_alternative<T>(result)) {
            throw std::logic_error("Task result already consumed");
        }
        T value = std::move(std::get<T>(result));
        result = std::monostate{};
        return value;
    }

    /**
     * Awaiter for co_await on Task
     */
    auto operator co_await() {
        if (!handle_) {
            throw std::logic_error("co_await on empty Task");
        }
        struct Awaiter {
            std::coroutine_handle<promise_type> handle;

            bool await_ready() const noexcept { return handle.done(); }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                handle.promise().continuation_ = caller;
                return handle;
            }

            T await_resume() {
                auto &result = handle.promise().result;
                if (std::holds_alternative<std::exception_ptr>(result)) {
                    auto ex = std::get<std::exception_ptr>(result);
                    result = std::monostate{};
                    std::rethrow_exception(ex);
                }
                if (!std::holds_alternative<T>(result)) {
                    throw std::logic_error("Task result not available");
                }
                T value = std::move(std::get<T>(result));
                result = std::monostate{};
                return value;
            }
        };
        return Awaiter{handle_};
    }

  private:
    std::coroutine_handle<promise_type> handle_;
};

/**
 * Specialization for void tasks
 */
template <> class Task<void> {
  public:
    struct promise_type {
        std::exception_ptr exception;
        std::coroutine_handle<> continuation_; /**< Caller to resume on completion */

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                if (h.promise().continuation_) {
                    return h.promise().continuation_;
                }
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }

        void return_void() {}

        void unhandled_exception() { exception = std::current_exception(); }
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    Task(Task &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    Task &operator=(Task &&other) noexcept {
        if (this != &other) {
            if (handle_) {
                if (!handle_.done()) {
                    std::terminate();
                }
                handle_.destroy();
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) {
            if (!handle_.done()) {
                std::terminate();
            }
            handle_.destroy();
        }
    }

    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;

    bool resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
            return !handle_.done();
        }
        return false;
    }

    [[nodiscard]] bool done() const noexcept { return !handle_ || handle_.done(); }

    void get() {
        if (!handle_ || !handle_.done()) {
            throw std::logic_error("Task not complete");
        }
        if (handle_.promise().exception) {
            auto ex = handle_.promise().exception;
            handle_.promise().exception = nullptr;
            std::rethrow_exception(ex);
        }
    }

    auto operator co_await() {
        if (!handle_) {
            throw std::logic_error("co_await on empty Task");
        }
        struct Awaiter {
            std::coroutine_handle<promise_type> handle;

            bool await_ready() const noexcept { return handle.done(); }

            std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
                handle.promise().continuation_ = caller;
                return handle;
            }

            void await_resume() {
                if (handle.promise().exception) {
                    auto ex = handle.promise().exception;
                    handle.promise().exception = nullptr;
                    std::rethrow_exception(ex);
                }
            }
        };
        return Awaiter{handle_};
    }

  private:
    std::coroutine_handle<promise_type> handle_;
};

/**
 * Awaitable for async I/O operations
 *
 * Returned by Engine::async_read, async_write, async_fsync.
 *
 * @warning Do NOT store IoAwaitable objects. They must be consumed immediately
 * via co_await. Storing an IoAwaitable and using it after Engine destruction
 * is undefined behavior. The bare Engine& reference is safe only because
 * awaitables are transient co_await-only objects created and consumed within
 * a single co_await expression.
 *
 * @warning Do NOT call Engine::wait(), poll(), or run() from within a
 * coroutine resumed by a completion callback. The callback holds the
 * event_loop_mutex_, so re-entering the event loop will deadlock.
 */
class IoAwaitable {
    friend class Engine;

    IoAwaitable(Engine &engine, int fd, BufferRef buf, size_t len, off_t offset, bool is_write)
        : engine_(engine), fd_(fd), buf_(buf), len_(len), offset_(offset), is_write_(is_write) {}

  public:
    IoAwaitable(const IoAwaitable &) = delete;
    IoAwaitable &operator=(const IoAwaitable &) = delete;
    IoAwaitable(IoAwaitable &&) = delete;
    IoAwaitable &operator=(IoAwaitable &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    ssize_t await_resume() {
        if (result_ < 0) {
            // Cast to unsigned before negating to avoid signed overflow UB on SSIZE_MIN.
            // Kernel error codes are small positive integers (1-4095).
            auto pos = static_cast<size_t>(-static_cast<std::make_unsigned_t<ssize_t>>(result_));
            int err = (pos >= 1 && pos <= 4095) ? static_cast<int>(pos) : EIO;
            throw Error(err, is_write_ ? "async_write" : "async_read");
        }
        return result_;
    }

  private:
    Engine &engine_;
    int fd_;
    BufferRef buf_;
    size_t len_;
    off_t offset_;
    bool is_write_;
    ssize_t result_ = 0;
    Request request_{nullptr};           ///< Stored for potential cancellation
    std::atomic<bool> completed_{false}; ///< Race coordination: callback vs await_suspend
};

/**
 * Awaitable for async fsync operations
 */
class FsyncAwaitable {
    friend class Engine;

    FsyncAwaitable(Engine &engine, int fd, bool datasync = false)
        : engine_(engine), fd_(fd), datasync_(datasync) {}

  public:
    FsyncAwaitable(const FsyncAwaitable &) = delete;
    FsyncAwaitable &operator=(const FsyncAwaitable &) = delete;
    FsyncAwaitable(FsyncAwaitable &&) = delete;
    FsyncAwaitable &operator=(FsyncAwaitable &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    void await_resume() {
        if (result_ < 0) {
            auto pos = static_cast<size_t>(-static_cast<std::make_unsigned_t<ssize_t>>(result_));
            int err = (pos >= 1 && pos <= 4095) ? static_cast<int>(pos) : EIO;
            throw Error(err, datasync_ ? "async_fdatasync" : "async_fsync");
        }
    }

  private:
    Engine &engine_;
    int fd_;
    bool datasync_;
    ssize_t result_ = 0;
    Request request_{nullptr};           ///< Stored for potential cancellation
    std::atomic<bool> completed_{false}; ///< Race coordination: callback vs await_suspend
};

/**
 * Awaitable for async openat operations
 *
 * co_await yields int (new fd), or throws aura::Error on failure.
 */
class OpenatAwaitable {
    friend class Engine;

    OpenatAwaitable(Engine &engine, int dirfd, const char *pathname, int flags, mode_t mode)
        : engine_(engine), dirfd_(dirfd), pathname_(pathname), flags_(flags), mode_(mode) {}

  public:
    OpenatAwaitable(const OpenatAwaitable &) = delete;
    OpenatAwaitable &operator=(const OpenatAwaitable &) = delete;
    OpenatAwaitable(OpenatAwaitable &&) = delete;
    OpenatAwaitable &operator=(OpenatAwaitable &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    int await_resume() {
        if (result_ < 0) {
            auto pos = static_cast<size_t>(-static_cast<std::make_unsigned_t<ssize_t>>(result_));
            int err = (pos >= 1 && pos <= 4095) ? static_cast<int>(pos) : EIO;
            throw Error(err, "async_openat");
        }
        return static_cast<int>(result_);
    }

  private:
    Engine &engine_;
    int dirfd_;
    const char *pathname_;
    int flags_;
    mode_t mode_;
    ssize_t result_ = 0;
    Request request_{nullptr};
    std::atomic<bool> completed_{false};
};

/**
 * Awaitable for async metadata operations (close, fallocate, ftruncate, sync_file_range)
 *
 * co_await returns void, or throws aura::Error on failure.
 */
class MetadataAwaitable {
    friend class Engine;

    enum class Op { Close, Fallocate, Ftruncate, SyncFileRange };

    // Close
    MetadataAwaitable(Engine &engine, int fd, Op op) : engine_(engine), fd_(fd), op_(op) {}

    // Fallocate / Ftruncate / SyncFileRange
    MetadataAwaitable(Engine &engine, int fd, Op op, int mode, off_t offset, off_t len,
                      unsigned int flags)
        : engine_(engine), fd_(fd), op_(op), mode_(mode), offset_(offset), len_(len),
          flags_(flags) {}

  public:
    MetadataAwaitable(const MetadataAwaitable &) = delete;
    MetadataAwaitable &operator=(const MetadataAwaitable &) = delete;
    MetadataAwaitable(MetadataAwaitable &&) = delete;
    MetadataAwaitable &operator=(MetadataAwaitable &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    void await_resume() {
        if (result_ < 0) {
            auto pos = static_cast<size_t>(-static_cast<std::make_unsigned_t<ssize_t>>(result_));
            int err = (pos >= 1 && pos <= 4095) ? static_cast<int>(pos) : EIO;
            const char *name = "async_metadata";
            switch (op_) {
            case Op::Close:
                name = "async_close";
                break;
            case Op::Fallocate:
                name = "async_fallocate";
                break;
            case Op::Ftruncate:
                name = "async_ftruncate";
                break;
            case Op::SyncFileRange:
                name = "async_sync_file_range";
                break;
            }
            throw Error(err, name);
        }
    }

  private:
    Engine &engine_;
    int fd_;
    Op op_;
    int mode_ = 0;
    off_t offset_ = 0;
    off_t len_ = 0;
    unsigned int flags_ = 0;
    ssize_t result_ = 0;
    Request request_{nullptr};
    std::atomic<bool> completed_{false};
};

#ifdef __linux__
/**
 * Awaitable for async statx operations
 *
 * co_await returns void (fills statxbuf), or throws aura::Error on failure.
 */
class StatxAwaitable {
    friend class Engine;

    StatxAwaitable(Engine &engine, int dirfd, const char *pathname, int flags, unsigned int mask,
                   struct statx *statxbuf)
        : engine_(engine), dirfd_(dirfd), pathname_(pathname), flags_(flags), mask_(mask),
          statxbuf_(statxbuf) {}

  public:
    StatxAwaitable(const StatxAwaitable &) = delete;
    StatxAwaitable &operator=(const StatxAwaitable &) = delete;
    StatxAwaitable(StatxAwaitable &&) = delete;
    StatxAwaitable &operator=(StatxAwaitable &&) = delete;

    bool await_ready() const noexcept { return false; }

    bool await_suspend(std::coroutine_handle<> handle);

    void await_resume() {
        if (result_ < 0) {
            auto pos = static_cast<size_t>(-static_cast<std::make_unsigned_t<ssize_t>>(result_));
            int err = (pos >= 1 && pos <= 4095) ? static_cast<int>(pos) : EIO;
            throw Error(err, "async_statx");
        }
    }

  private:
    Engine &engine_;
    int dirfd_;
    const char *pathname_;
    int flags_;
    unsigned int mask_;
    struct statx *statxbuf_;
    ssize_t result_ = 0;
    Request request_{nullptr};
    std::atomic<bool> completed_{false};
};
#endif

} // namespace aura

// Include engine.hpp for the await_suspend implementations
// (must be after the awaitable declarations)
#include <aura/engine.hpp>

namespace aura {

inline bool IoAwaitable::await_suspend(std::coroutine_handle<> handle) {
    try {
        auto callback = [this, handle](Request &, ssize_t result) {
            result_ = result;
            // If await_suspend already committed (set completed_=true),
            // we are responsible for resuming. Otherwise await_suspend
            // will see completed_=true and return false (don't suspend).
            if (completed_.exchange(true, std::memory_order_acq_rel)) {
                handle.resume();
            }
        };
        if (is_write_) {
            request_ = engine_.write(fd_, buf_, len_, offset_, std::move(callback));
        } else {
            request_ = engine_.read(fd_, buf_, len_, offset_, std::move(callback));
        }
        // If callback already fired, don't suspend (return false).
        // acq_rel ensures result_ written by callback is visible.
        return !completed_.exchange(true, std::memory_order_acq_rel);
    } catch (const Error &e) {
        result_ = -e.code();
        return false;
    }
}

inline bool FsyncAwaitable::await_suspend(std::coroutine_handle<> handle) {
    try {
        auto callback = [this, handle](Request &, ssize_t result) {
            result_ = result;
            if (completed_.exchange(true, std::memory_order_acq_rel)) {
                handle.resume();
            }
        };
        if (datasync_) {
            request_ = engine_.fdatasync(fd_, std::move(callback));
        } else {
            request_ = engine_.fsync(fd_, std::move(callback));
        }
        return !completed_.exchange(true, std::memory_order_acq_rel);
    } catch (const Error &e) {
        result_ = -e.code();
        return false;
    }
}

// Engine async method implementations
inline IoAwaitable Engine::async_read(int fd, BufferRef buf, size_t len, off_t offset) {
    return IoAwaitable(*this, fd, buf, len, offset, false);
}

inline IoAwaitable Engine::async_write(int fd, BufferRef buf, size_t len, off_t offset) {
    return IoAwaitable(*this, fd, buf, len, offset, true);
}

inline FsyncAwaitable Engine::async_fsync(int fd) {
    return FsyncAwaitable(*this, fd, false);
}

inline FsyncAwaitable Engine::async_fdatasync(int fd) {
    return FsyncAwaitable(*this, fd, true);
}

// --- Lifecycle awaitable await_suspend implementations ---

inline bool OpenatAwaitable::await_suspend(std::coroutine_handle<> handle) {
    try {
        auto callback = [this, handle](Request &, ssize_t result) {
            result_ = result;
            if (completed_.exchange(true, std::memory_order_acq_rel)) {
                handle.resume();
            }
        };
        request_ = engine_.openat(dirfd_, pathname_, flags_, mode_, std::move(callback));
        return !completed_.exchange(true, std::memory_order_acq_rel);
    } catch (const Error &e) {
        result_ = -e.code();
        return false;
    }
}

inline bool MetadataAwaitable::await_suspend(std::coroutine_handle<> handle) {
    try {
        auto callback = [this, handle](Request &, ssize_t result) {
            result_ = result;
            if (completed_.exchange(true, std::memory_order_acq_rel)) {
                handle.resume();
            }
        };
        switch (op_) {
        case Op::Close:
            request_ = engine_.close(fd_, std::move(callback));
            break;
        case Op::Fallocate:
            request_ = engine_.fallocate(fd_, mode_, offset_, len_, std::move(callback));
            break;
        case Op::Ftruncate:
            request_ = engine_.ftruncate(fd_, len_, std::move(callback));
            break;
        case Op::SyncFileRange:
            request_ = engine_.sync_file_range(fd_, offset_, len_, flags_, std::move(callback));
            break;
        }
        return !completed_.exchange(true, std::memory_order_acq_rel);
    } catch (const Error &e) {
        result_ = -e.code();
        return false;
    }
}

#ifdef __linux__
inline bool StatxAwaitable::await_suspend(std::coroutine_handle<> handle) {
    try {
        auto callback = [this, handle](Request &, ssize_t result) {
            result_ = result;
            if (completed_.exchange(true, std::memory_order_acq_rel)) {
                handle.resume();
            }
        };
        request_ = engine_.statx(dirfd_, pathname_, flags_, mask_, statxbuf_, std::move(callback));
        return !completed_.exchange(true, std::memory_order_acq_rel);
    } catch (const Error &e) {
        result_ = -e.code();
        return false;
    }
}
#endif

// --- Engine lifecycle async method implementations ---

inline OpenatAwaitable Engine::async_openat(int dirfd, const char *pathname, int flags,
                                            mode_t mode) {
    return OpenatAwaitable(*this, dirfd, pathname, flags, mode);
}

inline MetadataAwaitable Engine::async_close(int fd) {
    return MetadataAwaitable(*this, fd, MetadataAwaitable::Op::Close);
}

#ifdef __linux__
inline StatxAwaitable Engine::async_statx(int dirfd, const char *pathname, int flags,
                                          unsigned int mask, struct statx *statxbuf) {
    return StatxAwaitable(*this, dirfd, pathname, flags, mask, statxbuf);
}
#endif

inline MetadataAwaitable Engine::async_fallocate(int fd, int mode, off_t offset, off_t len) {
    return MetadataAwaitable(*this, fd, MetadataAwaitable::Op::Fallocate, mode, offset, len, 0);
}

inline MetadataAwaitable Engine::async_ftruncate(int fd, off_t length) {
    return MetadataAwaitable(*this, fd, MetadataAwaitable::Op::Ftruncate, 0, 0, length, 0);
}

inline MetadataAwaitable Engine::async_sync_file_range(int fd, off_t offset, off_t nbytes,
                                                       unsigned int flags) {
    return MetadataAwaitable(*this, fd, MetadataAwaitable::Op::SyncFileRange, 0, offset, nbytes,
                             flags);
}

} // namespace aura

#endif // AURA_CORO_HPP
