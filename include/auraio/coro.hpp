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

#ifndef AURAIO_CORO_HPP
#define AURAIO_CORO_HPP

#include <auraio/fwd.hpp>
#include <auraio/error.hpp>
#include <auraio/buffer.hpp>
#include <auraio/request.hpp>

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>
#include <variant>

namespace auraio {

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
template<typename T>
class Task {
public:
    struct promise_type {
        std::variant<std::monostate, T, std::exception_ptr> result;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_value(T value) {
            result = std::move(value);
        }

        void unhandled_exception() {
            result = std::current_exception();
        }
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    Task(Task&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) handle_.destroy();
    }

    // Non-copyable
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

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
    [[nodiscard]] bool done() const noexcept {
        return !handle_ || handle_.done();
    }

    /**
     * Get the result (after coroutine completes)
     * @return The value produced by co_return
     * @throws Any exception that was thrown in the coroutine
     */
    T get() {
        if (!handle_.done()) {
            throw std::logic_error("Task not complete");
        }

        auto& result = handle_.promise().result;
        if (std::holds_alternative<std::exception_ptr>(result)) {
            std::rethrow_exception(std::get<std::exception_ptr>(result));
        }
        return std::move(std::get<T>(result));
    }

    /**
     * Awaiter for co_await on Task
     */
    auto operator co_await() {
        struct Awaiter {
            std::coroutine_handle<promise_type> handle;

            bool await_ready() const noexcept { return handle.done(); }

            std::coroutine_handle<> await_suspend([[maybe_unused]] std::coroutine_handle<> caller) noexcept {
                return handle;
            }

            T await_resume() {
                auto& result = handle.promise().result;
                if (std::holds_alternative<std::exception_ptr>(result)) {
                    std::rethrow_exception(std::get<std::exception_ptr>(result));
                }
                return std::move(std::get<T>(result));
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
template<>
class Task<void> {
public:
    struct promise_type {
        std::exception_ptr exception;

        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }

        void return_void() {}

        void unhandled_exception() {
            exception = std::current_exception();
        }
    };

    Task() = default;
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}

    Task(Task&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~Task() {
        if (handle_) handle_.destroy();
    }

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool resume() {
        if (handle_ && !handle_.done()) {
            handle_.resume();
            return !handle_.done();
        }
        return false;
    }

    [[nodiscard]] bool done() const noexcept {
        return !handle_ || handle_.done();
    }

    void get() {
        if (!handle_.done()) {
            throw std::logic_error("Task not complete");
        }
        if (handle_.promise().exception) {
            std::rethrow_exception(handle_.promise().exception);
        }
    }

    auto operator co_await() {
        struct Awaiter {
            std::coroutine_handle<promise_type> handle;

            bool await_ready() const noexcept { return handle.done(); }

            std::coroutine_handle<> await_suspend([[maybe_unused]] std::coroutine_handle<> caller) noexcept {
                return handle;
            }

            void await_resume() {
                if (handle.promise().exception) {
                    std::rethrow_exception(handle.promise().exception);
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
 */
class IoAwaitable {
public:
    IoAwaitable(Engine& engine, int fd, BufferRef buf, size_t len, off_t offset, bool is_write)
        : engine_(engine)
        , fd_(fd)
        , buf_(buf)
        , len_(len)
        , offset_(offset)
        , is_write_(is_write)
    {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle);

    ssize_t await_resume() {
        if (result_ < 0) {
            throw Error(static_cast<int>(-result_), is_write_ ? "async_write" : "async_read");
        }
        return result_;
    }

private:
    Engine& engine_;
    int fd_;
    BufferRef buf_;
    size_t len_;
    off_t offset_;
    bool is_write_;
    ssize_t result_ = 0;
};

/**
 * Awaitable for async fsync operations
 */
class FsyncAwaitable {
public:
    FsyncAwaitable(Engine& engine, int fd, bool datasync = false)
        : engine_(engine)
        , fd_(fd)
        , datasync_(datasync)
    {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle);

    void await_resume() {
        if (result_ < 0) {
            throw Error(static_cast<int>(-result_), datasync_ ? "async_fdatasync" : "async_fsync");
        }
    }

private:
    Engine& engine_;
    int fd_;
    bool datasync_;
    ssize_t result_ = 0;
};

} // namespace auraio

// Include engine.hpp for the await_suspend implementations
// (must be after the awaitable declarations)
#include <auraio/engine.hpp>

namespace auraio {

inline void IoAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (is_write_) {
        engine_.write(fd_, buf_, len_, offset_, [this, handle](Request&, ssize_t result) {
            result_ = result;
            handle.resume();
        });
    } else {
        engine_.read(fd_, buf_, len_, offset_, [this, handle](Request&, ssize_t result) {
            result_ = result;
            handle.resume();
        });
    }
}

inline void FsyncAwaitable::await_suspend(std::coroutine_handle<> handle) {
    if (datasync_) {
        engine_.fdatasync(fd_, [this, handle](Request&, ssize_t result) {
            result_ = result;
            handle.resume();
        });
    } else {
        engine_.fsync(fd_, [this, handle](Request&, ssize_t result) {
            result_ = result;
            handle.resume();
        });
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

} // namespace auraio

#endif // AURAIO_CORO_HPP
