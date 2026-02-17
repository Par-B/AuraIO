/**
 * @file test_cpp.cpp
 * @brief Unit tests for AuraIO C++ bindings
 */

#include <aura.hpp>

#include <string>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <cassert>
#include <array>
#include <atomic>
#include <chrono>
#include <type_traits>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) void test_##name()
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  %-40s", #name);                                                                  \
        fflush(stdout);                                                                            \
        try {                                                                                      \
            test_##name();                                                                         \
            printf(" OK\n");                                                                       \
            tests_passed++;                                                                        \
        } catch (const std::exception &e) {                                                        \
            printf(" FAIL: %s\n", e.what());                                                       \
            tests_failed++;                                                                        \
        } catch (...) {                                                                            \
            printf(" FAIL: unknown exception\n");                                                  \
            tests_failed++;                                                                        \
        }                                                                                          \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            throw std::runtime_error("Assertion failed: " #cond);                                  \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(a, b)                                                                            \
    do {                                                                                           \
        if ((a) != (b)) {                                                                          \
            throw std::runtime_error("Assertion failed: " #a " == " #b);                           \
        }                                                                                          \
    } while (0)

#define ASSERT_NE(a, b)                                                                            \
    do {                                                                                           \
        if ((a) == (b)) {                                                                          \
            throw std::runtime_error("Assertion failed: " #a " != " #b);                           \
        }                                                                                          \
    } while (0)

#define ASSERT_GT(a, b)                                                                            \
    do {                                                                                           \
        if (!((a) > (b))) {                                                                        \
            throw std::runtime_error("Assertion failed: " #a " > " #b);                            \
        }                                                                                          \
    } while (0)

#define ASSERT_GE(a, b)                                                                            \
    do {                                                                                           \
        if (!((a) >= (b))) {                                                                       \
            throw std::runtime_error("Assertion failed: " #a " >= " #b);                           \
        }                                                                                          \
    } while (0)

#define ASSERT_THROWS(expr, exc_type)                                                              \
    do {                                                                                           \
        bool caught = false;                                                                       \
        try {                                                                                      \
            expr;                                                                                  \
        } catch (const exc_type &) {                                                               \
            caught = true;                                                                         \
        } catch (...) {                                                                            \
        }                                                                                          \
        if (!caught) {                                                                             \
            throw std::runtime_error("Expected exception " #exc_type " not thrown");               \
        }                                                                                          \
    } while (0)

// =============================================================================
// Helper: create a temporary file with test data
// =============================================================================

class TempFile {
  public:
    TempFile(size_t size = 4096) {
        snprintf(path_, sizeof(path_), "/tmp/aura_test_XXXXXX");
        fd_ = mkstemp(path_);
        if (fd_ < 0) {
            throw std::runtime_error("Failed to create temp file");
        }

        // Write test pattern (aligned for O_DIRECT)
        std::vector<char> buf(size, 'A');
        for (size_t i = 0; i < size; i++) {
            buf[i] = static_cast<char>('A' + (i % 26));
        }
        ssize_t n = ::write(fd_, buf.data(), size);
        if (n != static_cast<ssize_t>(size)) {
            throw std::runtime_error("Failed to write test data");
        }
        fsync(fd_);
    }

    ~TempFile() {
        if (fd_ >= 0) close(fd_);
        unlink(path_);
    }

    int fd() const { return fd_; }
    const char *path() const { return path_; }

    // Reopen with flags (e.g., O_DIRECT)
    int reopen(int flags) {
        if (fd_ >= 0) close(fd_);
        fd_ = open(path_, flags);
        return fd_;
    }

  private:
    char path_[64];
    int fd_ = -1;
};

// =============================================================================
// Engine Tests
// =============================================================================

TEST(engine_default_construct) {
    aura::Engine engine;
    ASSERT(engine.handle() != nullptr);
}

TEST(engine_with_options) {
    aura::Options opts;
    opts.queue_depth(64).ring_count(2).disable_adaptive(false);

    aura::Engine engine(opts);
    ASSERT(engine.handle() != nullptr);
}

TEST(engine_movable) {
    static_assert(std::is_move_constructible_v<aura::Engine>,
                  "Engine must be move-constructible");
    static_assert(std::is_move_assignable_v<aura::Engine>, "Engine must be move-assignable");
    static_assert(!std::is_copy_constructible_v<aura::Engine>,
                  "Engine must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<aura::Engine>, "Engine must not be copy-assignable");

    // Verify move constructor works
    aura::Engine engine1;
    auto *handle = engine1.handle();
    aura::Engine engine2(std::move(engine1));
    ASSERT(engine2.handle() == handle);
    ASSERT(engine1.handle() == nullptr);
}

TEST(engine_poll_fd_valid) {
    aura::Engine engine;
    int fd = engine.poll_fd();
    ASSERT_GE(fd, 0);
}

// =============================================================================
// Options Tests
// =============================================================================

TEST(options_default_values) {
    aura::Options opts;
    const auto &c = opts.c_options();

    // Verify defaults match aura_options_init
    ASSERT_EQ(c.queue_depth, 256);
    ASSERT_EQ(c.ring_count, 0); // 0 = auto-detect
    ASSERT_EQ(c.disable_adaptive, false);
}

TEST(options_builder_chain) {
    aura::Options opts;

    // Verify chaining returns reference to same object
    auto &ref = opts.queue_depth(128);
    ASSERT_EQ(&ref, &opts);

    ref.ring_count(2).disable_adaptive(true);
    ASSERT_EQ(opts.c_options().queue_depth, 128);
    ASSERT_EQ(opts.c_options().ring_count, 2);
    ASSERT_EQ(opts.c_options().disable_adaptive, true);
}

TEST(options_all_setters) {
    aura::Options opts;
    opts.queue_depth(512)
        .ring_count(8)
        .initial_in_flight(16)
        .min_in_flight(4)
        .max_p99_latency_ms(5.0)
        .disable_adaptive(false)
        .enable_sqpoll(false);

    const auto &c = opts.c_options();
    ASSERT_EQ(c.queue_depth, 512);
    ASSERT_EQ(c.ring_count, 8);
    ASSERT_EQ(c.initial_in_flight, 16);
    ASSERT_EQ(c.min_in_flight, 4);
    ASSERT(c.max_p99_latency_ms > 4.9 && c.max_p99_latency_ms < 5.1);
    ASSERT_EQ(c.disable_adaptive, false);
    ASSERT_EQ(c.enable_sqpoll, false);
}

TEST(options_ring_select) {
    // Default should be Adaptive
    aura::Options opts;
    ASSERT_EQ(opts.ring_select(), aura::RingSelect::Adaptive);

    // Round-trip each mode
    opts.ring_select(aura::RingSelect::CpuLocal);
    ASSERT_EQ(opts.ring_select(), aura::RingSelect::CpuLocal);

    opts.ring_select(aura::RingSelect::RoundRobin);
    ASSERT_EQ(opts.ring_select(), aura::RingSelect::RoundRobin);

    opts.ring_select(aura::RingSelect::Adaptive);
    ASSERT_EQ(opts.ring_select(), aura::RingSelect::Adaptive);

    // Verify C struct mapping
    ASSERT_EQ(opts.c_options().ring_select, AURA_SELECT_ADAPTIVE);
}

// =============================================================================
// Buffer Tests
// =============================================================================

TEST(buffer_allocate) {
    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    ASSERT_NE(buffer.data(), nullptr);
    ASSERT_EQ(buffer.size(), 4096u);

    // Should be aligned for O_DIRECT
    ASSERT_EQ(reinterpret_cast<uintptr_t>(buffer.data()) % 4096, 0u);
}

TEST(buffer_move_construct) {
    aura::Engine engine;
    auto buffer1 = engine.allocate_buffer(4096);
    void *ptr = buffer1.data();

    aura::Buffer buffer2(std::move(buffer1));
    ASSERT_EQ(buffer2.data(), ptr);
    ASSERT_EQ(buffer1.data(), nullptr);
}

TEST(buffer_move_assign) {
    aura::Engine engine;
    auto buffer1 = engine.allocate_buffer(4096);
    auto buffer2 = engine.allocate_buffer(4096);

    void *ptr1 = buffer1.data();
    buffer2 = std::move(buffer1);

    ASSERT_EQ(buffer2.data(), ptr1);
    ASSERT_EQ(buffer1.data(), nullptr);
}

TEST(buffer_wrap_no_free) {
    aura::Engine engine;
    alignas(4096) char stack_buf[4096];

    auto buffer = aura::Buffer::wrap(stack_buf, sizeof(stack_buf));
    ASSERT_EQ(buffer.data(), static_cast<void *>(stack_buf));
    ASSERT_EQ(buffer.size(), sizeof(stack_buf));
    // Destructor should not try to free stack memory
}

TEST(buffer_span_access) {
    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    auto span = buffer.span();
    ASSERT_EQ(span.size(), 4096u);
    ASSERT_EQ(span.data(), static_cast<std::byte *>(buffer.data()));
}

// =============================================================================
// BufferRef Tests
// =============================================================================

TEST(bufferref_from_ptr) {
    char buf[64];
    aura::BufferRef ref(buf);

    auto c = ref.c_buf();
    ASSERT_EQ(c.u.ptr, static_cast<void *>(buf));
    ASSERT_EQ(c.type, AURA_BUF_UNREGISTERED);
}

TEST(bufferref_from_buffer) {
    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    aura::BufferRef ref = buffer; // Implicit conversion
    auto c = ref.c_buf();
    ASSERT_EQ(c.u.ptr, buffer.data());
}

TEST(bufferref_fixed) {
    auto ref = aura::BufferRef::fixed(5, 128);
    auto c = ref.c_buf();

    ASSERT_EQ(c.type, AURA_BUF_REGISTERED);
    ASSERT_EQ(c.u.fixed.index, 5);
    ASSERT_EQ(c.u.fixed.offset, 128u);
}

// =============================================================================
// Error Tests
// =============================================================================

TEST(error_from_errno) {
    aura::Error err(EINVAL, "test operation");
    ASSERT_EQ(err.code(), EINVAL);
}

TEST(error_what_message) {
    aura::Error err(ENOENT, "open file");
    std::string msg = err.what();

    ASSERT(msg.find("open file") != std::string::npos);
    ASSERT(msg.find("No such file") != std::string::npos ||
           msg.find("ENOENT") != std::string::npos);
}

TEST(error_predicates) {
    ASSERT(aura::Error(EINVAL).is_invalid());
    ASSERT(aura::Error(EAGAIN).is_again());
    ASSERT(aura::Error(ENOENT).is_not_found());
    ASSERT(aura::Error(ECANCELED).is_cancelled());
    ASSERT(aura::Error(EBUSY).is_busy());
}

// =============================================================================
// Request Tests
// =============================================================================

TEST(request_null_handle) {
    aura::Request req(nullptr);
    ASSERT(!req);
    ASSERT_EQ(req.fd(), -1);
    ASSERT(!req.pending());
}

// =============================================================================
// Stats Tests
// =============================================================================

TEST(stats_all_fields) {
    aura::Engine engine;
    auto stats = engine.get_stats();

    // Initial stats should be zero/near-zero
    ASSERT_GE(stats.ops_completed(), 0);
    ASSERT_GE(stats.bytes_transferred(), 0);
    ASSERT_GE(stats.throughput_bps(), 0.0);
    ASSERT_GE(stats.p99_latency_ms(), 0.0);
    ASSERT_GE(stats.current_in_flight(), 0);
    ASSERT_GT(stats.optimal_in_flight(), 0);
}

// =============================================================================
// Registration Tests
// =============================================================================

TEST(request_unregister_reg_buffers) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto fixed_storage = engine.allocate_buffer(4096);
    std::array<iovec, 1> iovs = {iovec{fixed_storage.data(), fixed_storage.size()}};
    engine.register_buffers(iovs);

    std::atomic<bool> completed{false};
    std::atomic<ssize_t> read_result{0};
    (void)engine.read(file.fd(), aura::BufferRef::fixed(0, 0), 4096, 0,
                      [&](aura::Request &, ssize_t result) {
                          read_result.store(result, std::memory_order_seq_cst);
                          completed.store(true, std::memory_order_seq_cst);
                      });

    engine.request_unregister(AURA_REG_BUFFERS);

    bool rejected_during_drain = false;
    try {
        (void)engine.read(file.fd(), aura::BufferRef::fixed(0, 0), 64, 0,
                          [](aura::Request &, ssize_t) {});
    } catch (const aura::Error &e) {
        rejected_during_drain = e.is_busy() || e.is_not_found();
    }
    ASSERT(rejected_during_drain);

    while (!completed.load(std::memory_order_seq_cst)) {
        engine.wait(100);
    }
    ASSERT_EQ(read_result.load(std::memory_order_seq_cst), 4096);

    bool got_not_found = false;
    try {
        (void)engine.read(file.fd(), aura::BufferRef::fixed(0, 0), 64, 0,
                          [](aura::Request &, ssize_t) {});
    } catch (const aura::Error &e) {
        got_not_found = e.is_not_found();
    }
    ASSERT(got_not_found);

    /* No-op once already finalized */
    engine.unregister(AURA_REG_BUFFERS);
}

TEST(request_unregister_reg_files) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    std::array<int, 1> fds = {file.fd()};
    engine.register_files(fds);

    engine.request_unregister(AURA_REG_FILES);

    bool rejected_update = false;
    try {
        engine.update_file(0, file.fd());
    } catch (const aura::Error &e) {
        rejected_update = e.is_busy() || e.is_not_found();
    }
    ASSERT(rejected_update);

    /* No-op once already finalized */
    engine.unregister(AURA_REG_FILES);

    /* Registration should still work after deferred path */
    engine.register_files(fds);
    engine.unregister(AURA_REG_FILES);
}

// =============================================================================
// I/O Tests (Callback-based)
// =============================================================================

TEST(read_basic) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    bool completed = false;
    ssize_t bytes_read = 0;

    (void)engine.read(file.fd(), buffer, 4096, 0, [&](aura::Request &, ssize_t result) {
        completed = true;
        bytes_read = result;
    });

    engine.wait();
    ASSERT(completed);
    ASSERT_EQ(bytes_read, 4096);

    // Verify data
    char *data = static_cast<char *>(buffer.data());
    ASSERT_EQ(data[0], 'A');
    ASSERT_EQ(data[1], 'B');
}

TEST(read_with_capture) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    std::string captured_message = "not set";
    int captured_value = 0;

    (void)engine.read(file.fd(), buffer, 4096, 0,
                      [&captured_message, &captured_value](aura::Request &, ssize_t result) {
                          captured_message = "completed";
                          captured_value = static_cast<int>(result);
                      });

    engine.wait();
    ASSERT_EQ(captured_message, "completed");
    ASSERT_EQ(captured_value, 4096);
}

TEST(write_basic) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    // Fill buffer with pattern
    std::memset(buffer.data(), 'X', 4096);

    bool completed = false;
    ssize_t bytes_written = 0;

    (void)engine.write(file.fd(), buffer, 4096, 0, [&](aura::Request &, ssize_t result) {
        completed = true;
        bytes_written = result;
    });

    engine.wait();
    ASSERT(completed);
    ASSERT_EQ(bytes_written, 4096);
}

TEST(fsync_basic) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    bool completed = false;

    (void)engine.fsync(file.fd(), [&](aura::Request &, ssize_t result) {
        completed = true;
        ASSERT_EQ(result, 0);
    });

    engine.wait();
    ASSERT(completed);
}

TEST(fdatasync_basic) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    bool completed = false;

    (void)engine.fdatasync(file.fd(), [&](aura::Request &, ssize_t result) {
        completed = true;
        ASSERT_EQ(result, 0);
    });

    engine.wait();
    ASSERT(completed);
}

TEST(readv_basic) {
    TempFile file(8192);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buf1 = engine.allocate_buffer(4096);
    auto buf2 = engine.allocate_buffer(4096);

    iovec iov[2] = {{buf1.data(), 4096}, {buf2.data(), 4096}};

    bool completed = false;
    ssize_t total_read = 0;

    (void)engine.readv(file.fd(), std::span<const iovec>(iov, 2), 0,
                       [&](aura::Request &, ssize_t result) {
                           completed = true;
                           total_read = result;
                       });

    engine.wait();
    ASSERT(completed);
    ASSERT_EQ(total_read, 8192);
}

TEST(writev_basic) {
    TempFile file(8192);
    file.reopen(O_RDWR);

    aura::Engine engine;
    auto buf1 = engine.allocate_buffer(4096);
    auto buf2 = engine.allocate_buffer(4096);

    std::memset(buf1.data(), 'Y', 4096);
    std::memset(buf2.data(), 'Z', 4096);

    iovec iov[2] = {{buf1.data(), 4096}, {buf2.data(), 4096}};

    bool completed = false;
    ssize_t total_written = 0;

    (void)engine.writev(file.fd(), std::span<const iovec>(iov, 2), 0,
                        [&](aura::Request &, ssize_t result) {
                            completed = true;
                            total_written = result;
                        });

    engine.wait();
    ASSERT(completed);
    ASSERT_EQ(total_written, 8192);
}

// =============================================================================
// Event Loop Tests
// =============================================================================

TEST(poll_processes_completions) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    bool completed = false;
    (void)engine.read(file.fd(), buffer, 4096, 0,
                      [&](aura::Request &, ssize_t) { completed = true; });

    // Poll until complete (bounded to prevent hang)
    for (int i = 0; i < 5000 && !completed; i++) {
        engine.poll();
        usleep(1000);
    }

    ASSERT(completed);
}

TEST(wait_blocks) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    bool completed = false;
    (void)engine.read(file.fd(), buffer, 4096, 0,
                      [&](aura::Request &, ssize_t) { completed = true; });

    // wait() should block until at least one completion
    int n = engine.wait();
    ASSERT_GT(n, 0);
    ASSERT(completed);
}

// Note: this test relies on 20ms sleeps to observe lock contention ordering.
// May be flaky under heavy system load but is not a correctness issue.
TEST(run_serializes_with_poll) {
    aura::Engine engine;
    std::atomic<bool> poll_returned{false};

    std::thread run_thread([&]() { engine.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    std::thread poll_thread([&]() {
        (void)engine.poll();
        poll_returned.store(true, std::memory_order_seq_cst);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT(!poll_returned.load(std::memory_order_seq_cst));

    engine.stop();
    run_thread.join();
    poll_thread.join();
    ASSERT(poll_returned.load(std::memory_order_seq_cst));
}

TEST(multiple_concurrent_ops) {
    TempFile file(16384);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buf1 = engine.allocate_buffer(4096);
    auto buf2 = engine.allocate_buffer(4096);
    auto buf3 = engine.allocate_buffer(4096);
    auto buf4 = engine.allocate_buffer(4096);

    int completed = 0;

    (void)engine.read(file.fd(), buf1, 4096, 0, [&](aura::Request &, ssize_t) { completed++; });
    (void)engine.read(file.fd(), buf2, 4096, 4096,
                      [&](aura::Request &, ssize_t) { completed++; });
    (void)engine.read(file.fd(), buf3, 4096, 8192,
                      [&](aura::Request &, ssize_t) { completed++; });
    (void)engine.read(file.fd(), buf4, 4096, 12288,
                      [&](aura::Request &, ssize_t) { completed++; });

    // Wait for all (bounded to prevent hang)
    for (int i = 0; i < 100 && completed < 4; i++) {
        engine.wait(100);
    }

    ASSERT_EQ(completed, 4);
}

// =============================================================================
// RAII Tests
// =============================================================================

TEST(raii_engine_scope) {
    aura_engine_t *raw = nullptr;
    {
        aura::Engine engine;
        raw = engine.handle();
        ASSERT_NE(raw, nullptr);
    }
    // Engine destroyed here - we can't safely check raw handle
    // But if we got here without crash, cleanup worked
}

TEST(raii_buffer_scope) {
    aura::Engine engine;
    {
        auto buffer = engine.allocate_buffer(4096);
        ASSERT_NE(buffer.data(), nullptr);
    }
    // Buffer returned to pool here
    // Allocate another - should work
    auto buffer2 = engine.allocate_buffer(4096);
    ASSERT_NE(buffer2.data(), nullptr);
}

TEST(raii_exception_safety) {
    aura::Engine engine;

    try {
        auto buffer = engine.allocate_buffer(4096);
        throw std::runtime_error("simulated exception");
    } catch (const std::runtime_error &) {
        // Buffer should be cleaned up
    }

    // Engine should still work
    auto buffer2 = engine.allocate_buffer(4096);
    ASSERT_NE(buffer2.data(), nullptr);
}

TEST(raii_buffer_outlives_engine_scope) {
    aura::Buffer escaped;
    {
        aura::Engine engine;
        escaped = engine.allocate_buffer(4096);
        ASSERT_NE(escaped.data(), nullptr);
    }
    /* Engine is destroyed here. Buffer cleanup must remain safe. */
    ASSERT_NE(escaped.data(), nullptr);
}

// =============================================================================
// Error Path Tests
// =============================================================================

TEST(read_invalid_fd_throws) {
    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    bool threw = false;
    try {
        (void)engine.read(-1, buffer, 4096, 0, [](aura::Request &, ssize_t) {});
    } catch (const aura::Error &e) {
        threw = true;
        // fd=-1 may produce EBADF or EINVAL depending on kernel/io_uring version
        ASSERT(e.code() == EBADF || e.code() == EINVAL);
    }
    ASSERT(threw);
}

TEST(buffer_as_null_throws) {
    aura::Buffer buf; // Default-constructed — null
    bool threw = false;
    try {
        (void)buf.as<int>();
    } catch (const aura::Error &e) {
        threw = true;
        ASSERT_EQ(e.code(), EINVAL);
    }
    ASSERT(threw);
}

// =============================================================================
// Coroutine Tests
// =============================================================================

#if __has_include(<coroutine>)

aura::Task<ssize_t> async_read_task(aura::Engine &engine, int fd, aura::Buffer &buf,
                                      size_t len, off_t off) {
    ssize_t result = co_await engine.async_read(fd, buf, len, off);
    co_return result;
}

aura::Task<void> async_write_task(aura::Engine &engine, int fd, aura::Buffer &buf, size_t len,
                                    off_t off) {
    co_await engine.async_write(fd, buf, len, off);
}

TEST(async_read_basic) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    auto task = async_read_task(engine, file.fd(), buffer, 4096, 0);

    // Start the coroutine (it will suspend on I/O)
    task.resume();

    // Wait for completion - the I/O callback will resume the coroutine
    while (!task.done()) {
        engine.wait(100);
    }

    ssize_t result = task.get();
    ASSERT_EQ(result, 4096);
}

TEST(async_write_basic) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);
    std::memset(buffer.data(), 'W', 4096);

    auto task = async_write_task(engine, file.fd(), buffer, 4096, 0);

    // Start the coroutine
    task.resume();

    // Wait for completion
    while (!task.done()) {
        engine.wait(100);
    }

    // If we get here without exception, write succeeded
    task.get(); // May throw if there was an error
}

aura::Task<void> async_fsync_task(aura::Engine &engine, int fd) {
    co_await engine.async_fsync(fd);
}

TEST(async_fsync_basic) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    auto task = async_fsync_task(engine, file.fd());

    // Start the coroutine
    task.resume();

    // Wait for completion
    while (!task.done()) {
        engine.wait(100);
    }

    task.get();
}

aura::Task<ssize_t> async_sequential_reads(aura::Engine &engine, int fd, aura::Buffer &buf) {
    ssize_t total = 0;
    total += co_await engine.async_read(fd, buf, 1024, 0);
    total += co_await engine.async_read(fd, buf, 1024, 1024);
    total += co_await engine.async_read(fd, buf, 1024, 2048);
    co_return total;
}

TEST(coroutine_sequential) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    auto task = async_sequential_reads(engine, file.fd(), buffer);

    // Start the coroutine
    task.resume();

    // Wait for completion - each I/O in the sequence will complete
    // and the callback will resume the coroutine
    while (!task.done()) {
        engine.wait(100);
    }

    ssize_t total = task.get();
    ASSERT_EQ(total, 3072);
}

// --- Lifecycle coroutine tests ---

aura::Task<int> async_openat_task(aura::Engine &engine, const char *path) {
    int fd = co_await engine.async_openat(AT_FDCWD, path, O_RDONLY, 0);
    co_return fd;
}

TEST(async_openat_basic) {
    TempFile file(4096);

    aura::Engine engine;
    auto task = async_openat_task(engine, file.path());

    task.resume();
    while (!task.done()) {
        engine.wait(100);
    }

    int fd = task.get();
    ASSERT_GE(fd, 0);
    ::close(fd);
}

aura::Task<void> async_close_task(aura::Engine &engine, int fd) {
    co_await engine.async_close(fd);
}

TEST(async_close_basic) {
    TempFile file(4096);
    int fd = ::open(file.path(), O_RDONLY);
    ASSERT_GE(fd, 0);

    aura::Engine engine;
    auto task = async_close_task(engine, fd);

    task.resume();
    while (!task.done()) {
        engine.wait(100);
    }

    task.get();
    // fd should now be closed - verify by trying to fstat
    struct stat st;
    ASSERT_EQ(fstat(fd, &st), -1);
}

aura::Task<void> async_fallocate_task(aura::Engine &engine, int fd, off_t len) {
    co_await engine.async_fallocate(fd, 0, 0, len);
}

TEST(async_fallocate_basic) {
    TempFile file(0); // start empty
    file.reopen(O_RDWR);

    aura::Engine engine;
    auto task = async_fallocate_task(engine, file.fd(), 8192);

    task.resume();
    while (!task.done()) {
        engine.wait(100);
    }

    task.get();

    // Verify allocation
    struct stat st;
    ASSERT_EQ(fstat(file.fd(), &st), 0);
    ASSERT_GE(st.st_size, 8192);
}

#endif // __has_include(<coroutine>)

// =============================================================================
// Lifecycle callback tests
// =============================================================================

TEST(openat_close_callback) {
    TempFile file(4096);

    aura::Engine engine;
    bool open_done = false;
    int opened_fd = -1;

    (void)engine.openat(AT_FDCWD, file.path(), O_RDONLY, 0,
                        [&](aura::Request &, ssize_t result) {
                            opened_fd = static_cast<int>(result);
                            open_done = true;
                        });

    while (!open_done) {
        engine.wait(100);
    }
    ASSERT_GE(opened_fd, 0);

    // Now close it async
    bool close_done = false;
    ssize_t close_result = -1;
    (void)engine.close(opened_fd, [&](aura::Request &, ssize_t result) {
        close_result = result;
        close_done = true;
    });

    while (!close_done) {
        engine.wait(100);
    }
    ASSERT_EQ(close_result, 0);
}

TEST(statx_callback) {
    TempFile file(4096);

    aura::Engine engine;
    bool done = false;
    struct statx stx {};

    (void)engine.statx(AT_FDCWD, file.path(), 0, AURA_STATX_SIZE, &stx,
                       [&](aura::Request &, ssize_t result) {
                           (void)result;
                           done = true;
                       });

    while (!done) {
        engine.wait(100);
    }
    ASSERT_EQ(static_cast<off_t>(stx.stx_size), 4096);
}

TEST(fallocate_callback) {
    TempFile file(0);
    file.reopen(O_RDWR);

    aura::Engine engine;
    bool done = false;
    ssize_t alloc_result = -1;

    (void)engine.fallocate(file.fd(), 0, 0, 16384,
                           [&](aura::Request &, ssize_t result) {
                               alloc_result = result;
                               done = true;
                           });

    while (!done) {
        engine.wait(100);
    }
    ASSERT_EQ(alloc_result, 0);

    struct stat st;
    ASSERT_EQ(fstat(file.fd(), &st), 0);
    ASSERT_GE(st.st_size, 16384);
}

TEST(ftruncate_callback) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    bool done = false;
    ssize_t trunc_result = -1;

    (void)engine.ftruncate(file.fd(), 1024,
                           [&](aura::Request &, ssize_t result) {
                               trunc_result = result;
                               done = true;
                           });

    while (!done) {
        engine.wait(100);
    }

    // ftruncate requires kernel 6.9+; allow ENOSYS
    if (trunc_result == 0) {
        struct stat st;
        ASSERT_EQ(fstat(file.fd(), &st), 0);
        ASSERT_EQ(st.st_size, 1024);
    } else {
        // Accept -ENOSYS on older kernels
        ASSERT_EQ(trunc_result, -static_cast<ssize_t>(ENOSYS));
    }
}

TEST(sync_file_range_callback) {
    TempFile file(4096);
    file.reopen(O_RDWR);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);
    std::memset(buffer.data(), 'X', 4096);

    // Write some data first
    bool write_done = false;
    (void)engine.write(file.fd(), buffer, 4096, 0,
                       [&](aura::Request &, ssize_t) { write_done = true; });
    while (!write_done) {
        engine.wait(100);
    }

    // Now sync the range
    bool done = false;
    ssize_t sync_result = -1;
    (void)engine.sync_file_range(file.fd(), 0, 4096,
                                 AURA_SYNC_RANGE_WRITE | AURA_SYNC_RANGE_WAIT_AFTER,
                                 [&](aura::Request &, ssize_t result) {
                                     sync_result = result;
                                     done = true;
                                 });

    while (!done) {
        engine.wait(100);
    }
    ASSERT_EQ(sync_result, 0);
}

// =============================================================================
// Minor accessor tests
// =============================================================================

TEST(request_op_type) {
    TempFile file(4096);
    file.reopen(O_RDONLY);

    aura::Engine engine;
    auto buffer = engine.allocate_buffer(4096);

    int observed_op = -1;
    (void)engine.read(file.fd(), buffer, 4096, 0,
                      [&](aura::Request &req, ssize_t) {
                          observed_op = req.op_type();
                      });

    engine.wait();
    ASSERT_EQ(observed_op, AURA_OP_READ);
}

TEST(options_single_thread) {
    aura::Options opts;
    const auto &copts = opts;
    ASSERT(!copts.single_thread());

    opts.single_thread(true);
    ASSERT(copts.single_thread());

    opts.single_thread(false);
    ASSERT(!copts.single_thread());
}

TEST(stats_peak_in_flight) {
    aura::Engine engine;
    auto stats = engine.get_stats();
    // Just verify the accessor compiles and returns something reasonable
    ASSERT_GE(stats.peak_in_flight(), 0);

    auto rs = engine.get_ring_stats(0);
    ASSERT_GE(rs.peak_in_flight(), 0);
}

TEST(version_functions) {
    const char *ver = aura::version();
    ASSERT(ver != nullptr);
    ASSERT(std::strlen(ver) > 0);

    int ver_int = aura::version_int();
    ASSERT_GT(ver_int, 0);
}

// =============================================================================
// Main
// =============================================================================

int main() {
    printf("Running C++ binding tests...\n");

    RUN_TEST(engine_default_construct);
    RUN_TEST(engine_with_options);
    RUN_TEST(engine_movable);
    RUN_TEST(engine_poll_fd_valid);
    RUN_TEST(options_default_values);
    RUN_TEST(options_builder_chain);
    RUN_TEST(options_all_setters);
    RUN_TEST(options_ring_select);
    RUN_TEST(buffer_allocate);
    RUN_TEST(buffer_move_construct);
    RUN_TEST(buffer_move_assign);
    RUN_TEST(buffer_wrap_no_free);
    RUN_TEST(buffer_span_access);
    RUN_TEST(bufferref_from_ptr);
    RUN_TEST(bufferref_from_buffer);
    RUN_TEST(bufferref_fixed);
    RUN_TEST(error_from_errno);
    RUN_TEST(error_what_message);
    RUN_TEST(error_predicates);
    RUN_TEST(request_null_handle);
    RUN_TEST(stats_all_fields);
    RUN_TEST(request_unregister_reg_buffers);
    RUN_TEST(request_unregister_reg_files);
    RUN_TEST(read_basic);
    RUN_TEST(read_with_capture);
    RUN_TEST(write_basic);
    RUN_TEST(fsync_basic);
    RUN_TEST(fdatasync_basic);
    RUN_TEST(readv_basic);
    RUN_TEST(writev_basic);
    RUN_TEST(poll_processes_completions);
    RUN_TEST(wait_blocks);
    RUN_TEST(run_serializes_with_poll);
    RUN_TEST(multiple_concurrent_ops);
    RUN_TEST(raii_engine_scope);
    RUN_TEST(raii_buffer_scope);
    RUN_TEST(raii_exception_safety);
    RUN_TEST(raii_buffer_outlives_engine_scope);
    RUN_TEST(read_invalid_fd_throws);
    RUN_TEST(buffer_as_null_throws);

#if __has_include(<coroutine>)
    RUN_TEST(async_read_basic);
    RUN_TEST(async_write_basic);
    RUN_TEST(async_fsync_basic);
    RUN_TEST(coroutine_sequential);
    RUN_TEST(async_openat_basic);
    RUN_TEST(async_close_basic);
    RUN_TEST(async_fallocate_basic);
#endif

    RUN_TEST(openat_close_callback);
    RUN_TEST(statx_callback);
    RUN_TEST(fallocate_callback);
    RUN_TEST(ftruncate_callback);
    RUN_TEST(sync_file_range_callback);
    RUN_TEST(request_op_type);
    RUN_TEST(options_single_thread);
    RUN_TEST(stats_peak_in_flight);
    RUN_TEST(version_functions);

    if (tests_failed > 0) {
        printf("\n%d tests passed, %d FAILED\n", tests_passed, tests_failed);
    } else {
        printf("\n%d tests passed\n", tests_passed);
    }

    return tests_failed > 0 ? 1 : 0;
}
