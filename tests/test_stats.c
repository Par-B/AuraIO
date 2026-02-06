/**
 * @file test_stats.c
 * @brief Unit tests for enhanced statistics API
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>

#include "../include/auraio.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-45s", #name); \
    test_##name(); \
    printf(" OK\n"); \
    test_count++; \
} while(0)

/* ============================================================================
 * Helpers for I/O tests
 * ============================================================================ */

static char test_file[256];
static int test_fd = -1;

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

static void test_callback(auraio_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    (void)result;
    callback_called = 1;
}

/* ============================================================================
 * Ring Count Tests
 * ============================================================================ */

TEST(ring_count_null) {
    assert(auraio_get_ring_count(NULL) == 0);
}

TEST(ring_count_valid) {
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 2;
    opts.queue_depth = 32;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    int count = auraio_get_ring_count(engine);
    assert(count == 2);

    auraio_destroy(engine);
}

TEST(ring_count_auto) {
    auraio_engine_t *engine = auraio_create();
    assert(engine != NULL);

    int count = auraio_get_ring_count(engine);
    assert(count > 0);

    auraio_destroy(engine);
}

/* ============================================================================
 * Ring Stats Tests
 * ============================================================================ */

TEST(ring_stats_basic) {
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    auraio_ring_stats_t rs;
    int rc = auraio_get_ring_stats(engine, 0, &rs);
    assert(rc == 0);

    assert(rs.queue_depth == 64);
    assert(rs.aimd_phase >= 0 && rs.aimd_phase <= AURAIO_PHASE_CONVERGED);
    assert(rs.in_flight_limit > 0);
    assert(rs.pending_count == 0);

    auraio_destroy(engine);
}

TEST(ring_stats_null_engine) {
    auraio_ring_stats_t rs;
    memset(&rs, 0xFF, sizeof(rs));
    int rc = auraio_get_ring_stats(NULL, 0, &rs);
    assert(rc == -1);
    /* Struct should be unchanged when engine is NULL */
    assert(rs.queue_depth == (int)0xFFFFFFFF);
}

TEST(ring_stats_null_output) {
    auraio_engine_t *engine = auraio_create();
    assert(engine != NULL);
    int rc = auraio_get_ring_stats(engine, 0, NULL);
    assert(rc == -1);
    auraio_destroy(engine);
}

TEST(ring_stats_out_of_range) {
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    auraio_ring_stats_t rs;

    /* Negative index */
    int rc = auraio_get_ring_stats(engine, -1, &rs);
    assert(rc == -1);
    assert(rs.queue_depth == 0);
    assert(rs.ops_completed == 0);

    /* One past end (classic off-by-one boundary) */
    rc = auraio_get_ring_stats(engine, 1, &rs);
    assert(rc == -1);
    assert(rs.queue_depth == 0);

    /* Far out of range */
    rc = auraio_get_ring_stats(engine, 999, &rs);
    assert(rc == -1);
    assert(rs.queue_depth == 0);
    assert(rs.ops_completed == 0);

    auraio_destroy(engine);
}

/* ============================================================================
 * Histogram Tests
 * ============================================================================ */

TEST(histogram_basic) {
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    auraio_histogram_t hist;
    int rc = auraio_get_histogram(engine, 0, &hist);
    assert(rc == 0);

    assert(hist.bucket_width_us == AURAIO_HISTOGRAM_BUCKET_WIDTH_US);
    assert(hist.max_tracked_us == 10000);

    auraio_destroy(engine);
}

TEST(histogram_null_engine) {
    auraio_histogram_t hist;
    memset(&hist, 0xFF, sizeof(hist));
    int rc = auraio_get_histogram(NULL, 0, &hist);
    assert(rc == -1);
    /* Struct should be unchanged when engine is NULL */
    assert(hist.bucket_width_us == (int)0xFFFFFFFF);
}

TEST(histogram_null_output) {
    auraio_engine_t *engine = auraio_create();
    assert(engine != NULL);
    int rc = auraio_get_histogram(engine, 0, NULL);
    assert(rc == -1);
    auraio_destroy(engine);
}

TEST(histogram_out_of_range) {
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    auraio_histogram_t hist;

    /* Negative index */
    int rc = auraio_get_histogram(engine, -1, &hist);
    assert(rc == -1);
    assert(hist.bucket_width_us == 0);
    assert(hist.total_count == 0);

    /* One past end */
    rc = auraio_get_histogram(engine, 1, &hist);
    assert(rc == -1);
    assert(hist.bucket_width_us == 0);

    /* Far out of range */
    rc = auraio_get_histogram(engine, 999, &hist);
    assert(rc == -1);
    assert(hist.bucket_width_us == 0);
    assert(hist.total_count == 0);

    auraio_destroy(engine);
}

/* ============================================================================
 * Buffer Stats Tests
 * ============================================================================ */

TEST(buffer_stats_basic) {
    auraio_engine_t *engine = auraio_create();
    assert(engine != NULL);

    auraio_buffer_stats_t bs;
    int rc = auraio_get_buffer_stats(engine, &bs);
    assert(rc == 0);
    assert(bs.shard_count > 0);

    auraio_destroy(engine);
}

TEST(buffer_stats_null) {
    int rc = auraio_get_buffer_stats(NULL, NULL);
    assert(rc == -1);

    auraio_buffer_stats_t bs;
    memset(&bs, 0xFF, sizeof(bs));
    rc = auraio_get_buffer_stats(NULL, &bs);
    assert(rc == -1);
    /* Struct unchanged when engine is NULL */
    assert(bs.shard_count == (int)0xFFFFFFFF);
}

TEST(buffer_stats_null_output) {
    auraio_engine_t *engine = auraio_create();
    assert(engine != NULL);
    int rc = auraio_get_buffer_stats(engine, NULL);
    assert(rc == -1);
    auraio_destroy(engine);
}

TEST(buffer_stats_after_alloc) {
    auraio_engine_t *engine = auraio_create();
    assert(engine != NULL);

    /* Allocate some buffers */
    void *buf1 = auraio_buffer_alloc(engine, 4096);
    void *buf2 = auraio_buffer_alloc(engine, 8192);

    auraio_buffer_stats_t bs;
    auraio_get_buffer_stats(engine, &bs);

    assert(bs.total_allocated_bytes >= 12288);
    assert(bs.total_buffers >= 2);

    /* Free buffers — pool may cache them, so allocated_bytes may not decrease.
     * Verify stats are still valid (non-negative, no increase). */
    auraio_buffer_free(engine, buf1, 4096);
    auraio_buffer_free(engine, buf2, 8192);

    auraio_buffer_stats_t bs2;
    auraio_get_buffer_stats(engine, &bs2);
    assert(bs2.total_allocated_bytes <= bs.total_allocated_bytes);
    assert(bs2.total_buffers <= bs.total_buffers);

    auraio_destroy(engine);
}

/* ============================================================================
 * Phase Name Tests
 * ============================================================================ */

TEST(phase_name_valid) {
    assert(strcmp(auraio_phase_name(AURAIO_PHASE_BASELINE), "BASELINE") == 0);
    assert(strcmp(auraio_phase_name(AURAIO_PHASE_PROBING), "PROBING") == 0);
    assert(strcmp(auraio_phase_name(AURAIO_PHASE_STEADY), "STEADY") == 0);
    assert(strcmp(auraio_phase_name(AURAIO_PHASE_BACKOFF), "BACKOFF") == 0);
    assert(strcmp(auraio_phase_name(AURAIO_PHASE_SETTLING), "SETTLING") == 0);
    assert(strcmp(auraio_phase_name(AURAIO_PHASE_CONVERGED), "CONVERGED") == 0);
}

TEST(phase_name_invalid) {
    const char *name;
    name = auraio_phase_name(-1);
    assert(name != NULL);
    assert(strcmp(name, "UNKNOWN") == 0);
    name = auraio_phase_name(6);
    assert(name != NULL);
    assert(strcmp(name, "UNKNOWN") == 0);
    name = auraio_phase_name(999);
    assert(name != NULL);
    assert(strcmp(name, "UNKNOWN") == 0);
}

TEST(phase_constants_match) {
    /* Verify public constants match the values returned by phase_name */
    assert(AURAIO_PHASE_BASELINE == 0);
    assert(AURAIO_PHASE_PROBING == 1);
    assert(AURAIO_PHASE_STEADY == 2);
    assert(AURAIO_PHASE_BACKOFF == 3);
    assert(AURAIO_PHASE_SETTLING == 4);
    assert(AURAIO_PHASE_CONVERGED == 5);
}

/* ============================================================================
 * Aggregate Stats Tests
 * ============================================================================ */

TEST(aggregate_stats_null) {
    auraio_stats_t stats;
    memset(&stats, 0xFF, sizeof(stats));
    auraio_get_stats(NULL, &stats);
    /* Should not crash — struct unchanged since engine is NULL */
    assert(stats.ops_completed == (int64_t)0xFFFFFFFFFFFFFFFFLL);

    auraio_get_stats(NULL, NULL);
    /* Should not crash */
}

TEST(aggregate_stats_sanity) {
    io_setup();

    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit a single I/O */
    void *buf = auraio_buffer_alloc(engine, 4096);
    callback_called = 0;
    auraio_request_t *req = auraio_read(engine, test_fd, auraio_buf(buf),
                                         4096, 0, test_callback, NULL);
    assert(req != NULL);
    auraio_wait(engine, 1000);
    assert(callback_called == 1);

    auraio_stats_t stats;
    auraio_get_stats(engine, &stats);

    assert(stats.ops_completed >= 1);
    assert(stats.bytes_transferred >= 4096);
    assert(stats.optimal_in_flight > 0);
    assert(stats.optimal_batch_size >= 0);
    assert(stats.p99_latency_ms >= 0.0);
    assert(stats.current_throughput_bps >= 0.0);

    auraio_buffer_free(engine, buf, 4096);
    auraio_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * I/O Verification Tests — stats must reflect actual I/O
 * ============================================================================ */

TEST(ring_stats_after_io) {
    io_setup();

    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    /* Stats should be zero before any I/O */
    auraio_ring_stats_t rs;
    auraio_get_ring_stats(engine, 0, &rs);
    assert(rs.ops_completed == 0);
    assert(rs.bytes_transferred == 0);

    /* Submit and complete a read */
    void *buf = auraio_buffer_alloc(engine, 4096);
    callback_called = 0;
    auraio_request_t *req = auraio_read(engine, test_fd, auraio_buf(buf),
                                         4096, 0, test_callback, NULL);
    assert(req != NULL);
    auraio_wait(engine, 1000);
    assert(callback_called == 1);

    /* Ring stats should now reflect the completed operation */
    auraio_get_ring_stats(engine, 0, &rs);
    assert(rs.ops_completed >= 1);
    assert(rs.bytes_transferred >= 4096);

    auraio_buffer_free(engine, buf, 4096);
    auraio_destroy(engine);
    io_teardown();
}

TEST(histogram_after_io) {
    io_setup();

    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit and complete multiple reads to populate histogram */
    void *buf = auraio_buffer_alloc(engine, 4096);
    for (int i = 0; i < 16; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        auraio_request_t *req = auraio_read(engine, test_fd, auraio_buf(buf),
                                             4096, 0, test_callback, NULL);
        assert(req != NULL);
        auraio_wait(engine, 1000);
        assert(callback_called == 1);
    }

    /* Histogram should have recorded some samples */
    auraio_histogram_t hist;
    auraio_get_histogram(engine, 0, &hist);

    assert(hist.bucket_width_us == AURAIO_HISTOGRAM_BUCKET_WIDTH_US);
    assert(hist.max_tracked_us == 10000);

    /* With 16 ops, at least some samples should have been recorded.
     * The sampling rate is every 8th op, so we expect >= 1 sample. */
    assert(hist.total_count >= 1);

    /* Verify bucket sum consistency: sum of buckets + overflow should
     * approximately equal total_count (may differ slightly due to
     * concurrent writes — see histogram snapshot docs). */
    uint32_t bucket_sum = 0;
    for (int b = 0; b < AURAIO_HISTOGRAM_BUCKETS; b++) {
        bucket_sum += hist.buckets[b];
    }
    bucket_sum += hist.overflow;
    /* Allow small discrepancy due to approximate snapshot */
    assert(bucket_sum >= hist.total_count - 1);

    auraio_buffer_free(engine, buf, 4096);
    auraio_destroy(engine);
    io_teardown();
}

TEST(aggregate_stats_match_ring_stats) {
    io_setup();

    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 2;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    /* Submit some I/O */
    void *buf = auraio_buffer_alloc(engine, 4096);
    for (int i = 0; i < 4; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        auraio_request_t *req = auraio_read(engine, test_fd, auraio_buf(buf),
                                             4096, 0, test_callback, NULL);
        assert(req != NULL);
        auraio_wait(engine, 1000);
        assert(callback_called == 1);
    }

    /* Sum per-ring ops and bytes should match aggregate */
    auraio_stats_t agg;
    auraio_get_stats(engine, &agg);

    int64_t total_ops = 0;
    int64_t total_bytes = 0;
    int rings = auraio_get_ring_count(engine);
    for (int i = 0; i < rings; i++) {
        auraio_ring_stats_t rs;
        auraio_get_ring_stats(engine, i, &rs);
        total_ops += rs.ops_completed;
        total_bytes += rs.bytes_transferred;
    }

    assert(total_ops == agg.ops_completed);
    assert(total_bytes == agg.bytes_transferred);

    auraio_buffer_free(engine, buf, 4096);
    auraio_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Concurrency Test — stats readers vs active I/O
 * ============================================================================ */

struct stats_reader_ctx {
    auraio_engine_t *engine;
    _Atomic int stop;
    _Atomic int reads_done;
};

static void *stats_reader_thread(void *arg) {
    struct stats_reader_ctx *ctx = arg;
    int rings = auraio_get_ring_count(ctx->engine);

    while (!atomic_load(&ctx->stop)) {
        /* Exercise all stats functions under concurrent I/O */
        for (int i = 0; i < rings; i++) {
            auraio_ring_stats_t rs;
            auraio_get_ring_stats(ctx->engine, i, &rs);
            assert(rs.queue_depth > 0);
            assert(rs.aimd_phase >= 0 && rs.aimd_phase <= AURAIO_PHASE_CONVERGED);

            auraio_histogram_t hist;
            auraio_get_histogram(ctx->engine, i, &hist);
            assert(hist.bucket_width_us == AURAIO_HISTOGRAM_BUCKET_WIDTH_US);
        }

        auraio_buffer_stats_t bs;
        auraio_get_buffer_stats(ctx->engine, &bs);
        assert(bs.shard_count > 0);

        atomic_fetch_add(&ctx->reads_done, 1);
    }
    return NULL;
}

TEST(concurrent_stats_and_io) {
    io_setup();

    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 2;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    assert(engine != NULL);

    /* Start stats reader thread */
    struct stats_reader_ctx ctx = {
        .engine = engine,
        .stop = ATOMIC_VAR_INIT(0),
        .reads_done = ATOMIC_VAR_INIT(0)
    };
    pthread_t reader;
    int rc = pthread_create(&reader, NULL, stats_reader_thread, &ctx);
    assert(rc == 0);

    /* Give reader thread time to start */
    usleep(1000);

    /* Submit I/O while stats reader is running */
    void *buf = auraio_buffer_alloc(engine, 4096);
    for (int i = 0; i < 64; i++) {
        callback_called = 0;
        lseek(test_fd, 0, SEEK_SET);
        auraio_request_t *req = auraio_read(engine, test_fd, auraio_buf(buf),
                                             4096, 0, test_callback, NULL);
        assert(req != NULL);
        auraio_wait(engine, 1000);
        assert(callback_called == 1);
        if (i % 8 == 0) usleep(100);  /* Yield to reader thread */
    }

    /* Stop reader and verify it ran */
    atomic_store(&ctx.stop, 1);
    pthread_join(reader, NULL);
    assert(atomic_load(&ctx.reads_done) > 0);

    auraio_buffer_free(engine, buf, 4096);
    auraio_destroy(engine);
    io_teardown();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
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

    printf("\n  All %d tests passed!\n\n", test_count);
    return 0;
}
