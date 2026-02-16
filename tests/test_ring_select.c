/**
 * @file test_ring_select.c
 * @brief Test utility for ring selection modes
 *
 * Compares ADAPTIVE, CPU_LOCAL, and ROUND_ROBIN ring selection under
 * multi-threaded load to verify spill behavior and ring distribution.
 *
 * ADAPTIVE spills require "broad system pressure": multiple rings loaded
 * above 75% of in-flight limit, with no single ring being an outlier
 * (> 2x average). The tick thread must have run (~10ms) to populate
 * avg_ring_pending before spills can fire.
 *
 * To trigger spills reliably, the ADAPTIVE test uses a two-phase approach:
 *   Phase 1: All threads fill their local rings without polling (pending grows).
 *   Barrier + sleep: Wait for tick to update avg_ring_pending.
 *   Phase 2: Submit more ops — high pending + high avg → both gates pass → spills.
 *
 * Usage: ./test_ring_select [ring_count] [ops_per_thread]
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
#include <inttypes.h>
#include <time.h>

#include "../include/aura.h"

/* --- Configuration --- */
#define DEFAULT_OPS 5000
#define QUEUE_DEPTH 32 /* in_flight_limit = 8, spill threshold = 6 */
#define BUF_SIZE 4096
#define MAX_RINGS 128

/* Phase 1 fill count: enough to exceed the 75% threshold (6 out of 8).
 * Submit most of the queue depth without polling so pending stays high. */
#define FILL_COUNT 24

/* --- Per-thread context --- */
typedef struct {
    aura_engine_t *engine;
    int fd;
    int ops_target;
    _Atomic int ops_completed;
    pthread_barrier_t *barrier; /* NULL for non-ADAPTIVE modes */
} worker_ctx_t;

static void completion_cb(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)result;
    worker_ctx_t *ctx = user_data;
    atomic_fetch_add_explicit(&ctx->ops_completed, 1, memory_order_relaxed);
}

/**
 * Simple worker: submit ops with periodic polling.
 * Used for CPU_LOCAL and ROUND_ROBIN modes.
 */
static void *worker_simple(void *arg) {
    worker_ctx_t *ctx = arg;

    void *buf = aura_buffer_alloc(ctx->engine, BUF_SIZE);
    if (!buf) return NULL;

    int submitted = 0;
    int batch = 0;

    while (submitted < ctx->ops_target) {
        aura_request_t *req =
            aura_read(ctx->engine, ctx->fd, aura_buf(buf), BUF_SIZE, 0, completion_cb, ctx);
        if (req) {
            submitted++;
            batch++;
        } else {
            aura_poll(ctx->engine);
        }

        if (batch >= 16) {
            aura_poll(ctx->engine);
            batch = 0;
        }
    }

    while (atomic_load_explicit(&ctx->ops_completed, memory_order_relaxed) < submitted)
        aura_wait(ctx->engine, 100);

    aura_buffer_free(ctx->engine, buf, BUF_SIZE);
    return NULL;
}

/**
 * Two-phase worker for ADAPTIVE mode spill testing.
 *
 * Phase 1: Fill local ring without polling (pending builds to FILL_COUNT).
 * Barrier: All threads wait, then sleep 25ms for tick to update avg.
 * Phase 2: Continue submitting — now pending is high AND avg is high,
 *          so gate 1 (75% threshold) and gate 2 (outlier check) both pass.
 *          Polls only when request pool is exhausted.
 */
static void *worker_adaptive(void *arg) {
    worker_ctx_t *ctx = arg;

    void *buf = aura_buffer_alloc(ctx->engine, BUF_SIZE);
    if (!buf) return NULL;

    int submitted = 0;

    /* Phase 1: Fill without polling — pending grows to FILL_COUNT.
     * CQEs accumulate in the CQ but aren't reaped, so pending_count stays high. */
    for (int i = 0; i < FILL_COUNT && submitted < ctx->ops_target; i++) {
        aura_request_t *req =
            aura_read(ctx->engine, ctx->fd, aura_buf(buf), BUF_SIZE, 0, completion_cb, ctx);
        if (req) submitted++;
        else break; /* Pool full */
    }

    /* Barrier: wait for all threads to finish filling */
    pthread_barrier_wait(ctx->barrier);

    /* Sleep for tick thread to see all rings loaded and update avg_ring_pending.
     * Tick interval is 10ms; sleep 25ms to guarantee at least 2 ticks. */
    usleep(25000);

    /* Second barrier: ensure all threads resume Phase 2 together */
    pthread_barrier_wait(ctx->barrier);

    /* Phase 2: Submit remaining ops. At this point:
     *   - pending ≈ FILL_COUNT (un-reaped CQEs keep pending_count high)
     *   - avg ≈ FILL_COUNT (tick saw all rings loaded)
     *   - Gate 1: pending(24) >= limit*3/4 = 6 → PASS
     *   - Gate 2: pending(24) > avg(24)*2 = 48 → false → PASS → SPILL
     * Poll only when the request pool is exhausted (aura_read returns NULL). */
    int batch = 0;
    while (submitted < ctx->ops_target) {
        aura_request_t *req =
            aura_read(ctx->engine, ctx->fd, aura_buf(buf), BUF_SIZE, 0, completion_cb, ctx);
        if (req) {
            submitted++;
            batch++;
        } else {
            /* Pool exhausted — must drain some completions to free request slots */
            aura_poll(ctx->engine);
        }

        /* Poll infrequently to keep pending elevated */
        if (batch >= QUEUE_DEPTH) {
            aura_poll(ctx->engine);
            batch = 0;
        }
    }

    /* Drain */
    int spins = 0;
    while (atomic_load_explicit(&ctx->ops_completed, memory_order_relaxed) < submitted) {
        aura_wait(ctx->engine, 100);
        if (++spins > 50000) break;
    }

    aura_buffer_free(ctx->engine, buf, BUF_SIZE);
    return NULL;
}

/* --- Mode name helper --- */
static const char *mode_name(aura_ring_select_t mode) {
    switch (mode) {
    case AURA_SELECT_ADAPTIVE:
        return "ADAPTIVE";
    case AURA_SELECT_CPU_LOCAL:
        return "CPU_LOCAL";
    case AURA_SELECT_ROUND_ROBIN:
        return "ROUND_ROBIN";
    default:
        return "UNKNOWN";
    }
}

/* --- Test result --- */
typedef struct {
    const char *name;
    int ring_count;
    int thread_count;
    int ops_per_thread;
    int64_t per_ring_ops[MAX_RINGS];
    uint64_t spills;
    int rings_used;
    double elapsed_ms;
} test_result_t;

/* --- Run one mode --- */
static void run_mode(aura_ring_select_t mode, int ring_count, int ops_per_thread, int fd,
                     test_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->name = mode_name(mode);

    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = ring_count;
    opts.queue_depth = QUEUE_DEPTH;
    opts.ring_select = mode;
    opts.disable_adaptive = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "  ERROR: failed to create engine for %s\n", result->name);
        return;
    }

    int actual_rings = aura_get_ring_count(engine);
    result->ring_count = actual_rings;
    result->ops_per_thread = ops_per_thread;

    int thread_count = actual_rings;
    result->thread_count = thread_count;

    /* Barrier for ADAPTIVE mode synchronization */
    pthread_barrier_t barrier;
    int use_adaptive_worker = (mode == AURA_SELECT_ADAPTIVE);
    if (use_adaptive_worker) pthread_barrier_init(&barrier, NULL, thread_count);

    worker_ctx_t *workers = calloc(thread_count, sizeof(worker_ctx_t));
    pthread_t *threads = calloc(thread_count, sizeof(pthread_t));

    for (int i = 0; i < thread_count; i++) {
        workers[i].engine = engine;
        workers[i].fd = fd;
        workers[i].ops_target = ops_per_thread;
        workers[i].ops_completed = 0;
        workers[i].barrier = use_adaptive_worker ? &barrier : NULL;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    void *(*thread_fn)(void *) = use_adaptive_worker ? worker_adaptive : worker_simple;
    for (int i = 0; i < thread_count; i++) {
        int rc = pthread_create(&threads[i], NULL, thread_fn, &workers[i]);
        assert(rc == 0);
    }

    for (int i = 0; i < thread_count; i++) pthread_join(threads[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    result->elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

    /* Collect stats */
    aura_stats_t stats;
    aura_get_stats(engine, &stats);
    result->spills = stats.adaptive_spills;

    result->rings_used = 0;
    for (int i = 0; i < actual_rings && i < MAX_RINGS; i++) {
        aura_ring_stats_t rs;
        aura_get_ring_stats(engine, i, &rs);
        result->per_ring_ops[i] = rs.ops_completed;
        if (rs.ops_completed > 0) result->rings_used++;
    }

    if (use_adaptive_worker) pthread_barrier_destroy(&barrier);
    free(workers);
    free(threads);
    aura_destroy(engine);
}

/* --- Print results --- */
static void print_result(const test_result_t *r) {
    int64_t total_ops = 0;
    for (int i = 0; i < r->ring_count; i++) total_ops += r->per_ring_ops[i];

    printf("\n  %-12s  %d threads x %d ops = %" PRId64 " total  (%.0fms)\n", r->name,
           r->thread_count, r->ops_per_thread, total_ops, r->elapsed_ms);

    printf("    Rings [%d used / %d total]:", r->rings_used, r->ring_count);
    int show = r->ring_count < 16 ? r->ring_count : 16;
    for (int i = 0; i < show; i++) printf(" [%d]=%" PRId64, i, r->per_ring_ops[i]);
    if (r->ring_count > 16) printf(" ...");
    printf("\n");

    if (r->rings_used > 1 && total_ops > 0) {
        int64_t ideal = total_ops / r->rings_used;
        int64_t max_dev = 0;
        for (int i = 0; i < r->ring_count; i++) {
            int64_t dev = r->per_ring_ops[i] > ideal ? r->per_ring_ops[i] - ideal
                                                     : ideal - r->per_ring_ops[i];
            if (dev > max_dev) max_dev = dev;
        }
        printf("    Distribution: ideal=%" PRId64 "/ring, max deviation=%" PRId64 " (%.0f%%)\n",
               ideal, max_dev, 100.0 * max_dev / (ideal > 0 ? ideal : 1));
    }

    printf("    Spills: %" PRIu64 "\n", r->spills);
}

/* --- Main --- */
int main(int argc, char *argv[]) {
    int ring_count = 0;
    int ops_per_thread = DEFAULT_OPS;

    if (argc > 1) ring_count = atoi(argv[1]);
    if (argc > 2) ops_per_thread = atoi(argv[2]);

    long cpus = sysconf(_SC_NPROCESSORS_ONLN);

    if (ring_count == 0) ring_count = (int)(cpus < 8 ? cpus : 8);

    if (ring_count < 2) {
        printf("SKIP: need >= 2 CPUs for spill testing (have %ld)\n", cpus);
        return 0;
    }

    /* Create test file */
    char tmpfile[] = "/tmp/test_ring_select_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        strcpy(tmpfile, "./test_ring_select_XXXXXX");
        fd = mkstemp(tmpfile);
    }
    assert(fd >= 0);

    char data[BUF_SIZE];
    memset(data, 'X', sizeof(data));
    ssize_t w = write(fd, data, sizeof(data));
    assert(w == sizeof(data));
    lseek(fd, 0, SEEK_SET);

    int ifl = QUEUE_DEPTH / 4;
    int spill_at = ifl * 3 / 4;

    printf("\n=== Ring Selection Mode Test ===\n\n");
    printf("  CPUs: %ld, Rings: %d, QD: %d\n", cpus, ring_count, QUEUE_DEPTH);
    printf("  in_flight_limit: %d (fixed), spill at: %d pending (75%%)\n", ifl, spill_at);
    printf("  Fill phase: %d ops/thread (no polling), then 25ms tick wait\n", FILL_COUNT);
    printf("  Ops/thread: %d, Threads: %d (one per ring)\n", ops_per_thread, ring_count);

    test_result_t results[3];

    printf("\n--- Running CPU_LOCAL ---");
    fflush(stdout);
    run_mode(AURA_SELECT_CPU_LOCAL, ring_count, ops_per_thread, fd, &results[0]);

    printf("\n--- Running ROUND_ROBIN ---");
    fflush(stdout);
    run_mode(AURA_SELECT_ROUND_ROBIN, ring_count, ops_per_thread, fd, &results[1]);

    printf("\n--- Running ADAPTIVE ---");
    fflush(stdout);
    run_mode(AURA_SELECT_ADAPTIVE, ring_count, ops_per_thread, fd, &results[2]);

    printf("\n\n=== Results ===");
    for (int i = 0; i < 3; i++) print_result(&results[i]);

    printf("\n--- Checks ---\n\n");
    int passed = 0, failed = 0;

    if (results[0].spills == 0) {
        printf("  [PASS] CPU_LOCAL: 0 spills (expected)\n");
        passed++;
    } else {
        printf("  [FAIL] CPU_LOCAL: expected 0 spills, got %" PRIu64 "\n", results[0].spills);
        failed++;
    }

    if (results[1].spills == 0) {
        printf("  [PASS] ROUND_ROBIN: 0 spills (expected)\n");
        passed++;
    } else {
        printf("  [FAIL] ROUND_ROBIN: expected 0 spills, got %" PRIu64 "\n", results[1].spills);
        failed++;
    }

    if (results[1].rings_used == ring_count) {
        printf("  [PASS] ROUND_ROBIN: all %d rings used\n", ring_count);
        passed++;
    } else {
        printf("  [FAIL] ROUND_ROBIN: expected %d rings used, got %d\n", ring_count,
               results[1].rings_used);
        failed++;
    }

    if (results[2].spills > 0) {
        printf("  [PASS] ADAPTIVE: %" PRIu64 " spills under broad pressure\n", results[2].spills);
        passed++;
    } else if (results[2].rings_used <= 1) {
        printf("  [SKIP] ADAPTIVE: only %d ring loaded (threads on same CPU?)\n",
               results[2].rings_used);
    } else {
        printf("  [FAIL] ADAPTIVE: 0 spills with %d rings loaded\n", results[2].rings_used);
        failed++;
    }

    printf("\n%d tests passed\n", passed);
    if (failed > 0) printf("%d tests FAILED\n", failed);

    close(fd);
    unlink(tmpfile);
    return failed ? 1 : 0;
}
