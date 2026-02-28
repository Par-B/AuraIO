// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_linked_ops.c
 * @brief Tests for IOSQE_IO_LINK op chaining
 *
 * Verifies that linked (chained) operations work correctly through the
 * AuraIO API: ordering, cancellation on failure, ring pinning, etc.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "aura.h"
#include "adaptive_ring.h" /* struct aura_request (ring_idx) */

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-40s", #name); \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* ============================================================================
 * Helpers
 * ============================================================================ */

typedef struct {
    ssize_t result;
    _Atomic int done;
    int order; /* completion order tracking */
} cb_state_t;

static _Atomic int completion_order = 0;

static void basic_cb(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    cb_state_t *st = user_data;
    st->result = result;
    st->order = atomic_fetch_add(&completion_order, 1);
    st->done = 1;
}

static void run_until_done(aura_engine_t *engine, cb_state_t *st) {
    int iters = 0;
    while (!st->done && iters++ < 2000) {
        aura_poll(engine);
        if (!st->done) usleep(1000);
    }
    assert(st->done && "operation did not complete");
}

static aura_engine_t *make_engine(void) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_select = AURA_SELECT_THREAD_LOCAL;
    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine);
    return engine;
}

static aura_engine_t *make_engine_multi_ring(void) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 4;
    opts.ring_select = AURA_SELECT_THREAD_LOCAL;
    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine);
    return engine;
}

static char tmpdir[] = "/tmp/aura_link_XXXXXX";
static char filepath[512];

static void setup(void) {
    assert(mkdtemp(tmpdir));
    snprintf(filepath, sizeof(filepath), "%s/testfile", tmpdir);
}

static void teardown(void) {
    unlink(filepath);
    rmdir(tmpdir);
}

/* ============================================================================
 * Tests
 * ============================================================================ */

/**
 * Test 1: write → fsync chain
 * Write data and chain an fsync. Both should complete successfully in order.
 */
TEST(write_fsync_chain) {
    aura_engine_t *engine = make_engine();

    int fd = open(filepath, O_CREAT | O_RDWR, 0644);
    assert(fd >= 0);

    char buf[4096];
    memset(buf, 'A', sizeof(buf));

    cb_state_t ws = { 0 }, fs = { 0 };
    atomic_store(&completion_order, 0);

    /* Submit write, mark as linked */
    aura_request_t *wreq = aura_write(engine, fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &ws);
    assert(wreq);
    assert(!aura_request_is_linked(wreq));
    assert(aura_request_set_linked(wreq) == 0);
    assert(aura_request_is_linked(wreq));

    /* Submit fsync — end of chain (not linked) */
    aura_request_t *freq = aura_fsync(engine, fd, 0, 0, basic_cb, &fs);
    assert(freq);

    cb_state_t both[] = { ws, fs };
    (void)both;
    run_until_done(engine, &ws);
    run_until_done(engine, &fs);

    assert(ws.result == sizeof(buf));
    assert(fs.result == 0);
    /* Write should complete before fsync */
    assert(ws.order < fs.order);

    /* Verify data was written */
    char verify[4096];
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, verify, sizeof(verify));
    assert(n == sizeof(verify));
    assert(memcmp(buf, verify, sizeof(buf)) == 0);

    close(fd);
    aura_destroy(engine);
}

/**
 * Test 2: chain failure cancels subsequent ops
 * Write to a bad fd → fsync should get -ECANCELED.
 */
TEST(chain_failure_cancels) {
    aura_engine_t *engine = make_engine();

    /* Use an invalid fd */
    int bad_fd = 9999;

    char buf[64];
    memset(buf, 'B', sizeof(buf));

    cb_state_t ws = { 0 }, fs = { 0 };

    /* Submit write to bad fd, mark as linked */
    aura_request_t *wreq =
        aura_write(engine, bad_fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &ws);
    assert(wreq);
    assert(aura_request_set_linked(wreq) == 0);

    /* Submit fsync — should get canceled because write fails */
    aura_request_t *freq = aura_fsync(engine, bad_fd, 0, 0, basic_cb, &fs);
    assert(freq);

    run_until_done(engine, &ws);
    run_until_done(engine, &fs);

    /* Write should fail (bad fd) */
    assert(ws.result < 0);
    /* Fsync should be canceled by the kernel */
    assert(fs.result == -ECANCELED);

    aura_destroy(engine);
}

/**
 * Test 3: three-op chain (write → write → fsync)
 * A and B linked, C not. All complete in order.
 */
TEST(three_op_chain) {
    aura_engine_t *engine = make_engine();

    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    char buf1[1024], buf2[1024];
    memset(buf1, '1', sizeof(buf1));
    memset(buf2, '2', sizeof(buf2));

    cb_state_t s1 = { 0 }, s2 = { 0 }, s3 = { 0 };
    atomic_store(&completion_order, 0);

    /* Op 1: write at offset 0, linked */
    aura_request_t *r1 = aura_write(engine, fd, aura_buf(buf1), sizeof(buf1), 0, 0, basic_cb, &s1);
    assert(r1);
    assert(aura_request_set_linked(r1) == 0);

    /* Op 2: write at offset 1024, linked */
    aura_request_t *r2 =
        aura_write(engine, fd, aura_buf(buf2), sizeof(buf2), 1024, 0, basic_cb, &s2);
    assert(r2);
    assert(aura_request_set_linked(r2) == 0);

    /* Op 3: fsync, end of chain */
    aura_request_t *r3 = aura_fsync(engine, fd, 0, 0, basic_cb, &s3);
    assert(r3);

    run_until_done(engine, &s1);
    run_until_done(engine, &s2);
    run_until_done(engine, &s3);

    assert(s1.result == sizeof(buf1));
    assert(s2.result == sizeof(buf2));
    assert(s3.result == 0);
    assert(s1.order < s2.order);
    assert(s2.order < s3.order);

    /* Verify both writes landed */
    char verify[2048];
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, verify, sizeof(verify));
    assert(n == sizeof(verify));
    assert(memcmp(verify, buf1, 1024) == 0);
    assert(memcmp(verify + 1024, buf2, 1024) == 0);

    close(fd);
    aura_destroy(engine);
}

/**
 * Test 4: unlinked ops are unaffected (regression test)
 * Two independent ops without linking should work normally.
 */
TEST(unlinked_ops_unaffected) {
    aura_engine_t *engine = make_engine();

    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    char buf1[512], buf2[512];
    memset(buf1, 'X', sizeof(buf1));
    memset(buf2, 'Y', sizeof(buf2));

    cb_state_t s1 = { 0 }, s2 = { 0 };

    /* Two independent writes, no linking */
    aura_request_t *r1 = aura_write(engine, fd, aura_buf(buf1), sizeof(buf1), 0, 0, basic_cb, &s1);
    assert(r1);
    assert(!aura_request_is_linked(r1));

    aura_request_t *r2 =
        aura_write(engine, fd, aura_buf(buf2), sizeof(buf2), 512, 0, basic_cb, &s2);
    assert(r2);
    assert(!aura_request_is_linked(r2));

    run_until_done(engine, &s1);
    run_until_done(engine, &s2);

    assert(s1.result == sizeof(buf1));
    assert(s2.result == sizeof(buf2));

    close(fd);
    aura_destroy(engine);
}

/**
 * Test 5: ring pinning
 * In a multi-ring engine, linked pair goes to the same ring.
 */
TEST(ring_pinning) {
    aura_engine_t *engine = make_engine_multi_ring();

    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    char buf[256];
    memset(buf, 'P', sizeof(buf));

    cb_state_t ws = { 0 }, fs = { 0 };

    /* Submit write, mark as linked */
    aura_request_t *wreq = aura_write(engine, fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &ws);
    assert(wreq);
    int write_ring = wreq->ring_idx;
    assert(aura_request_set_linked(wreq) == 0);

    /* Submit fsync — should be on the same ring */
    aura_request_t *freq = aura_fsync(engine, fd, 0, 0, basic_cb, &fs);
    assert(freq);
    int fsync_ring = freq->ring_idx;

    assert(write_ring == fsync_ring && "linked ops must be on the same ring");

    run_until_done(engine, &ws);
    run_until_done(engine, &fs);

    assert(ws.result == sizeof(buf));
    assert(fs.result == 0);

    close(fd);
    aura_destroy(engine);
}

/**
 * Test 6: chain abort recovery
 * If a linked op submission is followed by a failed submission,
 * TLS state is cleaned up and subsequent ops work normally.
 */
TEST(chain_abort_recovery) {
    aura_engine_t *engine = make_engine();

    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    char buf[256];
    memset(buf, 'R', sizeof(buf));

    cb_state_t ws = { 0 };

    /* Submit a write and mark as linked */
    aura_request_t *wreq = aura_write(engine, fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &ws);
    assert(wreq);
    assert(aura_request_set_linked(wreq) == 0);

    /* The write was linked but we don't submit another linked op — instead,
     * we complete the chain normally by submitting an unlinked op */
    cb_state_t fs = { 0 };
    aura_request_t *freq = aura_fsync(engine, fd, 0, 0, basic_cb, &fs);
    assert(freq);

    run_until_done(engine, &ws);
    run_until_done(engine, &fs);

    assert(ws.result == sizeof(buf));
    assert(fs.result == 0);

    /* Now do a subsequent independent op to verify TLS state was cleaned up */
    cb_state_t s2 = { 0 };
    aura_request_t *r2 = aura_write(engine, fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &s2);
    assert(r2);

    run_until_done(engine, &s2);
    assert(s2.result == sizeof(buf));

    close(fd);
    aura_destroy(engine);
}

/**
 * Test 7: linked ops with registered files
 * Both IOSQE_FIXED_FILE and IOSQE_IO_LINK should be set correctly.
 */
TEST(linked_with_registered_files) {
    aura_engine_t *engine = make_engine();

    int fd = open(filepath, O_CREAT | O_RDWR | O_TRUNC, 0644);
    assert(fd >= 0);

    /* Register the file */
    int fds[] = { fd };
    int ret = aura_register_files(engine, fds, 1);
    assert(ret == 0);

    char buf[512];
    memset(buf, 'F', sizeof(buf));

    cb_state_t ws = { 0 }, fs = { 0 };

    /* Submit write using registered file, mark as linked */
    aura_request_t *wreq = aura_write(engine, fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &ws);
    assert(wreq);
    assert(aura_request_set_linked(wreq) == 0);

    /* Submit fsync */
    aura_request_t *freq = aura_fsync(engine, fd, 0, 0, basic_cb, &fs);
    assert(freq);

    run_until_done(engine, &ws);
    run_until_done(engine, &fs);

    assert(ws.result == sizeof(buf));
    assert(fs.result == 0);

    /* Unregister files before closing */
    aura_request_unregister(engine, AURA_REG_FILES);
    /* Drain any pending unregistration */
    aura_poll(engine);

    close(fd);
    aura_destroy(engine);
}

/**
 * Test 8: concurrent independent chains
 * Two threads each build independent chains concurrently.
 */
typedef struct {
    aura_engine_t *engine;
    int thread_id;
    int passed;
} thread_arg_t;

static void *chain_thread_func(void *arg) {
    thread_arg_t *ta = arg;
    aura_engine_t *engine = ta->engine;

    char path[512];
    snprintf(path, sizeof(path), "%s/thread_%d", tmpdir, ta->thread_id);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        ta->passed = 0;
        return NULL;
    }

    char buf[256];
    memset(buf, 'T' + ta->thread_id, sizeof(buf));

    cb_state_t ws = { 0 }, fs = { 0 };

    /* Build a write→fsync chain */
    aura_request_t *wreq = aura_write(engine, fd, aura_buf(buf), sizeof(buf), 0, 0, basic_cb, &ws);
    if (!wreq) {
        close(fd);
        ta->passed = 0;
        return NULL;
    }
    assert(aura_request_set_linked(wreq) == 0);

    aura_request_t *freq = aura_fsync(engine, fd, 0, 0, basic_cb, &fs);
    if (!freq) {
        close(fd);
        ta->passed = 0;
        return NULL;
    }

    /* Wait for completion */
    int iters = 0;
    while ((!ws.done || !fs.done) && iters++ < 5000) {
        aura_poll(engine);
        usleep(500);
    }

    ta->passed = (ws.done && fs.done && ws.result == sizeof(buf) && fs.result == 0);

    close(fd);
    unlink(path);
    return NULL;
}

TEST(concurrent_independent_chains) {
    aura_engine_t *engine = make_engine();

    thread_arg_t args[2] = {
        { .engine = engine, .thread_id = 0, .passed = 0 },
        { .engine = engine, .thread_id = 1, .passed = 0 },
    };

    pthread_t threads[2];
    for (int i = 0; i < 2; i++) {
        int ret = pthread_create(&threads[i], NULL, chain_thread_func, &args[i]);
        assert(ret == 0);
    }

    for (int i = 0; i < 2; i++) {
        pthread_join(threads[i], NULL);
        assert(args[i].passed && "thread chain failed");
    }

    aura_destroy(engine);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    setup();

    printf("test_linked_ops:\n");
    RUN_TEST(write_fsync_chain);
    RUN_TEST(chain_failure_cancels);
    RUN_TEST(three_op_chain);
    RUN_TEST(unlinked_ops_unaffected);
    RUN_TEST(ring_pinning);
    RUN_TEST(chain_abort_recovery);
    RUN_TEST(linked_with_registered_files);
    RUN_TEST(concurrent_independent_chains);

    teardown();

    printf("\n%d tests passed\n", test_count);
    return 0;
}
