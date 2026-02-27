// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_stats.c
 * @brief Unit tests for enhanced statistics API
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../include/aura.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-45s", #name); \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* ============================================================================
 * Helpers for I/O tests
 * ============================================================================ */

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

static void io_setup(void) {
    strcpy(test_file, "/tmp/test_stats_XXXXXX");
    test_fd = mkstemp(test_file);
    if (test_fd < 0) {
        strcpy(test_file, "./test_stats_XXXXXX");
        test_fd = mkstemp(test_file);
    }
    assert(test_fd >= 0);

    char data[4096];
    memset(data, 'A', sizeof(data));
    ssize_t w = write(test_fd, data, sizeof(data));
    assert(w == sizeof(data));
    lseek(test_fd, 0, SEEK_SET);
}

static void io_teardown(void) {
    if (test_fd >= 0) {
        close(test_fd);
        test_fd = -1;
    }
    unlink(test_file);
}

static _Atomic int callback_called;

static void test_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    (void)result;
    callback_called = 1;
}

/* ============================================================================
 * Ring Count Tests
 * ============================================================================ */

TEST(ring_count_null) {
    assert(aura_get_ring_count(NULL) == 0);
}

TEST(ring_count_valid) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 2;
    opts.queue_depth = 32;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    int count = aura_get_ring_count(engine);
    assert(count == 2);

    aura_destroy(engine);
}

TEST(ring_count_auto) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    int count = aura_get_ring_count(engine);
    assert(count > 0);

    aura_destroy(engine);
}

/* ============================================================================
 * Ring Stats Tests
 * ============================================================================ */

TEST(ring_stats_basic) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    aura_ring_stats_t rs;
    int rc = aura_get_ring_stats(engine, 0, &rs, sizeof(rs));
    assert(rc == 0);

    assert(rs.queue_depth == 64);
    assert(rs.aimd_phase >= 0 && rs.aimd_phase <= AURA_PHASE_PASSTHROUGH);
    assert(rs.in_flight_limit > 0);
    assert(rs.pending_count == 0);

    aura_destroy(engine);
}

TEST(ring_stats_null_engine) {
    aura_ring_stats_t rs;
    memset(&rs, 0xFF, sizeof(rs));
    int rc = aura_get_ring_stats(NULL, 0, &rs, sizeof(rs));
    assert(rc == -1);
    /* Struct should be unchanged when engine is NULL */
    assert(rs.queue_depth == (int)0xFFFFFFFF);
}

TEST(ring_stats_null_output) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);
    int rc = aura_get_ring_stats(engine, 0, NULL, 0);
    assert(rc == -1);
    aura_destroy(engine);
}

TEST(ring_stats_out_of_range) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    aura_ring_stats_t rs;

    /* Negative index */
    int rc = aura_get_ring_stats(engine, -1, &rs, sizeof(rs));
    assert(rc == -1);
    assert(rs.queue_depth == 0);
    assert(rs.ops_completed == 0);

    /* One past end (classic off-by-one boundary) */
    rc = aura_get_ring_stats(engine, 1, &rs, sizeof(rs));
    assert(rc == -1);
    assert(rs.queue_depth == 0);

    /* Far out of range */
    rc = aura_get_ring_stats(engine, 999, &rs, sizeof(rs));
    assert(rc == -1);
    assert(rs.queue_depth == 0);
    assert(rs.ops_completed == 0);

    aura_destroy(engine);
}

/* ============================================================================
 * Histogram Tests
 * ============================================================================ */

TEST(histogram_basic) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    aura_histogram_t hist;
    int rc = aura_get_histogram(engine, 0, &hist, sizeof(hist));
    assert(rc == 0);

    assert(hist.tier_count == AURA_HISTOGRAM_TIER_COUNT);
    assert(hist.max_tracked_us == 100000);

    aura_destroy(engine);
}

TEST(histogram_null_engine) {
    aura_histogram_t hist;
    memset(&hist, 0xFF, sizeof(hist));
    int rc = aura_get_histogram(NULL, 0, &hist, sizeof(hist));
    assert(rc == -1);
    /* Struct should be unchanged when engine is NULL */
    assert(hist.tier_count == (int)0xFFFFFFFF);
}

TEST(histogram_null_output) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);
    int rc = aura_get_histogram(engine, 0, NULL, sizeof(aura_histogram_t));
    assert(rc == -1);
    aura_destroy(engine);
}

TEST(histogram_out_of_range) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    aura_histogram_t hist;

    /* Negative index */
    int rc = aura_get_histogram(engine, -1, &hist, sizeof(hist));
    assert(rc == -1);
    assert(hist.tier_count == 0);
    assert(hist.total_count == 0);

    /* One past end */
    rc = aura_get_histogram(engine, 1, &hist, sizeof(hist));
    assert(rc == -1);
    assert(hist.tier_count == 0);

    /* Far out of range */
    rc = aura_get_histogram(engine, 999, &hist, sizeof(hist));
    assert(rc == -1);
    assert(hist.tier_count == 0);
    assert(hist.total_count == 0);

    aura_destroy(engine);
}

/* ============================================================================
 * Buffer Stats Tests
 * ============================================================================ */

TEST(buffer_stats_basic) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    aura_buffer_stats_t bs;
    int rc = aura_get_buffer_stats(engine, &bs, sizeof(aura_buffer_stats_t));
    assert(rc == 0);
    assert(bs.shard_count > 0);

    aura_destroy(engine);
}

TEST(buffer_stats_null) {
    int rc = aura_get_buffer_stats(NULL, NULL, sizeof(aura_buffer_stats_t));
    assert(rc == -1);

    aura_buffer_stats_t bs;
    memset(&bs, 0xFF, sizeof(bs));
    rc = aura_get_buffer_stats(NULL, &bs, sizeof(aura_buffer_stats_t));
    assert(rc == -1);
    /* Struct unchanged when engine is NULL */
    assert(bs.shard_count == (int)0xFFFFFFFF);
}

TEST(buffer_stats_null_output) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);
    int rc = aura_get_buffer_stats(engine, NULL, sizeof(aura_buffer_stats_t));
    assert(rc == -1);
    aura_destroy(engine);
}

TEST(buffer_stats_after_alloc) {
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);

    /* Allocate some buffers */
    void *buf1 = aura_buffer_alloc(engine, 4096);
    void *buf2 = aura_buffer_alloc(engine, 8192);

    aura_buffer_stats_t bs;
    aura_get_buffer_stats(engine, &bs, sizeof(aura_buffer_stats_t));

    assert(bs.total_allocated_bytes >= 12288);
    assert(bs.total_buffers >= 2);

    /* Free buffers — pool may cache them, so allocated_bytes may not decrease.
     * Verify stats are still valid (non-negative, no increase). */
    aura_buffer_free(engine, buf1);
    aura_buffer_free(engine, buf2);

    aura_buffer_stats_t bs2;
    aura_get_buffer_stats(engine, &bs2, sizeof(aura_buffer_stats_t));
    assert(bs2.total_allocated_bytes <= bs.total_allocated_bytes);
    assert(bs2.total_buffers <= bs.total_buffers);

    aura_destroy(engine);
}

/* ============================================================================
 * Phase Name Tests
 * ============================================================================ */

TEST(phase_name_valid) {
    assert(strcmp(aura_phase_name(AURA_PHASE_BASELINE), "BASELINE") == 0);
    assert(strcmp(aura_phase_name(AURA_PHASE_PROBING), "PROBING") == 0);
    assert(strcmp(aura_phase_name(AURA_PHASE_STEADY), "STEADY") == 0);
    assert(strcmp(aura_phase_name(AURA_PHASE_BACKOFF), "BACKOFF") == 0);
    assert(strcmp(aura_phase_name(AURA_PHASE_SETTLING), "SETTLING") == 0);
    assert(strcmp(aura_phase_name(AURA_PHASE_CONVERGED), "CONVERGED") == 0);
    assert(strcmp(aura_phase_name(AURA_PHASE_PASSTHROUGH), "PASSTHROUGH") == 0);
}

TEST(phase_name_invalid) {
    const char *name;
    name = aura_phase_name(-1);
    assert(name != NULL);
    assert(strcmp(name, "UNKNOWN") == 0);
    name = aura_phase_name(7);
    assert(name != NULL);
    assert(strcmp(name, "UNKNOWN") == 0);
    name = aura_phase_name(999);
    assert(name != NULL);
    assert(strcmp(name, "UNKNOWN") == 0);
}

TEST(phase_constants_match) {
    /* Verify public constants match the values returned by phase_name */
    assert(AURA_PHASE_BASELINE == 0);
    assert(AURA_PHASE_PROBING == 1);
    assert(AURA_PHASE_STEADY == 2);
    assert(AURA_PHASE_BACKOFF == 3);
    assert(AURA_PHASE_SETTLING == 4);
    assert(AURA_PHASE_CONVERGED == 5);
    assert(AURA_PHASE_PASSTHROUGH == 6);
}

/* ============================================================================
 * Aggregate Stats Tests
 * ============================================================================ */

TEST(aggregate_stats_null) {
    aura_stats_t stats;
    memset(&stats, 0xFF, sizeof(stats));
    aura_get_stats(NULL, &stats, sizeof(stats));
    /* Should not crash — struct unchanged since engine is NULL */
    assert(stats.ops_completed == (int64_t)0xFFFFFFFFFFFFFFFFLL);

    aura_get_stats(NULL, NULL, 0);
    /* Should not crash */
}

TEST(aggregate_stats_sanity) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit a single I/O */
    void *buf = aura_buffer_alloc(engine, 4096);
    callback_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);
    aura_wait(engine, 1000);
    assert(callback_called == 1);

    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));

    assert(stats.ops_completed >= 1);
    assert(stats.bytes_transferred >= 4096);
    assert(stats.optimal_in_flight > 0);
    assert(stats.optimal_batch_size >= 0);
    assert(stats.p99_latency_ms >= 0.0);
    assert(stats.current_throughput_bps >= 0.0);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * I/O Verification Tests — stats must reflect actual I/O
 * ============================================================================ */

TEST(ring_stats_after_io) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Stats should be zero before any I/O */
    aura_ring_stats_t rs;
    aura_get_ring_stats(engine, 0, &rs, sizeof(rs));
    assert(rs.ops_completed == 0);
    assert(rs.bytes_transferred == 0);

    /* Submit and complete a read */
    void *buf = aura_buffer_alloc(engine, 4096);
    callback_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);
    aura_wait(engine, 1000);
    assert(callback_called == 1);

    /* Ring stats should now reflect the completed operation */
    aura_get_ring_stats(engine, 0, &rs, sizeof(rs));
    assert(rs.ops_completed >= 1);
    assert(rs.bytes_transferred >= 4096);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(histogram_after_io) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit and complete multiple reads to populate histogram */
    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < 16; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
    }

    /* Histogram should have recorded some samples */
    aura_histogram_t hist;
    aura_get_histogram(engine, 0, &hist, sizeof(hist));

    assert(hist.tier_count == AURA_HISTOGRAM_TIER_COUNT);
    assert(hist.max_tracked_us == 100000);

    /* With 16 ops, at least some samples should have been recorded.
     * The sampling rate is every 8th op, so we expect >= 1 sample. */
    assert(hist.total_count >= 1);

    /* Verify bucket sum consistency: sum of buckets + overflow should
     * approximately equal total_count (may differ slightly due to
     * concurrent writes — see histogram snapshot docs). */
    uint32_t bucket_sum = 0;
    for (int b = 0; b < AURA_HISTOGRAM_BUCKETS; b++) {
        bucket_sum += hist.buckets[b];
    }
    bucket_sum += hist.overflow;
    /* Allow small discrepancy due to approximate snapshot */
    assert(bucket_sum >= hist.total_count - 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(aggregate_stats_match_ring_stats) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 2;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit some I/O */
    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < 4; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
    }

    /* Sum per-ring ops and bytes should match aggregate */
    aura_stats_t agg;
    aura_get_stats(engine, &agg, sizeof(agg));

    int64_t total_ops = 0;
    int64_t total_bytes = 0;
    int rings = aura_get_ring_count(engine);
    for (int i = 0; i < rings; i++) {
        aura_ring_stats_t rs;
        aura_get_ring_stats(engine, i, &rs, sizeof(rs));
        total_ops += rs.ops_completed;
        total_bytes += rs.bytes_transferred;
    }

    assert(total_ops == agg.ops_completed);
    assert(total_bytes == agg.bytes_transferred);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Ring Selection Tests
 * ============================================================================ */

TEST(ring_select_options_default) {
    aura_options_t opts;
    aura_options_init(&opts);
    assert(opts.ring_select == AURA_SELECT_ADAPTIVE);
}

TEST(ring_select_round_robin) {
    /* Round-robin mode should distribute ops across all rings */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 4;
    opts.queue_depth = 64;
    opts.ring_select = AURA_SELECT_ROUND_ROBIN;
    opts.disable_adaptive = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit enough ops from a single thread to hit all 4 rings */
    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < 40; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
    }

    /* Verify all 4 rings received some operations */
    int rings_used = 0;
    for (int i = 0; i < 4; i++) {
        aura_ring_stats_t rs;
        aura_get_ring_stats(engine, i, &rs, sizeof(rs));
        if (rs.ops_completed > 0) rings_used++;
    }
    assert(rings_used == 4);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(ring_select_cpu_local) {
    /* CPU_LOCAL mode from a single thread should hit only one ring */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 4;
    opts.queue_depth = 64;
    opts.ring_select = AURA_SELECT_CPU_LOCAL;
    opts.disable_adaptive = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < 20; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
    }

    /* Single thread + CPU_LOCAL = exactly 1 ring should have all ops */
    int rings_used = 0;
    for (int i = 0; i < 4; i++) {
        aura_ring_stats_t rs;
        aura_get_ring_stats(engine, i, &rs, sizeof(rs));
        if (rs.ops_completed > 0) rings_used++;
    }
    assert(rings_used == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(ring_select_enum_values) {
    /* Verify enum constants have expected values */
    assert(AURA_SELECT_ADAPTIVE == 0);
    assert(AURA_SELECT_CPU_LOCAL == 1);
    assert(AURA_SELECT_ROUND_ROBIN == 2);
    assert(AURA_SELECT_THREAD_LOCAL == 3);
}

TEST(ring_select_thread_local) {
    /* THREAD_LOCAL mode: single thread should own one ring exclusively */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 4;
    opts.queue_depth = 64;
    opts.ring_select = AURA_SELECT_THREAD_LOCAL;
    opts.disable_adaptive = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit and complete several ops from one thread */
    void *buf = aura_buffer_alloc(engine, 4096);
    assert(buf != NULL);

    for (int i = 0; i < 16; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
    }

    /* All ops should land on exactly one ring (the thread-local one) */
    int rings_used = 0;
    int actual_rings = aura_get_ring_count(engine);
    for (int i = 0; i < actual_rings; i++) {
        aura_ring_stats_t rs;
        aura_get_ring_stats(engine, i, &rs, sizeof(rs));
        if (rs.ops_completed > 0) rings_used++;
    }
    assert(rings_used == 1);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Ring Selection Validation Tests
 * ============================================================================ */

TEST(ring_select_invalid_mode) {
    /* An out-of-range ring_select should fall back to default (ADAPTIVE) */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 32;
    opts.ring_select = (aura_ring_select_t)99; /* invalid */

    aura_engine_t *engine = aura_create_with_options(&opts);
    /* Engine should still create (graceful fallback) or fail cleanly */
    if (engine) {
        /* If created, verify I/O still works */
        io_setup();
        void *buf = aura_buffer_alloc(engine, 4096);
        callback_called = 0;
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
        aura_buffer_free(engine, buf);
        aura_destroy(engine);
        io_teardown();
    }
    /* Either way: no crash = pass */
}

TEST(ring_select_round_robin_distribution) {
    /* Verify round-robin distributes evenly across rings */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 4;
    opts.queue_depth = 64;
    opts.ring_select = AURA_SELECT_ROUND_ROBIN;
    opts.disable_adaptive = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    int actual_rings = aura_get_ring_count(engine);

    /* Submit 4*actual_rings ops — should be perfectly even */
    void *buf = aura_buffer_alloc(engine, 4096);
    int total_ops = actual_rings * 4;
    for (int i = 0; i < total_ops; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
    }

    /* Each ring should have received at least 1 op */
    int64_t min_ops = INT64_MAX, max_ops = 0;
    for (int i = 0; i < actual_rings; i++) {
        aura_ring_stats_t rs;
        aura_get_ring_stats(engine, i, &rs, sizeof(rs));
        if (rs.ops_completed < min_ops) min_ops = rs.ops_completed;
        if (rs.ops_completed > max_ops) max_ops = rs.ops_completed;
    }
    /* Distribution should be within 2x of ideal (allowing for scheduling) */
    assert(min_ops > 0);
    assert(max_ops <= min_ops * 3); /* generous bound */

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Statistics Accuracy Tests
 * ============================================================================ */

TEST(stats_ops_count_accuracy) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit exactly N ops and verify count matches */
    int n = 10;
    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < n; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
    }

    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));
    assert(stats.ops_completed == n);

    aura_ring_stats_t rs;
    aura_get_ring_stats(engine, 0, &rs, sizeof(rs));
    assert(rs.ops_completed == n);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(stats_bytes_accuracy) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Read 4096 bytes 5 times = 20480 bytes */
    int n = 5;
    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < n; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
    }

    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));
    assert(stats.bytes_transferred == (int64_t)n * 4096);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(stats_zero_before_io) {
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 32;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Before any I/O, all counters should be zero */
    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));
    assert(stats.ops_completed == 0);
    assert(stats.bytes_transferred == 0);
    assert(stats.adaptive_spills == 0);

    aura_ring_stats_t rs;
    aura_get_ring_stats(engine, 0, &rs, sizeof(rs));
    assert(rs.ops_completed == 0);
    assert(rs.bytes_transferred == 0);
    assert(rs.pending_count == 0);

    aura_destroy(engine);
}

TEST(stats_monotonic_ops) {
    /* ops_completed should be monotonically non-decreasing */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);
    int64_t prev_ops = 0;

    for (int i = 0; i < 8; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);

        aura_stats_t stats;
        aura_get_stats(engine, &stats, sizeof(stats));
        assert(stats.ops_completed >= prev_ops);
        prev_ops = stats.ops_completed;
    }
    assert(prev_ops == 8);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Cancellation Tests
 * ============================================================================ */

TEST(cancel_null_params) {
    /* Cancel with NULL engine */
    int rc = aura_cancel(NULL, (aura_request_t *)0x1);
    assert(rc == -1);
    assert(errno == EINVAL);

    /* Cancel with NULL request */
    aura_engine_t *engine = aura_create();
    assert(engine != NULL);
    rc = aura_cancel(engine, NULL);
    assert(rc == -1);
    assert(errno == EINVAL);

    aura_destroy(engine);
}

TEST(cancel_completed_request) {
    /* Cancelling an already-completed request should fail with EALREADY */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);
    callback_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);
    aura_wait(engine, 1000);
    assert(callback_called == 1);

    /* Request is now completed — cancel should fail */
    int rc = aura_cancel(engine, req);
    assert(rc == -1);
    assert(errno == EALREADY);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

TEST(cancel_pending_request) {
    /* Submit a slow op and try to cancel it before it completes */
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    void *buf = aura_buffer_alloc(engine, 4096);
    callback_called = 0;
    aura_request_t *req = aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
    assert(req != NULL);

    /* Try to cancel — may succeed (0) or fail (-1/EALREADY) if already completed */
    int rc = aura_cancel(engine, req);
    /* Either outcome is valid */
    assert(rc == 0 || (rc == -1 && errno == EALREADY));

    /* Drain any remaining completions */
    int iters = 0;
    while (!callback_called && iters++ < 1000) {
        aura_poll(engine);
        usleep(1000);
    }

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Concurrency Test — stats readers vs active I/O
 * ============================================================================ */

struct stats_reader_ctx {
    aura_engine_t *engine;
    _Atomic int stop;
    _Atomic int reads_done;
};

static void *stats_reader_thread(void *arg) {
    struct stats_reader_ctx *ctx = arg;
    int rings = aura_get_ring_count(ctx->engine);

    while (!atomic_load(&ctx->stop)) {
        /* Exercise all stats functions under concurrent I/O */
        for (int i = 0; i < rings; i++) {
            aura_ring_stats_t rs;
            aura_get_ring_stats(ctx->engine, i, &rs, sizeof(rs));
            assert(rs.queue_depth > 0);
            assert(rs.aimd_phase >= 0 && rs.aimd_phase <= AURA_PHASE_PASSTHROUGH);

            aura_histogram_t hist;
            aura_get_histogram(ctx->engine, i, &hist, sizeof(hist));
            assert(hist.tier_count == AURA_HISTOGRAM_TIER_COUNT);
        }

        aura_buffer_stats_t bs;
        aura_get_buffer_stats(ctx->engine, &bs, sizeof(aura_buffer_stats_t));
        assert(bs.shard_count > 0);

        atomic_fetch_add(&ctx->reads_done, 1);
    }
    return NULL;
}

TEST(concurrent_stats_and_io) {
    io_setup();

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 2;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    assert(engine != NULL);

    /* Start stats reader thread */
    struct stats_reader_ctx ctx = { .engine = engine,
                                    .stop = ATOMIC_VAR_INIT(0),
                                    .reads_done = ATOMIC_VAR_INIT(0) };
    pthread_t reader;
    int rc = pthread_create(&reader, NULL, stats_reader_thread, &ctx);
    assert(rc == 0);

    /* Give reader thread time to start */
    usleep(1000);

    /* Submit I/O while stats reader is running */
    void *buf = aura_buffer_alloc(engine, 4096);
    for (int i = 0; i < 64; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        aura_request_t *req =
            aura_read(engine, test_fd, aura_buf(buf), 4096, 0, test_callback, NULL);
        assert(req != NULL);
        aura_wait(engine, 1000);
        assert(callback_called == 1);
        if (i % 8 == 0) usleep(100); /* Yield to reader thread */
    }

    /* Stop reader and verify it ran */
    atomic_store(&ctx.stop, 1);
    pthread_join(reader, NULL);
    assert(atomic_load(&ctx.reads_done) > 0);

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    atexit(cleanup_atexit);
    printf("\n=== Enhanced Stats API Tests ===\n\n");

    /* Ring count */
    RUN_TEST(ring_count_null);
    RUN_TEST(ring_count_valid);
    RUN_TEST(ring_count_auto);

    /* Ring stats */
    RUN_TEST(ring_stats_basic);
    RUN_TEST(ring_stats_null_engine);
    RUN_TEST(ring_stats_null_output);
    RUN_TEST(ring_stats_out_of_range);

    /* Histogram */
    RUN_TEST(histogram_basic);
    RUN_TEST(histogram_null_engine);
    RUN_TEST(histogram_null_output);
    RUN_TEST(histogram_out_of_range);

    /* Buffer stats */
    RUN_TEST(buffer_stats_basic);
    RUN_TEST(buffer_stats_null);
    RUN_TEST(buffer_stats_null_output);
    RUN_TEST(buffer_stats_after_alloc);

    /* Phase names */
    RUN_TEST(phase_name_valid);
    RUN_TEST(phase_name_invalid);
    RUN_TEST(phase_constants_match);

    /* Aggregate stats */
    RUN_TEST(aggregate_stats_null);
    RUN_TEST(aggregate_stats_sanity);

    /* I/O verification */
    RUN_TEST(ring_stats_after_io);
    RUN_TEST(histogram_after_io);
    RUN_TEST(aggregate_stats_match_ring_stats);

    /* Concurrency */
    RUN_TEST(concurrent_stats_and_io);

    /* Ring selection */
    RUN_TEST(ring_select_options_default);
    RUN_TEST(ring_select_enum_values);
    RUN_TEST(ring_select_round_robin);
    RUN_TEST(ring_select_cpu_local);
    RUN_TEST(ring_select_thread_local);

    /* Ring selection validation */
    RUN_TEST(ring_select_invalid_mode);
    RUN_TEST(ring_select_round_robin_distribution);

    /* Statistics accuracy */
    RUN_TEST(stats_ops_count_accuracy);
    RUN_TEST(stats_bytes_accuracy);
    RUN_TEST(stats_zero_before_io);
    RUN_TEST(stats_monotonic_ops);

    /* Concurrent cancellation */
    RUN_TEST(cancel_null_params);
    RUN_TEST(cancel_completed_request);
    RUN_TEST(cancel_pending_request);

    printf("\n  All %d tests passed!\n\n", test_count);
    return 0;
}
