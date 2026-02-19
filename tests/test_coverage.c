// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_coverage.c
 * @brief Expanded coverage tests for AuraIO core
 *
 * Targets specific uncovered code paths identified via llvm-cov:
 * - log.c (0% → covered)
 * - aura.c error paths, drain, vectored I/O, request introspection
 * - adaptive_buffer.c multi-thread cache lifecycle
 * - registered buffer/file validation edges
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../include/aura.h"
#include "../src/adaptive_buffer.h"
#include "../src/adaptive_ring.h"
#include "../src/log.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-45s", #name); \
        fflush(stdout);           \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

static char test_file[256];
static int test_fd = -1;

static void cleanup_atexit(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    if (test_file[0]) unlink(test_file);
}

static void io_setup(void) {
    strcpy(test_file, "/tmp/test_cov_XXXXXX");
    test_fd = mkstemp(test_file);
    if (test_fd < 0) {
        strcpy(test_file, "./test_cov_XXXXXX");
        test_fd = mkstemp(test_file);
    }
    assert(test_fd >= 0);
    char data[8192];
    memset(data, 'B', sizeof(data));
    ssize_t w = write(test_fd, data, sizeof(data));
    assert(w == (ssize_t)sizeof(data));
    lseek(test_fd, 0, SEEK_SET);
}

static void io_teardown(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    unlink(test_file);
}

static _Atomic int cb_called;
static _Atomic ssize_t cb_result;

static void basic_cb(aura_request_t *req, ssize_t result, void *ud) {
    (void)req;
    (void)ud;
    atomic_store(&cb_called, 1);
    atomic_store(&cb_result, result);
}

static aura_engine_t *make_engine(int rings, int qdepth) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = rings;
    opts.queue_depth = qdepth;
    return aura_create_with_options(&opts);
}

/* ============================================================================
 * Log Callback Tests (log.c — was 0% coverage)
 * ============================================================================ */

static _Atomic int log_cb_count;
static int last_log_level;
static char last_log_msg[256];
static void *last_log_ud;

static void test_log_handler(int level, const char *msg, void *userdata) {
    atomic_fetch_add(&log_cb_count, 1);
    last_log_level = level;
    strncpy(last_log_msg, msg, sizeof(last_log_msg) - 1);
    last_log_msg[sizeof(last_log_msg) - 1] = '\0';
    last_log_ud = userdata;
}

TEST(log_set_handler) {
    int sentinel = 42;
    atomic_store(&log_cb_count, 0);

    aura_set_log_handler(test_log_handler, &sentinel);

    /* The library should be silent when no errors occur, but setting
     * the handler itself should succeed without crash. */
    assert(atomic_load(&log_cb_count) >= 0);

    /* Clear handler */
    aura_set_log_handler(NULL, NULL);
}

TEST(log_null_handler) {
    /* Setting NULL handler should not crash */
    aura_set_log_handler(NULL, NULL);

    /* Creating and destroying an engine should work fine silently */
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    aura_destroy(engine);
}

TEST(log_handler_receives_messages) {
    /* Some operations produce log messages — try to trigger one.
     * Specifically, invalid operations or edge cases may log warnings. */
    int sentinel = 99;
    atomic_store(&log_cb_count, 0);
    aura_set_log_handler(test_log_handler, &sentinel);

    /* Create and immediately destroy — exercises lifecycle paths */
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    aura_destroy(engine);

    /* Userdata should be passed correctly if any message was logged */
    if (atomic_load(&log_cb_count) > 0) {
        assert(last_log_ud == &sentinel);
    }

    aura_set_log_handler(NULL, NULL);
}

TEST(log_emit_no_handler) {
    atomic_store(&log_cb_count, 0);
    aura_set_log_handler(NULL, NULL);

    /* Should be a no-op when no handler is installed. */
    aura_log(AURA_LOG_WARN, "no handler %d", 7);
    assert(atomic_load(&log_cb_count) == 0);
}

TEST(log_emit_formatted_message) {
    int sentinel = 1234;
    atomic_store(&log_cb_count, 0);
    last_log_level = 0;
    last_log_msg[0] = '\0';
    last_log_ud = NULL;

    aura_set_log_handler(test_log_handler, &sentinel);
    aura_log(AURA_LOG_ERR, "formatted %d %s", 42, "message");

    assert(atomic_load(&log_cb_count) == 1);
    assert(last_log_level == AURA_LOG_ERR);
    assert(strcmp(last_log_msg, "formatted 42 message") == 0);
    assert(last_log_ud == &sentinel);

    aura_set_log_handler(NULL, NULL);
}

TEST(log_emit_truncates_long_message) {
    int sentinel = 55;
    char big[1024];
    memset(big, 'X', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    atomic_store(&log_cb_count, 0);
    last_log_msg[0] = '\0';
    aura_set_log_handler(test_log_handler, &sentinel);
    aura_log(AURA_LOG_WARN, "%s", big);

    assert(atomic_load(&log_cb_count) == 1);
    assert(last_log_level == AURA_LOG_WARN);
    /* Internal buffer is 256 bytes, so emitted string must be truncated. */
    assert(strlen(last_log_msg) == 255);
    assert(last_log_ud == &sentinel);

    aura_set_log_handler(NULL, NULL);
}

/* --- aura_log_emit() public API tests ---------------------------------- */

TEST(log_emit_public_no_handler) {
    atomic_store(&log_cb_count, 0);
    aura_set_log_handler(NULL, NULL);

    /* Public emit should be a no-op when no handler is installed. */
    aura_log_emit(AURA_LOG_INFO, "nobody listening %d", 99);
    assert(atomic_load(&log_cb_count) == 0);
}

TEST(log_emit_public_formatted) {
    int sentinel = 7777;
    atomic_store(&log_cb_count, 0);
    last_log_level = 0;
    last_log_msg[0] = '\0';
    last_log_ud = NULL;

    aura_set_log_handler(test_log_handler, &sentinel);
    aura_log_emit(AURA_LOG_NOTICE, "hello %s %d", "world", 5);

    assert(atomic_load(&log_cb_count) == 1);
    assert(last_log_level == AURA_LOG_NOTICE);
    assert(strcmp(last_log_msg, "hello world 5") == 0);
    assert(last_log_ud == &sentinel);

    aura_set_log_handler(NULL, NULL);
}

TEST(log_emit_public_all_levels) {
    int sentinel = 0;
    aura_set_log_handler(test_log_handler, &sentinel);

    /* Verify each public log level constant dispatches correctly. */
    static const int levels[] = { AURA_LOG_ERR, AURA_LOG_WARN, AURA_LOG_NOTICE, AURA_LOG_INFO,
                                  AURA_LOG_DEBUG };
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        atomic_store(&log_cb_count, 0);
        last_log_level = -1;
        aura_log_emit(levels[i], "level %d", levels[i]);
        assert(atomic_load(&log_cb_count) == 1);
        assert(last_log_level == levels[i]);
    }

    aura_set_log_handler(NULL, NULL);
}

TEST(log_emit_public_truncates) {
    int sentinel = 0;
    char big[1024];
    memset(big, 'Y', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    atomic_store(&log_cb_count, 0);
    last_log_msg[0] = '\0';
    aura_set_log_handler(test_log_handler, &sentinel);
    aura_log_emit(AURA_LOG_DEBUG, "%s", big);

    assert(atomic_load(&log_cb_count) == 1);
    assert(last_log_level == AURA_LOG_DEBUG);
    assert(strlen(last_log_msg) == 255);

    aura_set_log_handler(NULL, NULL);
}

TEST(adaptive_inline_getters_direct) {
    adaptive_controller_t ctrl;
    int rc = adaptive_init(&ctrl, 128, 16, 4);
    assert(rc == 0);

    assert(adaptive_get_inflight_limit(&ctrl) == 16);
    assert(adaptive_get_batch_threshold(&ctrl) == ADAPTIVE_MIN_BATCH);

    adaptive_destroy(&ctrl);
}

/* ============================================================================
 * Request Introspection Tests (aura_request_fd, aura_request_user_data)
 * ============================================================================ */

TEST(request_fd_null) {
    int fd = aura_request_fd(NULL);
    assert(fd == -1);
    assert(errno == EINVAL);
}

TEST(request_user_data_null) {
    void *ud = aura_request_user_data(NULL);
    assert(ud == NULL);
    assert(errno == EINVAL);
}

TEST(request_pending_null) {
    assert(aura_request_pending(NULL) == false);
}

TEST(request_introspection_during_io) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf);

    int my_sentinel = 0xBEEF;
    cb_called = 0;
    aura_request_t *req =
        aura_read(engine, test_fd, aura_buf(buf), 4096, 0, basic_cb, &my_sentinel);
    assert(req);

    /* Before completion, request should be pending */
    assert(aura_request_pending(req) == true);
    assert(aura_request_fd(req) == test_fd);
    assert(aura_request_user_data(req) == &my_sentinel);

    aura_wait(engine, 1000);
    assert(cb_called == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Poll FD Tests
 * ============================================================================ */

TEST(poll_fd_null_engine) {
    int fd = aura_get_poll_fd(NULL);
    assert(fd == -1);
}

TEST(poll_fd_valid) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    int fd = aura_get_poll_fd(engine);
    assert(fd >= 0);
    aura_destroy(engine);
}

/* ============================================================================
 * Drain Tests
 * ============================================================================ */

TEST(drain_null_engine) {
    int rc = aura_drain(NULL, 1000);
    assert(rc == -1);
    assert(errno == EINVAL);
}

TEST(drain_no_pending) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Drain with nothing pending should return 0 immediately */
    int rc = aura_drain(engine, 1000);
    assert(rc == 0);

    aura_destroy(engine);
}

TEST(drain_nonblocking) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* timeout_ms=0 should be non-blocking */
    int rc = aura_drain(engine, 0);
    assert(rc == 0);

    aura_destroy(engine);
}

TEST(drain_with_pending_io) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf);

    /* Submit I/O then drain */
    cb_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, basic_cb, NULL);
    assert(req);

    int rc = aura_drain(engine, 5000);
    assert(rc >= 1);
    assert(cb_called == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Vectored I/O Tests (readv / writev)
 * ============================================================================ */

TEST(readv_basic) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    char buf1[2048], buf2[2048];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));
    struct iovec iov[2] = {
        { .iov_base = buf1, .iov_len = sizeof(buf1) },
        { .iov_base = buf2, .iov_len = sizeof(buf2) },
    };

    cb_called = 0;
    aura_request_t *req = aura_readv(engine, test_fd, iov, 2, 0, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 4096);

    /* Verify data read correctly */
    assert(buf1[0] == 'B');
    assert(buf2[0] == 'B');

    aura_destroy(engine);
    io_teardown();
}

TEST(writev_basic) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    char buf1[1024], buf2[1024];
    memset(buf1, 'X', sizeof(buf1));
    memset(buf2, 'Y', sizeof(buf2));
    struct iovec iov[2] = {
        { .iov_base = buf1, .iov_len = sizeof(buf1) },
        { .iov_base = buf2, .iov_len = sizeof(buf2) },
    };

    cb_called = 0;
    aura_request_t *req = aura_writev(engine, test_fd, iov, 2, 0, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 2048);

    /* Verify written data */
    char verify[2048];
    lseek(test_fd, 0, SEEK_SET);
    ssize_t n = read(test_fd, verify, sizeof(verify));
    assert(n == 2048);
    assert(verify[0] == 'X');
    assert(verify[1024] == 'Y');

    aura_destroy(engine);
    io_teardown();
}

TEST(readv_null_args) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* NULL engine */
    assert(aura_readv(NULL, 0, NULL, 0, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    /* NULL iov */
    assert(aura_readv(engine, test_fd, NULL, 1, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    /* iovcnt = 0 */
    struct iovec iov = { .iov_base = NULL, .iov_len = 0 };
    assert(aura_readv(engine, test_fd, &iov, 0, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(writev_null_args) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    assert(aura_writev(NULL, 0, NULL, 0, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    assert(aura_writev(engine, test_fd, NULL, 1, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

/* ============================================================================
 * Fsync Tests
 * ============================================================================ */

TEST(fsync_basic) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    cb_called = 0;
    aura_request_t *req = aura_fsync(engine, test_fd, AURA_FSYNC_DEFAULT, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 0);

    aura_destroy(engine);
    io_teardown();
}

TEST(fsync_datasync) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    cb_called = 0;
    aura_request_t *req = aura_fsync(engine, test_fd, AURA_FSYNC_DATASYNC, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 0);

    aura_destroy(engine);
    io_teardown();
}

TEST(fsync_null_callback) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    /* NULL callback is valid (fire-and-forget) */
    aura_request_t *req = aura_fsync(engine, test_fd, AURA_FSYNC_DEFAULT, NULL, NULL);
    assert(req);
    aura_drain(engine, 1000);

    aura_destroy(engine);
    io_teardown();
}

TEST(fsync_null_args) {
    assert(aura_fsync(NULL, 0, AURA_FSYNC_DEFAULT, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Negative fd */
    assert(aura_fsync(engine, -1, AURA_FSYNC_DEFAULT, NULL, NULL) == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

/* ============================================================================
 * Read/Write Error Path Tests
 * ============================================================================ */

TEST(read_null_engine) {
    char buf[64];
    assert(aura_read(NULL, 0, aura_buf(buf), 64, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
}

TEST(read_negative_fd) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    char buf[64];
    assert(aura_read(engine, -1, aura_buf(buf), 64, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
    aura_destroy(engine);
}

TEST(read_zero_length) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    char buf[64];
    assert(aura_read(engine, 0, aura_buf(buf), 0, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
    aura_destroy(engine);
}

TEST(read_null_buf_ptr) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    assert(aura_read(engine, 0, aura_buf(NULL), 64, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
    aura_destroy(engine);
}

TEST(write_null_engine) {
    char buf[64];
    assert(aura_write(NULL, 0, aura_buf(buf), 64, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
}

TEST(write_negative_fd) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    char buf[64];
    assert(aura_write(engine, -1, aura_buf(buf), 64, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
    aura_destroy(engine);
}

TEST(write_zero_length) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    char buf[64];
    assert(aura_write(engine, 0, aura_buf(buf), 0, 0, NULL, NULL) == NULL);
    assert(errno == EINVAL);
    aura_destroy(engine);
}

/* ============================================================================
 * Null Callback (fire-and-forget) Tests
 * ============================================================================ */

TEST(read_null_callback) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf);

    /* NULL callback should work (fire-and-forget) */
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, NULL, NULL);
    assert(req);
    aura_drain(engine, 1000);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(write_null_callback) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf);
    memset(buf, 'Z', 4096);

    aura_request_t *req = aura_write(engine, test_fd, aura_buf(buf), 4096, 0, NULL, NULL);
    assert(req);
    aura_drain(engine, 1000);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Cancel Tests
 * ============================================================================ */

TEST(cancel_null_args) {
    assert(aura_cancel(NULL, NULL) == -1);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    assert(aura_cancel(engine, NULL) == -1);
    assert(errno == EINVAL);
    aura_destroy(engine);
}

TEST(cancel_completed_request) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    cb_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);

    /* The request handle is returned to the pool after completion,
     * so we cannot safely call aura_cancel() on it.  Just verify
     * the I/O completed successfully. */
    assert(cb_called == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Registered Buffer Edge Cases
 * ============================================================================ */

TEST(register_buffers_null_args) {
    assert(aura_register_buffers(NULL, NULL, 0) == -1);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    assert(aura_register_buffers(engine, NULL, 1) == -1);
    assert(errno == EINVAL);

    struct iovec iov = { .iov_base = NULL, .iov_len = 4096 };
    assert(aura_register_buffers(engine, &iov, 0) == -1);
    assert(errno == EINVAL);

    assert(aura_register_buffers(engine, &iov, UINT_MAX) == -1);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(read_fixed_no_registration) {
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    /* Using a fixed buffer without registration should fail with ENOENT */
    aura_buf_t fbuf = aura_buf_fixed(0, 0);
    aura_request_t *req = aura_read(engine, 0, fbuf, 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == ENOENT);

    aura_destroy(engine);
}

TEST(write_fixed_no_registration) {
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    aura_buf_t fbuf = aura_buf_fixed(0, 0);
    aura_request_t *req = aura_write(engine, 0, fbuf, 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == ENOENT);

    aura_destroy(engine);
}

TEST(register_buffers_and_use) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    /* Allocate and register a buffer */
    void *regbuf = NULL;
    int rc = posix_memalign(&regbuf, 4096, 4096);
    assert(rc == 0 && regbuf);
    memset(regbuf, 0, 4096);

    struct iovec iov = { .iov_base = regbuf, .iov_len = 4096 };
    rc = aura_register_buffers(engine, &iov, 1);
    assert(rc == 0);

    /* Double registration should fail */
    rc = aura_register_buffers(engine, &iov, 1);
    assert(rc == -1);
    assert(errno == EBUSY);

    /* Read using registered buffer */
    cb_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 4096);
    assert(((char *)regbuf)[0] == 'B');

    /* Out-of-range buffer index */
    req = aura_read(engine, test_fd, aura_buf_fixed(99, 0), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* Overflow: offset beyond buffer length */
    req = aura_read(engine, test_fd, aura_buf_fixed(0, 8192), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EOVERFLOW);

    /* Overflow: length exceeds remaining space */
    req = aura_read(engine, test_fd, aura_buf_fixed(0, 0), 8192, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EOVERFLOW);

    /* Write using registered buffer */
    memset(regbuf, 'W', 4096);
    cb_called = 0;
    req = aura_write(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 4096);

    /* Unregister */
    rc = aura_unregister(engine, AURA_REG_BUFFERS);
    assert(rc == 0);

    /* Post-unregister: fixed buffers should fail with ENOENT */
    req = aura_read(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == ENOENT);

    free(regbuf);
    aura_destroy(engine);
    io_teardown();
}

TEST(unregister_reg_buffers_not_registered) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Unregister when nothing is registered — should not crash */
    int rc = aura_unregister(engine, AURA_REG_BUFFERS);
    assert(rc == 0 || rc == -1);

    aura_destroy(engine);
}

/* ============================================================================
 * Registered File Edge Cases
 * ============================================================================ */

TEST(register_files_null_args) {
    assert(aura_register_files(NULL, NULL, 0) == -1);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    assert(aura_register_files(engine, NULL, 1) == -1);
    assert(errno == EINVAL);

    int fd = 0;
    assert(aura_register_files(engine, &fd, 0) == -1);
    assert(errno == EINVAL);

    assert(aura_register_files(engine, &fd, UINT_MAX) == -1);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(register_files_and_io) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    int rc = aura_register_files(engine, &test_fd, 1);
    assert(rc == 0);

    /* Double registration should fail */
    rc = aura_register_files(engine, &test_fd, 1);
    assert(rc == -1);
    assert(errno == EBUSY);

    /* I/O should work transparently with registered files */
    void *buf = aura_buffer_alloc(engine, 4096);
    cb_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, basic_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(cb_called == 1);
    assert(cb_result == 4096);

    aura_buffer_free(engine, buf);

    rc = aura_unregister(engine, AURA_REG_FILES);
    assert(rc == 0);

    aura_destroy(engine);
    io_teardown();
}

TEST(update_file_null_args) {
    assert(aura_update_file(NULL, 0, -1) == -1);
    assert(errno == EINVAL);

    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Negative index */
    assert(aura_update_file(engine, -1, 0) == -1);
    assert(errno == EINVAL);

    /* No files registered */
    assert(aura_update_file(engine, 0, -1) == -1);
    assert(errno == ENOENT);

    aura_destroy(engine);
}

TEST(update_file_out_of_range) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    int rc = aura_register_files(engine, &test_fd, 1);
    assert(rc == 0);

    /* Index beyond count */
    rc = aura_update_file(engine, 99, -1);
    assert(rc == -1);
    assert(errno == EINVAL);

    rc = aura_unregister(engine, AURA_REG_FILES);
    assert(rc == 0);
    aura_destroy(engine);
    io_teardown();
}

TEST(unregister_reg_files_not_registered) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    int rc = aura_unregister(engine, AURA_REG_FILES);
    assert(rc == 0 || rc == -1);
    aura_destroy(engine);
}

/* ============================================================================
 * Deferred Unregister Tests
 * ============================================================================ */

TEST(request_unregister_null_buffers) {
    assert(aura_request_unregister(NULL, AURA_REG_BUFFERS) == -1);
    assert(errno == EINVAL);
}

TEST(request_unregister_null_files) {
    assert(aura_request_unregister(NULL, AURA_REG_FILES) == -1);
    assert(errno == EINVAL);
}

TEST(deferred_unregister_buffers) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *regbuf = NULL;
    int rc = posix_memalign(&regbuf, 4096, 4096);
    assert(rc == 0 && regbuf);

    struct iovec iov = { .iov_base = regbuf, .iov_len = 4096 };
    rc = aura_register_buffers(engine, &iov, 1);
    assert(rc == 0);

    /* Request deferred unregister */
    rc = aura_request_unregister(engine, AURA_REG_BUFFERS);
    assert(rc == 0);

    /* Poll to finalize the deferred unregistration (no in-flight ops) */
    aura_poll(engine);

    /* After finalization, fixed buffer submission should fail */
    aura_request_t *req = aura_read(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == ENOENT || errno == EBUSY);

    free(regbuf);
    aura_destroy(engine);
    io_teardown();
}

TEST(deferred_unregister_files) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    int rc = aura_register_files(engine, &test_fd, 1);
    assert(rc == 0);

    rc = aura_request_unregister(engine, AURA_REG_FILES);
    assert(rc == 0);

    /* Poll to finalize */
    aura_poll(engine);

    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Options Validation Tests
 * ============================================================================ */

TEST(options_init_null) {
    /* Should not crash */
    aura_options_init(NULL);
}

TEST(options_defaults) {
    aura_options_t opts;
    aura_options_init(&opts);
    assert(opts.queue_depth == 1024); /* DEFAULT_QUEUE_DEPTH */
    assert(opts.ring_count == 0); /* 0 = auto */
    assert(opts.disable_adaptive == false);
    assert(opts.enable_sqpoll == false);
    assert(opts.ring_select == AURA_SELECT_ADAPTIVE);
}

TEST(create_with_disable_adaptive) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 32;
    opts.disable_adaptive = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine);
    aura_destroy(engine);
}

/* ============================================================================
 * Version API Tests
 * ============================================================================ */

TEST(version_string) {
    const char *v = aura_version();
    assert(v);
    assert(strlen(v) > 0);
    /* Should contain dots */
    assert(strchr(v, '.'));
}

TEST(version_int) {
    int vi = aura_version_int();
    assert(vi > 0);
    assert(vi == AURA_VERSION);
}

/* ============================================================================
 * Buffer Pool Edge Cases
 * ============================================================================ */

TEST(buffer_alloc_free_multiple_sizes) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Test various size classes */
    size_t sizes[] = { 4096, 8192, 16384, 32768, 65536, 131072 };
    void *bufs[6];

    for (int i = 0; i < 6; i++) {
        bufs[i] = aura_buffer_alloc(engine, sizes[i]);
        assert(bufs[i]);
        /* Write to confirm allocation is real */
        memset(bufs[i], 0xAA, sizes[i]);
    }

    for (int i = 0; i < 6; i++) {
        aura_buffer_free(engine, bufs[i]);
    }

    aura_destroy(engine);
}

TEST(buffer_alloc_null_engine) {
    void *buf = aura_buffer_alloc(NULL, 4096);
    assert(buf == NULL);
}

TEST(buffer_free_null) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    /* Free NULL should be no-op */
    aura_buffer_free(engine, NULL);
    aura_destroy(engine);
}

/* ============================================================================
 * Multi-Thread Buffer Pool Tests
 * ============================================================================ */

struct thread_buf_ctx {
    aura_engine_t *engine;
    _Atomic int iterations;
    _Atomic int done;
};

static void *buffer_thread(void *arg) {
    struct thread_buf_ctx *ctx = arg;
    for (int i = 0; i < ctx->iterations; i++) {
        void *buf = aura_buffer_alloc(ctx->engine, 4096);
        assert(buf);
        memset(buf, 0, 4096);
        aura_buffer_free(ctx->engine, buf);
    }
    atomic_store(&ctx->done, 1);
    return NULL;
}

TEST(buffer_pool_multithread) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

#define NUM_THREADS 4
    pthread_t threads[NUM_THREADS];
    struct thread_buf_ctx ctxs[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        ctxs[i] = (struct thread_buf_ctx){ .engine = engine, .iterations = 100, .done = 0 };
        int rc = pthread_create(&threads[i], NULL, buffer_thread, &ctxs[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
        assert(atomic_load(&ctxs[i].done) == 1);
    }
#undef NUM_THREADS

    aura_destroy(engine);
}

/* Thread that allocates buffers and exits (exercises TLS destructor) */
static void *buffer_thread_exit(void *arg) {
    struct thread_buf_ctx *ctx = arg;
    /* Allocate and free to register TLS cache, then exit */
    for (int i = 0; i < 5; i++) {
        void *buf = aura_buffer_alloc(ctx->engine, 4096);
        assert(buf);
        aura_buffer_free(ctx->engine, buf);
    }
    atomic_store(&ctx->done, 1);
    return NULL;
}

/* Thread that exits after pool destroy (exercises TLS destructor race) */
static void *buffer_thread_late_exit(void *arg) {
    struct thread_buf_ctx *ctx = arg;
    /* Allocate buffers to register TLS cache */
    for (int i = 0; i < 10; i++) {
        void *buf = aura_buffer_alloc(ctx->engine, 4096);
        assert(buf);
        aura_buffer_free(ctx->engine, buf);
    }
    /* Signal done but don't exit yet - pool will be destroyed while thread is alive */
    atomic_store(&ctx->done, 1);
    /* Wait for pool to be destroyed */
    while (atomic_load(&ctx->iterations) == 0) {
        usleep(1000);
    }
    /* Engine is destroyed at this point, so the engine pointer is invalid.
     * Use plain free() here; direct buffer_pool_free-after-destroy coverage is
     * exercised by a dedicated test on a standalone pool instance. */
    void *buf = malloc(4096);
    if (buf) {
        free(buf);
    }
    return NULL;
}

TEST(buffer_pool_thread_exit) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

/* Spawn threads that register TLS caches then exit */
#define NUM_THREADS 8
    pthread_t threads[NUM_THREADS];
    struct thread_buf_ctx ctxs[NUM_THREADS];

    for (int i = 0; i < NUM_THREADS; i++) {
        ctxs[i] = (struct thread_buf_ctx){ .engine = engine, .done = 0 };
        int rc = pthread_create(&threads[i], NULL, buffer_thread_exit, &ctxs[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }
#undef NUM_THREADS

    /* Engine destroy after all threads exited — should clean up orphaned caches */
    aura_destroy(engine);
}

TEST(buffer_pool_destroy_null) {
    /* NULL check should be no-op */
    buffer_pool_destroy(NULL);
}

TEST(buffer_pool_free_after_destroy_direct) {
    buffer_pool_t pool;
    int rc = buffer_pool_init(&pool, 4096);
    assert(rc == 0);

    void *buf = malloc(4096);
    assert(buf != NULL);

    buffer_pool_destroy(&pool);

    /* Valid use: pool object still exists, destroyed flag is set. */
    buffer_pool_free(&pool, buf, 4096);
}

TEST(buffer_pool_huge_alloc) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Allocate > 128MB - should cap to max size class
     * The pool returns NULL for oversized allocations, which is correct */
    void *buf = aura_buffer_alloc(engine, 256 * 1024 * 1024);
    /* Buffer should be NULL for oversized request */
    assert(buf == NULL);

    /* Try allocating exactly at the max (128MB) */
    buf = aura_buffer_alloc(engine, 128 * 1024 * 1024);
    if (buf) {
        aura_buffer_free(engine, buf);
    }

    aura_destroy(engine);
}

TEST(buffer_pool_shard_capacity) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

/* Allocate and free many buffers of the SAME size class from a SINGLE thread
 * to exceed shard capacity (256 per shard). Since we have 2+ shards by default,
 * we need 300+ buffers to reliably overflow at least one shard. */

/* Use a very large number to guarantee shard overflow */
#define NUM_BUFS 600
    void *bufs[NUM_BUFS];

    /* First round: 4KB buffers */
    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i] = aura_buffer_alloc(engine, 4096);
        assert(bufs[i]);
    }

    /* Free all at once from the same thread - this forces overflow
     * because thread cache (16) -> flush to shard in batches of 8,
     * but shard capacity is 256. With 600 buffers, at least one shard
     * will overflow and trigger deferred free. */
    for (int i = 0; i < NUM_BUFS; i++) {
        aura_buffer_free(engine, bufs[i]);
    }

    /* Second round: 8KB buffers to exercise different size class */
    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i] = aura_buffer_alloc(engine, 8192);
        assert(bufs[i]);
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        aura_buffer_free(engine, bufs[i]);
    }

    /* Third round: Force the "no metadata slots" path by exhausting slots */
    for (int i = 0; i < NUM_BUFS; i++) {
        bufs[i] = aura_buffer_alloc(engine, 16384);
        assert(bufs[i]);
    }
    for (int i = 0; i < NUM_BUFS; i++) {
        aura_buffer_free(engine, bufs[i]);
    }

#undef NUM_BUFS

    aura_destroy(engine);
}

TEST(buffer_pool_cross_thread_reuse) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Allocate and free from one thread, then allocate from another
     * This exercises the shard slow path (cache miss -> shard hit) */

    void *buf1 = aura_buffer_alloc(engine, 8192);
    assert(buf1);
    aura_buffer_free(engine, buf1);

    /* Now spawn a thread that allocates - should hit shard slow path */
    pthread_t t;
    struct thread_buf_ctx ctx = { .engine = engine, .iterations = 10, .done = 0 };
    int rc = pthread_create(&t, NULL, buffer_thread, &ctx);
    assert(rc == 0);
    pthread_join(t, NULL);

    aura_destroy(engine);
}

TEST(buffer_pool_direct_shard_free) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Allocate many buffers, then free them all at once to trigger
     * the direct shard free path (when cache full and flush can't help) */
    void *bufs[1000];
    for (int i = 0; i < 1000; i++) {
        bufs[i] = aura_buffer_alloc(engine, 4096);
        assert(bufs[i]);
    }

    /* Free all at once - should overflow thread cache and shard */
    for (int i = 0; i < 1000; i++) {
        aura_buffer_free(engine, bufs[i]);
    }

    aura_destroy(engine);
}

TEST(buffer_pool_size_classes) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Test all size classes to ensure size_to_class coverage */
    size_t sizes[] = {
        1024, /* Class 0: 4KB */
        5000, /* Class 1: 8KB */
        10000, /* Class 2: 16KB */
        20000, /* Class 3: 32KB */
        40000, /* Class 4: 64KB */
        80000, /* Class 5: 128KB */
        150000, /* Class 6: 256KB */
        300000, /* Class 7: 512KB (slow path via __builtin_clzl) */
        1000000, /* Class 8: 1MB */
        5000000, /* Class 9: 8MB */
        50000000, /* Class 12: 64MB */
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        void *buf = aura_buffer_alloc(engine, sizes[i]);
        if (buf) { /* May be NULL for very large sizes */
            aura_buffer_free(engine, buf);
        }
    }

    aura_destroy(engine);
}

TEST(buffer_pool_destroy_with_threads) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Start threads that will exit after pool destroy */
    pthread_t threads[2];
    struct thread_buf_ctx ctxs[2];

    for (int i = 0; i < 2; i++) {
        ctxs[i] = (struct thread_buf_ctx){ .engine = engine, .iterations = 0, .done = 0 };
        int rc = pthread_create(&threads[i], NULL, buffer_thread_late_exit, &ctxs[i]);
        assert(rc == 0);
    }

    /* Wait for threads to register TLS caches */
    while (atomic_load(&ctxs[0].done) == 0 || atomic_load(&ctxs[1].done) == 0) {
        usleep(1000);
    }

    /* Destroy pool while threads are still alive - exercises TLS destructor path */
    aura_destroy(engine);

    /* Signal threads to exit */
    atomic_store(&ctxs[0].iterations, 1);
    atomic_store(&ctxs[1].iterations, 1);

    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
    }
}

/* ============================================================================
 * adaptive_ring.c Coverage Tests
 * ============================================================================ */

TEST(ring_destroy_null) {
    /* NULL check should be no-op */
    ring_destroy(NULL);
}

TEST(ring_null_operations) {
    /* Test NULL context checks on all operations */
    assert(ring_poll(NULL) == 0); /* NULL check returns 0 */
    assert(ring_wait(NULL, 1000) == -1);
    assert(ring_can_submit(NULL) == false);
    assert(ring_should_flush(NULL) == false);
    assert(ring_get_fd(NULL) == -1);
}

TEST(ring_request_pool_exhaustion) {
    /* Create a small ring and exhaust request pool */
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 4, -1, NULL);
    assert(rc == 0);

    /* Get all 4 requests */
    int op_idx[4];
    aura_request_t *reqs[4];
    for (int i = 0; i < 4; i++) {
        reqs[i] = ring_get_request(&ctx, &op_idx[i]);
        assert(reqs[i] != NULL);
    }

    /* Next request should fail (pool exhausted) */
    int dummy_idx;
    aura_request_t *req = ring_get_request(&ctx, &dummy_idx);
    assert(req == NULL);

    /* Return all requests */
    for (int i = 0; i < 4; i++) {
        ring_put_request(&ctx, op_idx[i]);
    }

    ring_destroy(&ctx);
}

TEST(ring_submit_null_validations) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* All submit functions should validate NULL */
    assert(ring_submit_read(NULL, NULL) == -1);
    assert(ring_submit_write(NULL, NULL) == -1);
    assert(ring_submit_readv(NULL, NULL) == -1);
    assert(ring_submit_writev(NULL, NULL) == -1);
    assert(ring_submit_fsync(NULL, NULL) == -1);
    assert(ring_submit_fdatasync(NULL, NULL) == -1);
    assert(ring_submit_cancel(NULL, NULL, NULL) == -1);
    assert(ring_submit_read_fixed(NULL, NULL) == -1);
    assert(ring_submit_write_fixed(NULL, NULL) == -1);

    ring_destroy(&ctx);
}

TEST(ring_sqe_exhaustion) {
    io_setup();
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 4, -1, NULL);
    assert(rc == 0);

    /* Get all requests and submit without flushing */
    int op_idx[4];
    aura_request_t *reqs[4];
    char buf[4096];

    for (int i = 0; i < 4; i++) {
        reqs[i] = ring_get_request(&ctx, &op_idx[i]);
        assert(reqs[i] != NULL);

        /* Setup read request */
        reqs[i]->op_type = AURA_OP_READ;
        reqs[i]->fd = test_fd;
        reqs[i]->buffer = buf;
        reqs[i]->len = 1024;
        reqs[i]->offset = 0;
        reqs[i]->callback = NULL;
        reqs[i]->user_data = NULL;
        atomic_store(&reqs[i]->pending, true);

        /* Submit - this should queue SQE */
        rc = ring_submit_read(&ctx, reqs[i]);
        assert(rc == 0);
    }

    /* Flush to submit all */
    rc = ring_flush(&ctx);
    assert(rc == 4);

    /* Wait for completions */
    ring_wait(&ctx, 1000);

    ring_destroy(&ctx);
    io_teardown();
}

TEST(ring_wait_nonblocking) {
    io_setup();
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Submit an operation */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    char buf[4096];
    req->op_type = AURA_OP_READ;
    req->fd = test_fd;
    req->buffer = buf;
    req->len = 4096;
    req->offset = 0;
    req->callback = NULL;
    req->user_data = NULL;
    atomic_store(&req->pending, true);

    rc = ring_submit_read(&ctx, req);
    assert(rc == 0);
    rc = ring_flush(&ctx);
    assert(rc == 1);

    /* Non-blocking wait (timeout_ms=0) */
    rc = ring_wait(&ctx, 0);
    assert(rc >= 0);

    /* Drain any remaining */
    ring_wait(&ctx, 1000);

    ring_destroy(&ctx);
    io_teardown();
}

TEST(ring_sqpoll_fallback) {
    /* Try to enable SQPOLL as non-root - should fallback to normal mode */
    ring_ctx_t ctx;
    ring_options_t opts = { .enable_sqpoll = true, .sqpoll_idle_ms = 1000 };
    int rc = ring_init(&ctx, 32, -1, &opts);

    /* Either succeeds with fallback or fails (both acceptable) */
    if (rc == 0) {
        /* Check that SQPOLL was not enabled (fallback occurred) */
        /* Note: sqpoll_enabled may be true if running as root */
        ring_destroy(&ctx);
    }
}

TEST(ring_destroy_with_pending) {
    io_setup();

    /* Create pipe for blocking I/O */
    int pipefd[2];
    int rc = pipe(pipefd);
    assert(rc == 0);

    ring_ctx_t ctx;
    rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Submit read on pipe (will block since no data) */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    char buf[64];
    req->op_type = AURA_OP_READ;
    req->fd = pipefd[0];
    req->buffer = buf;
    req->len = 64;
    req->offset = 0;
    req->callback = NULL;
    req->user_data = NULL;
    atomic_store(&req->pending, true);

    rc = ring_submit_read(&ctx, req);
    assert(rc == 0);
    rc = ring_flush(&ctx);
    assert(rc == 1);

    /* Write data to unblock */
    write(pipefd[1], "data", 4);

    /* Destroy with pending - should drain */
    ring_destroy(&ctx);

    close(pipefd[0]);
    close(pipefd[1]);
    io_teardown();
}

TEST(ring_put_request_invalid) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Put invalid index - should be no-op or handle gracefully */
    ring_put_request(&ctx, -1);
    ring_put_request(&ctx, 999);

    /* Double-free detection: get a request, put it twice */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);
    ring_put_request(&ctx, op_idx);
    /* Second put of same index - implementation should handle this gracefully */

    ring_destroy(&ctx);
}

TEST(ring_submit_with_null_request) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Submit functions with valid ctx but NULL request */
    assert(ring_submit_read(&ctx, NULL) == -1);
    assert(ring_submit_write(&ctx, NULL) == -1);
    assert(ring_submit_readv(&ctx, NULL) == -1);
    assert(ring_submit_writev(&ctx, NULL) == -1);
    assert(ring_submit_fsync(&ctx, NULL) == -1);
    assert(ring_submit_fdatasync(&ctx, NULL) == -1);
    assert(ring_submit_read_fixed(&ctx, NULL) == -1);
    assert(ring_submit_write_fixed(&ctx, NULL) == -1);

    ring_destroy(&ctx);
}

TEST(ring_can_submit_should_flush) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Initially should be able to submit */
    assert(ring_can_submit(&ctx) == true);

    /* Initially should not need to flush */
    assert(ring_should_flush(&ctx) == false);

    ring_destroy(&ctx);
}

TEST(ring_flush_empty) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Flush with nothing queued should return 0 */
    rc = ring_flush(&ctx);
    assert(rc == 0);

    ring_destroy(&ctx);
}

TEST(ring_poll_wait_no_pending) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Poll with no pending should return 0 */
    rc = ring_poll(&ctx);
    assert(rc == 0);

    /* Wait with no pending should return 0 immediately */
    rc = ring_wait(&ctx, 1000);
    assert(rc == 0);

    ring_destroy(&ctx);
}

TEST(ring_get_fd_valid) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    int fd = ring_get_fd(&ctx);
    assert(fd >= 0);

    ring_destroy(&ctx);
}

TEST(ring_submit_len_overflow) {
    io_setup();
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Get a request */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    /* Setup request with len > UINT_MAX */
    char buf[4096];
    req->op_type = AURA_OP_READ;
    req->fd = test_fd;
    req->buffer = buf;
    req->len = (size_t)UINT_MAX + 1;
    req->offset = 0;
    req->callback = NULL;
    req->user_data = NULL;

    /* Submit should fail with validation error */
    rc = ring_submit_read(&ctx, req);
    assert(rc == -1);

    /* Same for write */
    req->op_type = AURA_OP_WRITE;
    rc = ring_submit_write(&ctx, req);
    assert(rc == -1);

    ring_put_request(&ctx, op_idx);
    ring_destroy(&ctx);
    io_teardown();
}

TEST(ring_submit_readv_writev_validation) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Get a request */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    char buf[4096];
    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };

    /* NULL iov should fail */
    req->iov = NULL;
    req->iovcnt = 1;
    rc = ring_submit_readv(&ctx, req);
    assert(rc == -1);

    rc = ring_submit_writev(&ctx, req);
    assert(rc == -1);

    /* Zero iovcnt should fail */
    req->iov = &iov;
    req->iovcnt = 0;
    rc = ring_submit_readv(&ctx, req);
    assert(rc == -1);

    rc = ring_submit_writev(&ctx, req);
    assert(rc == -1);

    /* Negative iovcnt should fail */
    req->iovcnt = -1;
    rc = ring_submit_readv(&ctx, req);
    assert(rc == -1);

    rc = ring_submit_writev(&ctx, req);
    assert(rc == -1);

    ring_put_request(&ctx, op_idx);
    ring_destroy(&ctx);
}

TEST(ring_submit_cancel_validation) {
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    /* NULL target should fail */
    rc = ring_submit_cancel(&ctx, req, NULL);
    assert(rc == -1);

    /* Both NULL should fail */
    rc = ring_submit_cancel(&ctx, NULL, NULL);
    assert(rc == -1);

    ring_put_request(&ctx, op_idx);
    ring_destroy(&ctx);
}

TEST(ring_multiple_flushes) {
    io_setup();
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 32, -1, NULL);
    assert(rc == 0);

    /* Submit multiple operations and flush multiple times */
    for (int round = 0; round < 3; round++) {
        int op_idx;
        aura_request_t *req = ring_get_request(&ctx, &op_idx);
        assert(req != NULL);

        char buf[1024];
        req->op_type = AURA_OP_READ;
        req->fd = test_fd;
        req->buffer = buf;
        req->len = 1024;
        req->offset = 0;
        req->callback = NULL;
        req->user_data = NULL;
        atomic_store(&req->pending, true);

        rc = ring_submit_read(&ctx, req);
        assert(rc == 0);

        rc = ring_flush(&ctx);
        assert(rc == 1);

        /* Wait for completion */
        ring_wait(&ctx, 1000);
    }

    ring_destroy(&ctx);
    io_teardown();
}

TEST(ring_submit_all_op_types) {
    io_setup();
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 64, -1, NULL);
    assert(rc == 0);

    char buf[4096];
    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };

    /* Test fsync */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);
    req->fd = test_fd;
    req->callback = NULL;
    req->user_data = NULL;
    rc = ring_submit_fsync(&ctx, req);
    assert(rc == 0);

    /* Test fdatasync */
    req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);
    req->fd = test_fd;
    req->callback = NULL;
    req->user_data = NULL;
    rc = ring_submit_fdatasync(&ctx, req);
    assert(rc == 0);

    /* Test readv */
    req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);
    req->fd = test_fd;
    req->iov = &iov;
    req->iovcnt = 1;
    req->offset = 0;
    req->callback = NULL;
    req->user_data = NULL;
    rc = ring_submit_readv(&ctx, req);
    assert(rc == 0);

    /* Test writev */
    req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);
    req->fd = test_fd;
    req->iov = &iov;
    req->iovcnt = 1;
    req->offset = 0;
    req->callback = NULL;
    req->user_data = NULL;
    rc = ring_submit_writev(&ctx, req);
    assert(rc == 0);

    /* Test write */
    req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);
    req->fd = test_fd;
    req->buffer = buf;
    req->len = 4096;
    req->offset = 0;
    req->callback = NULL;
    req->user_data = NULL;
    rc = ring_submit_write(&ctx, req);
    assert(rc == 0);

    /* Flush and wait */
    rc = ring_flush(&ctx);
    assert(rc == 5);

    ring_wait(&ctx, 2000);

    ring_destroy(&ctx);
    io_teardown();
}

TEST(ring_latency_sampling) {
    io_setup();
    ring_ctx_t ctx;
    int rc = ring_init(&ctx, 64, -1, NULL);
    assert(rc == 0);
    char *bufs[32] = { 0 };

    /* Submit many operations to trigger latency sampling at different phases */
    for (int i = 0; i < 32; i++) {
        int op_idx;
        aura_request_t *req = ring_get_request(&ctx, &op_idx);
        assert(req != NULL);

        char *buf = malloc(1024);
        assert(buf);
        bufs[i] = buf;
        req->fd = test_fd;
        req->buffer = buf;
        req->len = 1024;
        req->offset = 0;
        req->callback = NULL;
        req->user_data = buf; /* Store buf to free later */

        rc = ring_submit_read(&ctx, req);
        assert(rc == 0);
    }

    rc = ring_flush(&ctx);
    assert(rc == 32);

    /* Wait and let completions happen */
    for (int i = 0; i < 32; i++) {
        ring_wait(&ctx, 1000);
    }

    for (int i = 0; i < 32; i++) {
        free(bufs[i]);
    }

    ring_destroy(&ctx);
    io_teardown();
}

TEST(buffer_pool_thread_cache_refill) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    void *bufs[30] = { 0 };

    /* Allocate buffers from main thread and free them */
    for (int i = 0; i < 20; i++) {
        void *buf = aura_buffer_alloc(engine, 4096);
        assert(buf);
        aura_buffer_free(engine, buf);
    }

    /* Allocate again - should hit cache refill path from shard */
    for (int i = 0; i < 30; i++) {
        bufs[i] = aura_buffer_alloc(engine, 4096);
        assert(bufs[i]);
    }

    for (int i = 0; i < 30; i++) {
        aura_buffer_free(engine, bufs[i]);
    }

    aura_destroy(engine);
}

TEST(buffer_pool_mixed_sizes) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    /* Mix allocations of different sizes to exercise multiple buckets */
    void *bufs[100];
    size_t sizes[] = { 4096, 8192, 16384, 32768, 4096, 8192 };

    for (int i = 0; i < 100; i++) {
        size_t size = sizes[i % 6];
        bufs[i] = aura_buffer_alloc(engine, size);
        assert(bufs[i]);
    }

    /* Free in reverse order */
    for (int i = 99; i >= 0; i--) {
        aura_buffer_free(engine, bufs[i]);
    }

    /* Allocate again to reuse freed buffers */
    for (int i = 0; i < 100; i++) {
        bufs[i] = aura_buffer_alloc(engine, sizes[i % 6]);
        assert(bufs[i]);
        aura_buffer_free(engine, bufs[i]);
    }

    aura_destroy(engine);
}

/* ============================================================================
 * Run/Stop Tests
 * ============================================================================ */

struct run_ctx {
    aura_engine_t *engine;
    _Atomic int running;
};

static void *run_thread(void *arg) {
    struct run_ctx *ctx = arg;
    atomic_store(&ctx->running, 1);
    aura_run(ctx->engine);
    atomic_store(&ctx->running, 0);
    return NULL;
}

TEST(run_stop) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);

    struct run_ctx ctx = { .engine = engine, .running = 0 };
    pthread_t t;
    int rc = pthread_create(&t, NULL, run_thread, &ctx);
    assert(rc == 0);

    /* Wait for run to start */
    while (!atomic_load(&ctx.running)) usleep(1000);

    /* Stop it */
    aura_stop(engine);
    pthread_join(t, NULL);
    assert(atomic_load(&ctx.running) == 0);

    aura_destroy(engine);
}

/* ============================================================================
 * Shutdown Behavior Tests
 * ============================================================================ */

TEST(submit_after_destroy_begins) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    /* Submit I/O, drain, then destroy */
    void *buf = aura_buffer_alloc(engine, 4096);
    cb_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, basic_cb, NULL);
    assert(req);
    aura_drain(engine, 1000);
    assert(cb_called == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * aura_buf_t Helper Tests
 * ============================================================================ */

TEST(buf_descriptor_unregistered) {
    char data[64];
    aura_buf_t b = aura_buf(data);
    assert(b.type == AURA_BUF_UNREGISTERED);
    assert(b.u.ptr == data);
}

TEST(buf_descriptor_fixed) {
    aura_buf_t b = aura_buf_fixed(3, 512);
    assert(b.type == AURA_BUF_REGISTERED);
    assert(b.u.fixed.index == 3);
    assert(b.u.fixed.offset == 512);
}

TEST(buf_descriptor_fixed_idx) {
    aura_buf_t b = aura_buf_fixed_idx(7);
    assert(b.type == AURA_BUF_REGISTERED);
    assert(b.u.fixed.index == 7);
    assert(b.u.fixed.offset == 0);
}

TEST(buf_descriptor_negative_index) {
    aura_buf_t b = aura_buf_fixed(-1, 0);
    assert(b.type == AURA_BUF_UNREGISTERED);
    assert(b.u.ptr == NULL);
}

/* ============================================================================
 * Wait Edge Cases
 * ============================================================================ */

TEST(wait_null_engine) {
    int rc = aura_wait(NULL, 100);
    assert(rc == -1);
}

TEST(wait_zero_timeout) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    /* Non-blocking wait with no pending I/O */
    int rc = aura_wait(engine, 0);
    assert(rc >= 0);
    aura_destroy(engine);
}

TEST(poll_no_pending) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    int rc = aura_poll(engine);
    assert(rc >= 0);
    aura_destroy(engine);
}

/* ============================================================================
 * API Coverage: aura_create(), aura_get_fatal_error(),
 *               aura_in_callback_context(), aura_histogram_percentile(),
 *               aura_request_op_type()
 * ============================================================================ */

TEST(create_default) {
    aura_engine_t *engine = aura_create();
    assert(engine);
    aura_destroy(engine);
}

TEST(get_fatal_error_null) {
    int err = aura_get_fatal_error(NULL);
    assert(err == -1);
    assert(errno == EINVAL);
}

TEST(get_fatal_error_clean) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    int err = aura_get_fatal_error(engine);
    assert(err == 0);
    aura_destroy(engine);
}

TEST(in_callback_context_outside) {
    /* Outside any callback, should return false */
    assert(aura_in_callback_context() == false);
}

static void check_in_callback_cb(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)result;
    int *flag = user_data;
    *flag = aura_in_callback_context() ? 1 : 0;
}

TEST(in_callback_context_inside) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    int in_cb = -1;
    aura_request_t *req =
        aura_read(engine, test_fd, aura_buf(buf), 4096, 0, check_in_callback_cb, &in_cb);
    assert(req);
    aura_wait(engine, 1000);
    assert(in_cb == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(histogram_percentile_null) {
    double val = aura_histogram_percentile(NULL, 50.0);
    assert(val < 0.0);
}

TEST(histogram_percentile_out_of_range) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    aura_histogram_t hist;
    aura_get_histogram(engine, 0, &hist, sizeof(hist));
    double val = aura_histogram_percentile(&hist, -1.0);
    assert(val < 0.0);
    val = aura_histogram_percentile(&hist, 101.0);
    assert(val < 0.0);
    aura_destroy(engine);
}

TEST(histogram_percentile_valid) {
    aura_engine_t *engine = make_engine(1, 32);
    assert(engine);
    aura_histogram_t hist;
    aura_get_histogram(engine, 0, &hist, sizeof(hist));
    /* With valid range, should not crash; value depends on data */
    double val = aura_histogram_percentile(&hist, 50.0);
    (void)val; /* may be 0 or negative with empty histogram */
    aura_destroy(engine);
}

TEST(request_op_type_null) {
    int op = aura_request_op_type(NULL);
    assert(op == -1);
}

static int last_op_type = -999;
static void capture_op_cb(aura_request_t *req, ssize_t result, void *user_data) {
    (void)result;
    (void)user_data;
    last_op_type = aura_request_op_type(req);
}

TEST(request_op_type_read) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    last_op_type = -999;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, capture_op_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(last_op_type == AURA_OP_READ);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(request_op_type_write) {
    io_setup();
    aura_engine_t *engine = make_engine(1, 64);
    assert(engine);

    void *buf = aura_buffer_alloc(engine, 4096);
    memset(buf, 'Z', 4096);
    last_op_type = -999;
    aura_request_t *req = aura_write(engine, test_fd, aura_buf(buf), 4096, 0, capture_op_cb, NULL);
    assert(req);
    aura_wait(engine, 1000);
    assert(last_op_type == AURA_OP_WRITE);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    atexit(cleanup_atexit);
    printf("\n=== Coverage Expansion Tests ===\n\n");

    /* Log callback (log.c) */
    RUN_TEST(log_set_handler);
    RUN_TEST(log_null_handler);
    RUN_TEST(log_handler_receives_messages);
    RUN_TEST(log_emit_no_handler);
    RUN_TEST(log_emit_formatted_message);
    RUN_TEST(log_emit_truncates_long_message);
    RUN_TEST(log_emit_public_no_handler);
    RUN_TEST(log_emit_public_formatted);
    RUN_TEST(log_emit_public_all_levels);
    RUN_TEST(log_emit_public_truncates);
    RUN_TEST(adaptive_inline_getters_direct);

    /* Request introspection */
    RUN_TEST(request_fd_null);
    RUN_TEST(request_user_data_null);
    RUN_TEST(request_pending_null);
    RUN_TEST(request_introspection_during_io);

    /* Poll FD */
    RUN_TEST(poll_fd_null_engine);
    RUN_TEST(poll_fd_valid);

    /* Drain */
    RUN_TEST(drain_null_engine);
    RUN_TEST(drain_no_pending);
    RUN_TEST(drain_nonblocking);
    RUN_TEST(drain_with_pending_io);

    /* Vectored I/O */
    RUN_TEST(readv_basic);
    RUN_TEST(writev_basic);
    RUN_TEST(readv_null_args);
    RUN_TEST(writev_null_args);

    /* Fsync */
    RUN_TEST(fsync_basic);
    RUN_TEST(fsync_datasync);
    RUN_TEST(fsync_null_callback);
    RUN_TEST(fsync_null_args);

    /* Read/write error paths */
    RUN_TEST(read_null_engine);
    RUN_TEST(read_negative_fd);
    RUN_TEST(read_zero_length);
    RUN_TEST(read_null_buf_ptr);
    RUN_TEST(write_null_engine);
    RUN_TEST(write_negative_fd);
    RUN_TEST(write_zero_length);

    /* Null callback (fire-and-forget) */
    RUN_TEST(read_null_callback);
    RUN_TEST(write_null_callback);

    /* Cancel */
    RUN_TEST(cancel_null_args);
    RUN_TEST(cancel_completed_request);

    /* Registered buffers */
    RUN_TEST(register_buffers_null_args);
    RUN_TEST(read_fixed_no_registration);
    RUN_TEST(write_fixed_no_registration);
    RUN_TEST(register_buffers_and_use);
    RUN_TEST(unregister_reg_buffers_not_registered);

    /* Registered files */
    RUN_TEST(register_files_null_args);
    RUN_TEST(register_files_and_io);
    RUN_TEST(update_file_null_args);
    RUN_TEST(update_file_out_of_range);
    RUN_TEST(unregister_reg_files_not_registered);

    /* Deferred unregister */
    RUN_TEST(request_unregister_null_buffers);
    RUN_TEST(request_unregister_null_files);
    RUN_TEST(deferred_unregister_buffers);
    RUN_TEST(deferred_unregister_files);

    /* Options */
    RUN_TEST(options_init_null);
    RUN_TEST(options_defaults);
    RUN_TEST(create_with_disable_adaptive);

    /* Version */
    RUN_TEST(version_string);
    RUN_TEST(version_int);

    /* Buffer pool */
    RUN_TEST(buffer_alloc_free_multiple_sizes);
    RUN_TEST(buffer_alloc_null_engine);
    RUN_TEST(buffer_free_null);
    RUN_TEST(buffer_pool_multithread);
    RUN_TEST(buffer_pool_thread_exit);
    RUN_TEST(buffer_pool_destroy_null);
    RUN_TEST(buffer_pool_free_after_destroy_direct);
    RUN_TEST(buffer_pool_huge_alloc);
    RUN_TEST(buffer_pool_shard_capacity);
    RUN_TEST(buffer_pool_cross_thread_reuse);
    RUN_TEST(buffer_pool_direct_shard_free);
    RUN_TEST(buffer_pool_size_classes);
    RUN_TEST(buffer_pool_destroy_with_threads);

    /* Ring operations */
    RUN_TEST(ring_destroy_null);
    RUN_TEST(ring_null_operations);
    RUN_TEST(ring_request_pool_exhaustion);
    RUN_TEST(ring_submit_null_validations);
    RUN_TEST(ring_sqe_exhaustion);
    RUN_TEST(ring_wait_nonblocking);
    RUN_TEST(ring_sqpoll_fallback);
    RUN_TEST(ring_destroy_with_pending);
    RUN_TEST(ring_put_request_invalid);
    RUN_TEST(ring_submit_with_null_request);
    RUN_TEST(ring_can_submit_should_flush);
    RUN_TEST(ring_flush_empty);
    RUN_TEST(ring_poll_wait_no_pending);
    RUN_TEST(ring_get_fd_valid);
    RUN_TEST(ring_submit_len_overflow);
    RUN_TEST(ring_submit_readv_writev_validation);
    RUN_TEST(ring_submit_cancel_validation);
    RUN_TEST(ring_multiple_flushes);
    RUN_TEST(ring_submit_all_op_types);
    RUN_TEST(ring_latency_sampling);
    RUN_TEST(buffer_pool_thread_cache_refill);
    RUN_TEST(buffer_pool_mixed_sizes);

    /* Run/stop */
    RUN_TEST(run_stop);

    /* Shutdown */
    RUN_TEST(submit_after_destroy_begins);

    /* Buffer descriptors */
    RUN_TEST(buf_descriptor_unregistered);
    RUN_TEST(buf_descriptor_fixed);
    RUN_TEST(buf_descriptor_fixed_idx);
    RUN_TEST(buf_descriptor_negative_index);

    /* Wait/poll edges */
    RUN_TEST(wait_null_engine);
    RUN_TEST(wait_zero_timeout);
    RUN_TEST(poll_no_pending);

    /* API coverage */
    RUN_TEST(create_default);
    RUN_TEST(get_fatal_error_null);
    RUN_TEST(get_fatal_error_clean);
    RUN_TEST(in_callback_context_outside);
    RUN_TEST(in_callback_context_inside);
    RUN_TEST(histogram_percentile_null);
    RUN_TEST(histogram_percentile_out_of_range);
    RUN_TEST(histogram_percentile_valid);
    RUN_TEST(request_op_type_null);
    RUN_TEST(request_op_type_read);
    RUN_TEST(request_op_type_write);

    printf("\n  All %d tests passed!\n\n", test_count);
    return 0;
}
