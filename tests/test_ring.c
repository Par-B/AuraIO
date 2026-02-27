// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_ring.c
 * @brief Unit tests for io_uring ring wrapper
 *
 * Note: These tests require Linux with io_uring support.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <stdatomic.h>
#include <sys/stat.h>

#include "../include/aura.h"
#include "../src/adaptive_ring.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-40s", #name); \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* Test file path */
static char test_file[256];
static int test_fd = -1;

/* Ensure temp files are cleaned up even on crash/abort */
static void cleanup_atexit(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    if (test_file[0]) {
        unlink(test_file);
    }
}

/* ============================================================================
 * Setup / Teardown
 * ============================================================================ */

static void setup(void) {
    /* Try /tmp first, then current directory */
    strcpy(test_file, "/tmp/test_ring_XXXXXX");
    test_fd = mkstemp(test_file);
    if (test_fd < 0) {
        strcpy(test_file, "./test_ring_XXXXXX");
        test_fd = mkstemp(test_file);
    }
    assert(test_fd >= 0);

    /* Write some test data */
    char data[4096];
    memset(data, 'A', sizeof(data));
    ssize_t written = write(test_fd, data, sizeof(data));
    assert(written == sizeof(data));

    /* Seek back to start */
    lseek(test_fd, 0, SEEK_SET);
}

static void teardown(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    unlink(test_file);
}

/* ============================================================================
 * Ring Tests
 * ============================================================================ */

TEST(ring_init) {
    ring_ctx_t ctx;

    int ret = ring_init(&ctx, 64, -1, NULL);
    assert(ret == 0);
    assert(ctx.ring_initialized);
    assert(ctx.max_requests == 64);
    assert(ctx.pending_count == 0);

    ring_destroy(&ctx);
}

TEST(ring_init_invalid) {
    ring_ctx_t ctx;

    /* NULL context */
    assert(ring_init(NULL, 64, -1, NULL) == -1);

    /* Zero queue depth */
    assert(ring_init(&ctx, 0, -1, NULL) == -1);
}

TEST(ring_request_management) {
    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Get requests */
    int idx1, idx2;
    aura_request_t *req1 = ring_get_request(&ctx, &idx1);
    aura_request_t *req2 = ring_get_request(&ctx, &idx2);

    assert(req1 != NULL);
    assert(req2 != NULL);
    assert(idx1 != idx2);
    assert(ctx.free_request_count == 62);

    /* Return requests */
    ring_put_request(&ctx, idx1);
    ring_put_request(&ctx, idx2);
    assert(ctx.free_request_count == 64);

    ring_destroy(&ctx);
}

TEST(ring_can_submit) {
    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* In passthrough mode, always submittable */
    assert(ring_can_submit(&ctx));

    /* Disable passthrough to test AIMD gating */
    atomic_store(&ctx.adaptive.passthrough_mode, false);

    assert(ring_can_submit(&ctx));

    /* Use up all slots (artificially) */
    ctx.pending_count = adaptive_get_inflight_limit(&ctx.adaptive);
    assert(!ring_can_submit(&ctx));

    ctx.pending_count = 0;
    ring_destroy(&ctx);
}

TEST(ring_should_flush) {
    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    ctx.queued_sqes = 0;
    assert(!ring_should_flush(&ctx));

    ctx.queued_sqes = adaptive_get_batch_threshold(&ctx.adaptive);
    assert(ring_should_flush(&ctx));

    ctx.queued_sqes = 0;
    ring_destroy(&ctx);
}

/* Callback flag for async tests */
static _Atomic int callback_called = 0;
static _Atomic ssize_t callback_result = 0;

static void test_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    callback_called = 1;
    callback_result = result;
}

typedef struct {
    aura_engine_t *engine;
    int fd;
    int unregister_rc;
    int nested_submit_errno;
} unregister_cb_ctx_t;

static void unregister_in_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    unregister_cb_ctx_t *ctx = (unregister_cb_ctx_t *)user_data;
    callback_called = 1;
    callback_result = result;

    ctx->unregister_rc = aura_unregister(ctx->engine, AURA_REG_BUFFERS);

    aura_request_t *nested =
        aura_read(ctx->engine, ctx->fd, aura_buf_fixed(0, 0), 64, 0, NULL, NULL);
    if (nested) {
        ctx->nested_submit_errno = 0;
    } else {
        ctx->nested_submit_errno = errno;
    }
}

typedef struct {
    _Atomic int completions;
    int first_ring;
    int second_ring;
} fixed_unreg_ctx_t;

static void fixed_unreg_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)result;
    fixed_unreg_ctx_t *ctx = (fixed_unreg_ctx_t *)user_data;
    int idx = atomic_fetch_add_explicit(&ctx->completions, 1, memory_order_relaxed);
    if (idx == 0) {
        ctx->first_ring = req->ring_idx;
    } else if (idx == 1) {
        ctx->second_ring = req->ring_idx;
    }
}

typedef struct {
    aura_engine_t *engine;
    int request_rc;
    int update_rc;
    int update_errno;
} files_unreg_cb_ctx_t;

static void request_unregister_files_in_callback(aura_request_t *req, ssize_t result,
                                                 void *user_data) {
    (void)req;
    files_unreg_cb_ctx_t *ctx = (files_unreg_cb_ctx_t *)user_data;
    callback_called = 1;
    callback_result = result;

    ctx->request_rc = aura_request_unregister(ctx->engine, AURA_REG_FILES);
    ctx->update_rc = aura_update_file(ctx->engine, 0, -1);
    ctx->update_errno = (ctx->update_rc == 0) ? 0 : errno;
}

TEST(ring_read_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Allocate aligned buffer */
    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    assert(buf != NULL);
    memset(buf, 0, 4096); /* Initialize for valgrind */

    /* Get a request */
    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    /* Fill in request */
    req->fd = test_fd;
    req->buffer = buf;
    req->len = 4096;
    req->offset = 0;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;

    /* Submit read */
    int ret = ring_submit_read(&ctx, req);
    assert(ret == 0);
    assert(ctx.pending_count == 1);

    /* Flush and wait */
    ring_flush(&ctx);
    ret = ring_wait(&ctx, 1000);
    assert(ret > 0);

    /* Check callback was invoked */
    assert(callback_called == 1);
    assert(callback_result == 4096);

    /* Verify data */
    assert(((char *)buf)[0] == 'A');

    free(buf);
    ring_destroy(&ctx);
    teardown();
}

TEST(ring_write_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    memset(buf, 'B', 4096);

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    req->fd = test_fd;
    req->buffer = buf;
    req->len = 4096;
    req->offset = 0;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;

    int ret = ring_submit_write(&ctx, req);
    assert(ret == 0);

    ring_flush(&ctx);
    ret = ring_wait(&ctx, 1000);
    assert(ret > 0);

    assert(callback_called == 1);
    assert(callback_result == 4096);

    /* Verify write - read back the data */
    lseek(test_fd, 0, SEEK_SET);
    char verify[4096];
    assert(read(test_fd, verify, 4096) > 0);
    assert(verify[0] == 'B');

    free(buf);
    ring_destroy(&ctx);
    teardown();
}

TEST(ring_poll_no_ops) {
    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Poll with nothing pending */
    int ret = ring_poll(&ctx);
    assert(ret == 0);

    ring_destroy(&ctx);
}

TEST(ring_readv_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Allocate multiple aligned buffers for scatter read */
    void *buf1 = NULL, *buf2 = NULL, *buf3 = NULL;
    posix_memalign(&buf1, 4096, 1024);
    posix_memalign(&buf2, 4096, 2048);
    posix_memalign(&buf3, 4096, 1024);
    assert(buf1 && buf2 && buf3);
    memset(buf1, 0, 1024);
    memset(buf2, 0, 2048);
    memset(buf3, 0, 1024);

    /* Set up iovec */
    struct iovec iov[3] = {
        { .iov_base = buf1, .iov_len = 1024 },
        { .iov_base = buf2, .iov_len = 2048 },
        { .iov_base = buf3, .iov_len = 1024 },
    };

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    assert(req != NULL);

    req->fd = test_fd;
    req->iov = iov;
    req->iovcnt = 3;
    req->offset = 0;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;

    int ret = ring_submit_readv(&ctx, req);
    assert(ret == 0);
    assert(ctx.pending_count == 1);

    ring_flush(&ctx);
    ret = ring_wait(&ctx, 1000);
    assert(ret > 0);

    assert(callback_called == 1);
    assert(callback_result == 4096); /* Total read */

    /* Verify data scattered across buffers */
    assert(((char *)buf1)[0] == 'A');
    assert(((char *)buf2)[0] == 'A');
    assert(((char *)buf3)[0] == 'A');

    free(buf1);
    free(buf2);
    free(buf3);
    ring_destroy(&ctx);
    teardown();
}

TEST(ring_writev_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Allocate multiple buffers for gather write */
    void *buf1 = NULL, *buf2 = NULL;
    posix_memalign(&buf1, 4096, 2048);
    posix_memalign(&buf2, 4096, 2048);
    assert(buf1 && buf2);
    memset(buf1, 'X', 2048);
    memset(buf2, 'Y', 2048);

    struct iovec iov[2] = {
        { .iov_base = buf1, .iov_len = 2048 },
        { .iov_base = buf2, .iov_len = 2048 },
    };

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    req->fd = test_fd;
    req->iov = iov;
    req->iovcnt = 2;
    req->offset = 0;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;

    int ret = ring_submit_writev(&ctx, req);
    assert(ret == 0);

    ring_flush(&ctx);
    ret = ring_wait(&ctx, 1000);
    assert(ret > 0);

    assert(callback_called == 1);
    assert(callback_result == 4096);

    /* Verify gathered write */
    lseek(test_fd, 0, SEEK_SET);
    char verify[4096];
    assert(read(test_fd, verify, 4096) > 0);
    assert(verify[0] == 'X'); /* First buffer */
    assert(verify[2048] == 'Y'); /* Second buffer */

    free(buf1);
    free(buf2);
    ring_destroy(&ctx);
    teardown();
}

TEST(ring_fdatasync_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Write some data first */
    char data[] = "test data for fdatasync";
    write(test_fd, data, sizeof(data));

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    req->fd = test_fd;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;

    int ret = ring_submit_fdatasync(&ctx, req);
    assert(ret == 0);
    assert(ctx.pending_count == 1);

    ring_flush(&ctx);
    ret = ring_wait(&ctx, 1000);
    assert(ret > 0);

    assert(callback_called == 1);
    assert(callback_result == 0); /* fsync returns 0 on success */

    ring_destroy(&ctx);
    teardown();
}

TEST(ring_cancel_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Submit a read that we'll try to cancel */
    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    memset(buf, 0, 4096);

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    req->fd = test_fd;
    req->buffer = buf;
    req->len = 4096;
    req->offset = 0;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;
    callback_result = 0;

    /* Submit read but don't flush yet */
    int ret = ring_submit_read(&ctx, req);
    assert(ret == 0);

    /* Now submit a cancel request */
    int cancel_idx;
    aura_request_t *cancel_req = ring_get_request(&ctx, &cancel_idx);
    assert(cancel_req != NULL);

    ret = ring_submit_cancel(&ctx, cancel_req, req);
    assert(ret == 0);

    /* Flush both and wait */
    ring_flush(&ctx);

    /* Wait for completions - we should get 2 (original + cancel) */
    int completed = 0;
    while (ctx.pending_count > 0 && completed < 10) {
        ret = ring_wait(&ctx, 100);
        if (ret > 0) completed += ret;
    }

    /* The callback should have been called - either with success or ECANCELED */
    assert(callback_called == 1);
    /* Result is either the read result or -ECANCELED depending on timing */
    assert(callback_result == 4096 || callback_result == -ECANCELED);

    free(buf);
    ring_destroy(&ctx);
    teardown();
}

TEST(ring_request_pending_flag) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    memset(buf, 0, 4096);

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    req->fd = test_fd;
    req->buffer = buf;
    req->len = 4096;
    req->offset = 0;
    req->callback = test_callback;
    req->user_data = NULL;

    /* Before submission, pending should be false */
    assert(atomic_load(&req->pending) == false);

    ring_submit_read(&ctx, req);

    /* After submission, pending should be true */
    assert(atomic_load(&req->pending) == true);

    ring_flush(&ctx);
    ring_wait(&ctx, 1000);

    /* After completion, pending should be false */
    assert(atomic_load(&req->pending) == false);

    free(buf);
    ring_destroy(&ctx);
    teardown();
}

TEST(poll_fd_eventfd_integration) {
    setup();

    /* Create engine with public API */
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Get poll fd - should be a valid eventfd */
    int poll_fd = aura_get_poll_fd(engine);
    assert(poll_fd >= 0);

    /* Allocate buffer */
    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf != NULL);

    callback_called = 0;

    /* Submit a read */
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);

    /* Force flush the submission queue (aura_wait with 0 timeout flushes all rings) */
    aura_wait(engine, 0);

    /* Use poll() to wait on the eventfd - it should become readable when op completes */
    struct pollfd pfd = { .fd = poll_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 1000); /* 1 second timeout */
    assert(ret > 0); /* Should have data */
    assert(pfd.revents & POLLIN);

    /* Now call aura_poll to process the completion */
    ret = aura_poll(engine);
    assert(ret >= 0);

    /* Callback should have been invoked */
    assert(callback_called == 1);
    assert(callback_result == 4096);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    teardown();
}

/* ============================================================================
 * Registered Buffer Tests (Phase 5)
 * ============================================================================ */

TEST(registered_buffers_basic) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Allocate aligned buffers */
    void *buf1 = NULL, *buf2 = NULL;
    posix_memalign(&buf1, 4096, 4096);
    posix_memalign(&buf2, 4096, 4096);
    assert(buf1 && buf2);
    memset(buf1, 0, 4096);
    memset(buf2, 0, 4096);

    /* Register buffers */
    struct iovec iovs[2] = { { .iov_base = buf1, .iov_len = 4096 },
                             { .iov_base = buf2, .iov_len = 4096 } };
    int ret = aura_register_buffers(engine, iovs, 2);
    assert(ret == 0);

    /* Cannot register again without unregistering first */
    ret = aura_register_buffers(engine, iovs, 2);
    assert(ret == -1 && errno == EBUSY);

    callback_called = 0;

    /* Read using registered buffer 0 */
    aura_request_t *req =
        aura_read(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, test_callback, NULL);
    assert(req != NULL);

    aura_wait(engine, 1000);

    assert(callback_called == 1);
    assert(callback_result == 4096);
    assert(((char *)buf1)[0] == 'A'); /* Verify data was read into buf1 */

    /* Unregister */
    ret = aura_unregister(engine, AURA_REG_BUFFERS);
    assert(ret == 0);

    /* Unregister again should be no-op */
    ret = aura_unregister(engine, AURA_REG_BUFFERS);
    assert(ret == 0);

    free(buf1);
    free(buf2);
    aura_destroy(engine);
    teardown();
}

TEST(registered_buffers_callback_deferred_unregister) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    assert(buf != NULL);
    memset(buf, 0, 4096);

    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };
    int ret = aura_register_buffers(engine, &iov, 1);
    assert(ret == 0);

    unregister_cb_ctx_t cb_ctx = {
        .engine = engine,
        .fd = test_fd,
        .unregister_rc = -1,
        .nested_submit_errno = 0,
    };

    callback_called = 0;

    aura_request_t *req =
        aura_read(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, unregister_in_callback, &cb_ctx);
    assert(req != NULL);

    ret = aura_wait(engine, 1000);
    assert(ret > 0);
    assert(callback_called == 1);
    assert(callback_result == 4096);
    assert(cb_ctx.unregister_rc == 0);
    /* Since inflight counters are decremented before the callback, the
     * unregister inside the callback sees inflight==0 and completes
     * immediately.  The nested submit gets ENOENT (already unregistered). */
    assert(cb_ctx.nested_submit_errno == ENOENT);

    free(buf);
    aura_destroy(engine);
    teardown();
}

TEST(registered_buffers_request_deferred_unregister) {
    int pipefd[2];
    int ret = pipe(pipefd);
    assert(ret == 0);

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 2;
    opts.ring_select = AURA_SELECT_ROUND_ROBIN;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    void *buf1 = NULL;
    void *buf2 = NULL;
    posix_memalign(&buf1, 4096, 4096);
    posix_memalign(&buf2, 4096, 4096);
    assert(buf1 && buf2);
    memset(buf1, 0, 4096);
    memset(buf2, 0, 4096);

    struct iovec iovs[2] = {
        { .iov_base = buf1, .iov_len = 4096 },
        { .iov_base = buf2, .iov_len = 4096 },
    };
    ret = aura_register_buffers(engine, iovs, 2);
    assert(ret == 0);

    fixed_unreg_ctx_t cb_ctx;
    atomic_init(&cb_ctx.completions, 0);
    cb_ctx.first_ring = -1;
    cb_ctx.second_ring = -1;

    aura_request_t *r1 =
        aura_read(engine, pipefd[0], aura_buf_fixed(0, 0), 1, -1, fixed_unreg_callback, &cb_ctx);
    aura_request_t *r2 =
        aura_read(engine, pipefd[0], aura_buf_fixed(1, 0), 1, -1, fixed_unreg_callback, &cb_ctx);
    assert(r1 != NULL);
    assert(r2 != NULL);

    ret = aura_request_unregister(engine, AURA_REG_BUFFERS);
    assert(ret == 0);

    errno = 0;
    aura_request_t *blocked = aura_read(engine, pipefd[0], aura_buf_fixed(0, 0), 1, -1, NULL, NULL);
    assert(blocked == NULL && errno == EBUSY);

    const char bytes[2] = { 'x', 'y' };
    ssize_t written = write(pipefd[1], bytes, sizeof(bytes));
    assert(written == (ssize_t)sizeof(bytes));

    while (atomic_load_explicit(&cb_ctx.completions, memory_order_relaxed) < 2) {
        ret = aura_wait(engine, 1000);
        assert(ret >= 0);
    }

    /* With round-robin enabled, the first two submissions should split rings. */
    assert(cb_ctx.first_ring >= 0);
    assert(cb_ctx.second_ring >= 0);
    assert(cb_ctx.first_ring != cb_ctx.second_ring);

    errno = 0;
    aura_request_t *after = aura_read(engine, pipefd[0], aura_buf_fixed(0, 0), 1, -1, NULL, NULL);
    assert(after == NULL && errno == ENOENT);

    close(pipefd[0]);
    close(pipefd[1]);
    free(buf1);
    free(buf2);
    aura_destroy(engine);
}

TEST(registered_buffers_write_fixed) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    assert(buf != NULL);
    memset(buf, 'Z', 4096);

    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };
    int ret = aura_register_buffers(engine, &iov, 1);
    assert(ret == 0);

    callback_called = 0;

    /* Write using registered buffer */
    aura_request_t *req =
        aura_write(engine, test_fd, aura_buf_fixed(0, 0), 4096, 0, test_callback, NULL);
    assert(req != NULL);

    aura_wait(engine, 1000);

    assert(callback_called == 1);
    assert(callback_result == 4096);

    /* Verify data was written */
    lseek(test_fd, 0, SEEK_SET);
    char verify[4096];
    assert(read(test_fd, verify, 4096) > 0);
    assert(verify[0] == 'Z');

    aura_unregister(engine, AURA_REG_BUFFERS);
    free(buf);
    aura_destroy(engine);
    teardown();
}

TEST(registered_buffers_offset) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    assert(buf != NULL);
    memset(buf, 0, 4096);

    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };
    int ret = aura_register_buffers(engine, &iov, 1);
    assert(ret == 0);

    callback_called = 0;

    /* Read into buffer at offset 1024 */
    aura_request_t *req =
        aura_read(engine, test_fd, aura_buf_fixed(0, 1024), 1024, 0, test_callback, NULL);
    assert(req != NULL);

    aura_wait(engine, 1000);

    assert(callback_called == 1);
    assert(callback_result == 1024);
    /* First 1024 bytes should still be 0, next 1024 should be 'A' */
    assert(((char *)buf)[0] == 0);
    assert(((char *)buf)[1024] == 'A');

    aura_unregister(engine, AURA_REG_BUFFERS);
    free(buf);
    aura_destroy(engine);
    teardown();
}

TEST(registered_buffers_invalid) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Read without registering buffers */
    aura_request_t *req = aura_read(engine, 0, aura_buf_fixed(0, 0), 4096, 0, NULL, NULL);
    assert(req == NULL && errno == ENOENT);

    /* Register a buffer */
    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };
    int reg_ret = aura_register_buffers(engine, &iov, 1);
    assert(reg_ret == 0);

    /* Invalid buffer index */
    req = aura_read(engine, 0, aura_buf_fixed(5, 0), 4096, 0, NULL, NULL);
    assert(req == NULL && errno == EINVAL);

    /* Buffer overflow (offset + len > buffer size) */
    req = aura_read(engine, 0, aura_buf_fixed(0, 1024), 4096, 0, NULL, NULL);
    assert(req == NULL && errno == EOVERFLOW);

    aura_unregister(engine, AURA_REG_BUFFERS);
    free(buf);
    aura_destroy(engine);
}

TEST(sqpoll_option) {
    /* Test SQPOLL option is parsed correctly.
     * Note: SQPOLL requires root/CAP_SYS_NICE, so it may fall back to normal mode. */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.enable_sqpoll = true;
    opts.sqpoll_idle_ms = 500;

    aura_engine_t *engine = aura_create_with_options(&opts);
    /* Engine creation should succeed even if SQPOLL fails (falls back to normal) */
    assert(engine != NULL);

    aura_destroy(engine);
}

/* ============================================================================
 * Registered Files Tests
 * ============================================================================ */

TEST(registered_files_basic) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Register the test file */
    int fds[2] = { test_fd, test_fd };
    int ret = aura_register_files(engine, fds, 2);
    assert(ret == 0);

    /* Cannot register again without unregistering first */
    ret = aura_register_files(engine, fds, 2);
    assert(ret == -1 && errno == EBUSY);

    /* Unregister */
    ret = aura_unregister(engine, AURA_REG_FILES);
    assert(ret == 0);

    /* Unregister again should be no-op */
    ret = aura_unregister(engine, AURA_REG_FILES);
    assert(ret == 0);

    aura_destroy(engine);
    teardown();
}

TEST(registered_files_update) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Register the test file */
    int fds[2] = { test_fd, -1 }; /* Second slot empty */
    int ret = aura_register_files(engine, fds, 2);
    assert(ret == 0);

    /* Update slot 1 with test_fd */
    ret = aura_update_file(engine, 1, test_fd);
    assert(ret == 0);

    /* Update slot 0 with -1 (unregister) */
    ret = aura_update_file(engine, 0, -1);
    assert(ret == 0);

    /* Invalid index */
    errno = 0;
    ret = aura_update_file(engine, 10, test_fd);
    assert(ret == -1 && errno == EINVAL);

    /* Negative index */
    errno = 0;
    ret = aura_update_file(engine, -1, test_fd);
    assert(ret == -1 && errno == EINVAL);

    aura_unregister(engine, AURA_REG_FILES);
    aura_destroy(engine);
    teardown();
}

TEST(registered_files_invalid) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* NULL fds array */
    errno = 0;
    int ret = aura_register_files(engine, NULL, 1);
    assert(ret == -1 && errno == EINVAL);

    /* Zero count */
    int fd = 0;
    errno = 0;
    ret = aura_register_files(engine, &fd, 0);
    assert(ret == -1 && errno == EINVAL);

    /* Overflow count */
    errno = 0;
    ret = aura_register_files(engine, &fd, UINT_MAX);
    assert(ret == -1 && errno == EINVAL);

    /* Update without registering first */
    errno = 0;
    ret = aura_update_file(engine, 0, 0);
    assert(ret == -1 && errno == ENOENT);

    aura_destroy(engine);
}

TEST(registered_files_callback_request_unregister) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    int fds[1] = { test_fd };
    int ret = aura_register_files(engine, fds, 1);
    assert(ret == 0);

    char *buf = malloc(4096);
    assert(buf != NULL);
    memset(buf, 0, 4096);

    files_unreg_cb_ctx_t cb_ctx = {
        .engine = engine,
        .request_rc = -1,
        .update_rc = 0,
        .update_errno = 0,
    };
    callback_called = 0;
    callback_result = 0;

    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0,
                                    request_unregister_files_in_callback, &cb_ctx);
    assert(req != NULL);

    ret = aura_wait(engine, 1000);
    assert(ret > 0);
    assert(callback_called == 1);
    assert(callback_result == 4096);
    assert(cb_ctx.request_rc == 0);
    assert(cb_ctx.update_rc == -1);
    assert(cb_ctx.update_errno == ENOENT || cb_ctx.update_errno == EBUSY);

    /* File table should be unregistered after callback path. */
    ret = aura_register_files(engine, fds, 1);
    assert(ret == 0);
    ret = aura_unregister(engine, AURA_REG_FILES);
    assert(ret == 0);

    free(buf);
    aura_destroy(engine);
    teardown();
}

TEST(register_buffers_overflow_count) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = NULL;
    posix_memalign(&buf, 4096, 4096);
    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };

    /* Overflow count should fail with EINVAL */
    errno = 0;
    int ret = aura_register_buffers(engine, &iov, UINT_MAX);
    assert(ret == -1 && errno == EINVAL);

    /* Large count: allocate a proper array so memcpy is safe.
     * The kernel may accept or reject large registrations depending
     * on limits, so we just verify it doesn't crash.  Unregister
     * if it succeeded. */
    int large_count = 100000;
    struct iovec *large_iovs = calloc((size_t)large_count, sizeof(struct iovec));
    assert(large_iovs != NULL);
    ret = aura_register_buffers(engine, large_iovs, large_count);
    if (ret == 0) {
        aura_unregister(engine, AURA_REG_BUFFERS);
    }
    free(large_iovs);

    free(buf);
    aura_destroy(engine);
}

TEST(register_files_overflow_count) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Overflow count should fail with EINVAL */
    int fd = 0;
    errno = 0;
    int ret = aura_register_files(engine, &fd, UINT_MAX);
    assert(ret == -1 && errno == EINVAL);

    /* Large count: allocate a proper array so memcpy is safe.
     * The kernel may accept large file registrations, so we just
     * verify it doesn't crash.  Unregister if it succeeded. */
    int large_count = 100000;
    int *large_fds = calloc((size_t)large_count, sizeof(int));
    assert(large_fds != NULL);
    ret = aura_register_files(engine, large_fds, large_count);
    if (ret == 0) {
        aura_unregister(engine, AURA_REG_FILES);
    }
    free(large_fds);

    aura_destroy(engine);
}

/* ============================================================================
 * Fsync Tests
 * ============================================================================ */

TEST(ring_fsync_basic) {
    setup();

    ring_ctx_t ctx;
    ring_init(&ctx, 64, -1, NULL);

    /* Write some data first */
    char data[] = "test data for fsync";
    write(test_fd, data, sizeof(data));

    int op_idx;
    aura_request_t *req = ring_get_request(&ctx, &op_idx);
    req->fd = test_fd;
    req->callback = test_callback;
    req->user_data = NULL;

    callback_called = 0;

    int ret = ring_submit_fsync(&ctx, req);
    assert(ret == 0);
    assert(ctx.pending_count == 1);

    ring_flush(&ctx);
    ret = ring_wait(&ctx, 1000);
    assert(ret > 0);

    assert(callback_called == 1);
    assert(callback_result == 0); /* fsync returns 0 on success */

    ring_destroy(&ctx);
    teardown();
}

TEST(aura_fsync_both_modes) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Write some data */
    char data[] = "test data for fsync test";
    write(test_fd, data, sizeof(data));

    callback_called = 0;

    /* Test fsync with DEFAULT flag */
    aura_request_t *req = aura_fsync(engine, test_fd, AURA_FSYNC_DEFAULT, test_callback, NULL);
    assert(req != NULL);

    aura_wait(engine, 1000);
    assert(callback_called == 1);
    assert(callback_result == 0);

    /* Test fsync with DATASYNC flag */
    callback_called = 0;
    req = aura_fsync(engine, test_fd, AURA_FSYNC_DATASYNC, test_callback, NULL);
    assert(req != NULL);

    aura_wait(engine, 1000);
    assert(callback_called == 1);
    assert(callback_result == 0);

    aura_destroy(engine);
    teardown();
}

/* ============================================================================
 * Event Loop Tests
 * ============================================================================ */

static _Atomic int run_stop_callback_count = 0;

static void run_stop_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)result;
    aura_engine_t *engine = user_data;
    run_stop_callback_count++;

    /* After 3 completions, stop the event loop */
    if (run_stop_callback_count >= 3) {
        aura_stop(engine);
    }
}

static void *run_thread_func(void *arg) {
    aura_engine_t *engine = arg;
    aura_run(engine);
    return NULL;
}

TEST(aura_run_stop) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    run_stop_callback_count = 0;

    /* Allocate buffer */
    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf != NULL);

    /* Submit some reads that will trigger the callback which stops the loop */
    for (int i = 0; i < 5; i++) {
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, run_stop_callback, engine);
        assert(req != NULL);
    }

    /* Start event loop in a separate thread */
    pthread_t thread;
    int ret = pthread_create(&thread, NULL, run_thread_func, engine);
    assert(ret == 0);

    /* Wait for thread to finish (should stop after 3 completions) */
    ret = pthread_join(thread, NULL);
    assert(ret == 0);

    /* Should have completed at least 3 */
    assert(run_stop_callback_count >= 3);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    teardown();
}

TEST(aura_stop_before_run) {
    /* Test that stop before run doesn't cause issues */
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Stop without run - should be safe */
    aura_stop(engine);

    /* Destroy should also be safe */
    aura_destroy(engine);
}

/* ============================================================================
 * Error Path Tests
 * ============================================================================ */

TEST(error_null_engine) {
    void *buf = malloc(4096);

    /* aura_read with NULL engine */
    errno = 0;
    aura_request_t *req = aura_read(NULL, 0, aura_buf(buf), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_write with NULL engine */
    errno = 0;
    req = aura_write(NULL, 0, aura_buf(buf), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_readv with NULL engine */
    struct iovec iov = { .iov_base = buf, .iov_len = 4096 };
    errno = 0;
    req = aura_readv(NULL, 0, &iov, 1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_writev with NULL engine */
    errno = 0;
    req = aura_writev(NULL, 0, &iov, 1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_fsync with NULL engine */
    errno = 0;
    req = aura_fsync(NULL, 0, AURA_FSYNC_DEFAULT, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_cancel with NULL engine */
    errno = 0;
    int ret = aura_cancel(NULL, (aura_request_t *)0x1);
    assert(ret == -1);
    assert(errno == EINVAL);

    /* aura_get_poll_fd with NULL engine */
    errno = 0;
    ret = aura_get_poll_fd(NULL);
    assert(ret == -1);
    assert(errno == EINVAL);

    free(buf);
}

TEST(error_invalid_fd) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);

    /* aura_read with invalid fd */
    errno = 0;
    aura_request_t *req = aura_read(engine, -1, aura_buf(buf), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_write with invalid fd */
    errno = 0;
    req = aura_write(engine, -1, aura_buf(buf), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_fsync with invalid fd */
    errno = 0;
    req = aura_fsync(engine, -1, AURA_FSYNC_DEFAULT, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
}

TEST(error_null_buffer) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* aura_read with NULL buffer */
    errno = 0;
    aura_request_t *req = aura_read(engine, 0, aura_buf(NULL), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_write with NULL buffer */
    errno = 0;
    req = aura_write(engine, 0, aura_buf(NULL), 4096, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_readv with NULL iov */
    errno = 0;
    req = aura_readv(engine, 0, NULL, 1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_readv with zero iovcnt */
    struct iovec iov = { .iov_base = (void *)0x1, .iov_len = 4096 };
    errno = 0;
    req = aura_readv(engine, 0, &iov, 0, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_readv with negative iovcnt */
    errno = 0;
    req = aura_readv(engine, 0, &iov, -1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_writev with negative iovcnt */
    errno = 0;
    req = aura_writev(engine, 0, &iov, -1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_readv with iovcnt > IOV_MAX */
    errno = 0;
    req = aura_readv(engine, 0, &iov, IOV_MAX + 1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_writev with iovcnt > IOV_MAX */
    errno = 0;
    req = aura_writev(engine, 0, &iov, IOV_MAX + 1, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(error_zero_length) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);

    /* aura_read with zero length */
    errno = 0;
    aura_request_t *req = aura_read(engine, 0, aura_buf(buf), 0, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    /* aura_write with zero length */
    errno = 0;
    req = aura_write(engine, 0, aura_buf(buf), 0, 0, NULL, NULL);
    assert(req == NULL);
    assert(errno == EINVAL);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
}

TEST(error_cancel_null_request) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* aura_cancel with NULL request */
    errno = 0;
    int ret = aura_cancel(engine, NULL);
    assert(ret == -1);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(error_request_introspection_null) {
    /* Request introspection with NULL */
    assert(aura_request_pending(NULL) == false);
    assert(aura_request_fd(NULL) == -1);
    assert(aura_request_user_data(NULL) == NULL);
}

TEST(error_buffer_pool_invalid) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* aura_buffer_alloc with NULL engine */
    errno = 0;
    void *buf = aura_buffer_alloc(NULL, 4096);
    assert(buf == NULL);
    assert(errno == EINVAL);

    /* aura_buffer_alloc with zero size */
    errno = 0;
    buf = aura_buffer_alloc(engine, 0);
    assert(buf == NULL);
    assert(errno == EINVAL);

    /* aura_buffer_free with NULL - should not crash */
    aura_buffer_free(NULL, (void *)0x1);
    aura_buffer_free(engine, NULL);

    aura_destroy(engine);
}

/* ============================================================================
 * Shutdown Tests
 * ============================================================================ */

TEST(shutdown_rejects_new_submissions) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf != NULL);

    /* Submit a request successfully */
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);

    /* Flush and wait for completion before destroy to avoid hanging */
    aura_wait(engine, 1000);

    /* Free buffer before destroy */
    aura_buffer_free(engine, buf);

    /* Start shutdown - this will set the shutting_down flag */
    aura_destroy(engine);

    /* Note: After destroy, we can't test further since engine is gone.
     * This test just verifies destroy doesn't crash with pending ops. */

    teardown();
}

TEST(shutdown_drains_pending_ops) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);
    callback_called = 0;

    /* Submit multiple requests */
    for (int i = 0; i < 5; i++) {
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
    }

    /* Wait for completions - this ensures ops are flushed */
    int total = 0;
    for (int i = 0; i < 50 && total < 5; i++) {
        int completed = aura_wait(engine, 100);
        if (completed > 0) total += completed;
    }

    /* Free buffer before destroy */
    aura_buffer_free(engine, buf);

    /* Destroy should complete cleanly */
    aura_destroy(engine);

    /* All 5 operations should have completed */
    assert(total >= 5);

    teardown();
}

/* ============================================================================
 * Stats Tests
 * ============================================================================ */

TEST(stats_basic) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));

    /* Initially, stats should be zero or near-zero */
    assert(stats.ops_completed == 0);
    assert(stats.bytes_transferred == 0);
    assert(stats.current_in_flight == 0);

    /* Submit and complete an operation */
    void *buf = aura_buffer_alloc(engine, 4096);
    callback_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);

    aura_wait(engine, 1000);
    assert(callback_called == 1);

    /* Check stats after completion */
    aura_get_stats(engine, &stats, sizeof(stats));
    assert(stats.ops_completed >= 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    teardown();
}

TEST(stats_null_params) {
    /* Should not crash with NULL params */
    aura_get_stats(NULL, NULL, 0);

    aura_engine_t *engine = aura_create();
    aura_get_stats(engine, NULL, 0);
    aura_get_stats(NULL, &(aura_stats_t){ 0 }, sizeof(aura_stats_t));
    aura_destroy(engine);
}

/* ============================================================================
 * Integration / Load Tests
 * ============================================================================ */

static _Atomic int concurrent_callback_count = 0;

static void concurrent_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    if (result > 0) {
        atomic_fetch_add_explicit(&concurrent_callback_count, 1, memory_order_relaxed);
    }
}

TEST(load_many_concurrent_reads) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    const int NUM_OPS = 50;
    void *bufs[NUM_OPS];
    concurrent_callback_count = 0;

    /* Submit many concurrent reads */
    int submitted = 0;
    for (int i = 0; i < NUM_OPS; i++) {
        bufs[i] = aura_buffer_alloc(engine, 4096);
        assert(bufs[i] != NULL);

        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(bufs[i]), 4096, 0, concurrent_callback, NULL);
        if (req != NULL) {
            submitted++;
        }
        /* Some may return EAGAIN if we hit the limit, that's expected */
    }

    /* Wait for completions */
    int total_completed = 0;
    for (int i = 0; i < 100 && total_completed < submitted; i++) {
        int completed = aura_wait(engine, 100);
        if (completed > 0) {
            total_completed += completed;
        }
    }

    /* All submitted ops should complete */
    assert(concurrent_callback_count == submitted);

    for (int i = 0; i < NUM_OPS; i++) {
        aura_buffer_free(engine, bufs[i]);
    }
    aura_destroy(engine);
    teardown();
}

TEST(load_mixed_read_write) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    concurrent_callback_count = 0;

    /* Alternate between reads and writes */
    const int NUM_OPS = 20;
    void *bufs[NUM_OPS];
    int submitted = 0;

    for (int i = 0; i < NUM_OPS; i++) {
        bufs[i] = aura_buffer_alloc(engine, 4096);
        memset(bufs[i], 'X' + (i % 26), 4096);

        aura_request_t *req;
        if (i % 2 == 0) {
            req = aura_read(engine, test_fd, aura_buf(bufs[i]), 4096, 0, concurrent_callback, NULL);
        } else {
            req =
                aura_write(engine, test_fd, aura_buf(bufs[i]), 4096, 0, concurrent_callback, NULL);
        }
        if (req != NULL) {
            submitted++;
        }
    }

    /* Wait for completions */
    int total_completed = 0;
    for (int i = 0; i < 100 && total_completed < submitted; i++) {
        int completed = aura_wait(engine, 100);
        if (completed > 0) {
            total_completed += completed;
        }
    }

    /* All submitted ops should complete */
    assert(concurrent_callback_count == submitted);

    for (int i = 0; i < NUM_OPS; i++) {
        aura_buffer_free(engine, bufs[i]);
    }
    aura_destroy(engine);
    teardown();
}

TEST(load_vectored_io) {
    setup();

    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    concurrent_callback_count = 0;

    /* Submit multiple vectored operations */
    const int NUM_OPS = 10;
    void *bufs[NUM_OPS][3];
    struct iovec iovs[NUM_OPS][3];
    int submitted = 0;

    for (int i = 0; i < NUM_OPS; i++) {
        for (int j = 0; j < 3; j++) {
            posix_memalign(&bufs[i][j], 4096, 1024);
            memset(bufs[i][j], 0, 1024);
            iovs[i][j].iov_base = bufs[i][j];
            iovs[i][j].iov_len = 1024;
        }

        aura_request_t *req = aura_readv(engine, test_fd, iovs[i], 3, 0, concurrent_callback, NULL);
        if (req != NULL) {
            submitted++;
        }
    }

    /* Wait for completions */
    int total_completed = 0;
    for (int i = 0; i < 100 && total_completed < submitted; i++) {
        int completed = aura_wait(engine, 100);
        if (completed > 0) {
            total_completed += completed;
        }
    }

    assert(concurrent_callback_count == submitted);

    for (int i = 0; i < NUM_OPS; i++) {
        for (int j = 0; j < 3; j++) {
            free(bufs[i][j]);
        }
    }
    aura_destroy(engine);
    teardown();
}

/* ============================================================================
 * Drain Tests
 * ============================================================================ */

static _Atomic int drain_callback_count = 0;

static void drain_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)result;
    (void)user_data;
    drain_callback_count++;
}

TEST(drain_basic) {
    setup();
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    drain_callback_count = 0;

    /* Submit some reads */
    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf != NULL);
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, drain_callback, NULL);
    assert(req != NULL);

    /* Drain with generous timeout - should complete */
    int n = aura_drain(engine, 5000);
    assert(n >= 0);
    assert(drain_callback_count == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    teardown();
}

TEST(drain_already_empty) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Drain with no pending ops should return 0 immediately */
    int n = aura_drain(engine, 1000);
    assert(n == 0);

    aura_destroy(engine);
}

TEST(drain_nonblocking) {
    setup();
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    drain_callback_count = 0;

    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf != NULL);
    aura_request_t *drain_req =
        aura_read(engine, test_fd, aura_buf(buf), 4096, 0, drain_callback, NULL);
    assert(drain_req != NULL);

    /* Non-blocking drain (timeout=0) - may or may not complete */
    int n = aura_drain(engine, 0);
    assert(n >= 0); /* Should not return error */

    /* Drain remaining with timeout */
    aura_drain(engine, 5000);
    assert(drain_callback_count == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    teardown();
}

TEST(drain_null_engine) {
    int n = aura_drain(NULL, 1000);
    assert(n == -1);
    assert(errno == EINVAL);
}

/* ============================================================================
 * Version API Tests
 * ============================================================================ */

TEST(version_api) {
    const char *version = aura_version();
    assert(version != NULL);
    assert(strlen(version) > 0);
    assert(strstr(version, ".") != NULL); /* Should contain a dot */

    int version_int = aura_version_int();
    assert(version_int >= 100); /* At least 0.1.0 */
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    atexit(cleanup_atexit);
    printf("Running io_uring ring tests...\n");

    RUN_TEST(ring_init);
    RUN_TEST(ring_init_invalid);
    RUN_TEST(ring_request_management);
    RUN_TEST(ring_can_submit);
    RUN_TEST(ring_should_flush);
    RUN_TEST(ring_read_basic);
    RUN_TEST(ring_write_basic);
    RUN_TEST(ring_poll_no_ops);
    RUN_TEST(ring_readv_basic);
    RUN_TEST(ring_writev_basic);
    RUN_TEST(ring_fdatasync_basic);
    RUN_TEST(ring_cancel_basic);
    RUN_TEST(ring_request_pending_flag);
    RUN_TEST(poll_fd_eventfd_integration);

    /* Phase 5: Registered buffers and SQPOLL */
    RUN_TEST(registered_buffers_basic);
    RUN_TEST(registered_buffers_callback_deferred_unregister);
    RUN_TEST(registered_buffers_request_deferred_unregister);
    RUN_TEST(registered_buffers_write_fixed);
    RUN_TEST(registered_buffers_offset);
    RUN_TEST(registered_buffers_invalid);
    RUN_TEST(sqpoll_option);

    /* Registered files tests */
    RUN_TEST(registered_files_basic);
    RUN_TEST(registered_files_update);
    RUN_TEST(registered_files_invalid);
    RUN_TEST(registered_files_callback_request_unregister);
    RUN_TEST(register_buffers_overflow_count);
    RUN_TEST(register_files_overflow_count);

    /* Fsync tests */
    RUN_TEST(ring_fsync_basic);
    RUN_TEST(aura_fsync_both_modes);

    /* Event loop tests */
    RUN_TEST(aura_run_stop);
    RUN_TEST(aura_stop_before_run);

    /* Error path tests */
    RUN_TEST(error_null_engine);
    RUN_TEST(error_invalid_fd);
    RUN_TEST(error_null_buffer);
    RUN_TEST(error_zero_length);
    RUN_TEST(error_cancel_null_request);
    RUN_TEST(error_request_introspection_null);
    RUN_TEST(error_buffer_pool_invalid);

    /* Shutdown tests */
    RUN_TEST(shutdown_rejects_new_submissions);
    RUN_TEST(shutdown_drains_pending_ops);

    /* Stats tests */
    RUN_TEST(stats_basic);
    RUN_TEST(stats_null_params);

    /* Integration / load tests */
    RUN_TEST(load_many_concurrent_reads);
    RUN_TEST(load_mixed_read_write);
    RUN_TEST(load_vectored_io);

    /* Drain tests */
    RUN_TEST(drain_basic);
    RUN_TEST(drain_already_empty);
    RUN_TEST(drain_nonblocking);
    RUN_TEST(drain_null_engine);

    /* Version API tests */
    RUN_TEST(version_api);

    printf("\n%d tests passed\n", test_count);
    return 0;
}
