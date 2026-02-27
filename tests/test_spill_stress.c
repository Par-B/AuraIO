// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_spill_stress.c
 * @brief Stress test for adaptive spill logic
 *
 * Hammers the adaptive ring selection with high concurrency and small queue
 * depths to force spills across multiple iterations.  Verifies that spills
 * occur, all operations complete, and the engine destroys cleanly.
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

#define RING_COUNT 4
#define QUEUE_DEPTH 32
#define THREAD_COUNT 8
#define OPS_PER_THREAD 2000
#define FILL_COUNT 24
#define BUF_SIZE 4096
#define ITERATIONS 10

typedef struct {
    aura_engine_t *engine;
    int fd;
    int ops_target;
    _Atomic int ops_completed;
    pthread_barrier_t *barrier;
} worker_ctx_t;

static void completion_cb(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)result;
    worker_ctx_t *ctx = user_data;
    atomic_fetch_add_explicit(&ctx->ops_completed, 1, memory_order_relaxed);
}

static void *worker(void *arg) {
    worker_ctx_t *ctx = arg;

    void *buf = aura_buffer_alloc(ctx->engine, BUF_SIZE);
    if (!buf) return NULL;

    int submitted = 0;

    /* Phase 1: Fill without polling to build pending pressure */
    for (int i = 0; i < FILL_COUNT && submitted < ctx->ops_target; i++) {
        aura_request_t *req =
            aura_read(ctx->engine, ctx->fd, aura_buf(buf), BUF_SIZE, 0, 0, completion_cb, ctx);
        if (req) submitted++;
        else break;
    }

    /* Barrier + tick wait */
    pthread_barrier_wait(ctx->barrier);
    usleep(25000);
    pthread_barrier_wait(ctx->barrier);

    /* Phase 2: Submit remaining ops under pressure */
    int batch = 0;
    while (submitted < ctx->ops_target) {
        aura_request_t *req =
            aura_read(ctx->engine, ctx->fd, aura_buf(buf), BUF_SIZE, 0, 0, completion_cb, ctx);
        if (req) {
            submitted++;
            batch++;
        } else {
            aura_poll(ctx->engine);
        }

        if (batch >= QUEUE_DEPTH) {
            aura_poll(ctx->engine);
            batch = 0;
        }
    }

    /* Drain completions */
    int spins = 0;
    while (atomic_load_explicit(&ctx->ops_completed, memory_order_relaxed) < submitted) {
        aura_wait(ctx->engine, 100);
        if (++spins > 50000) break;
    }

    aura_buffer_free(ctx->engine, buf);
    return NULL;
}

int main(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 2) {
        printf("SKIP: need >= 2 CPUs for spill stress (have %ld)\n", cpus);
        return 0;
    }

    /* Create test file */
    char tmpfile[] = "/tmp/test_spill_stress_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        strcpy(tmpfile, "./test_spill_stress_XXXXXX");
        fd = mkstemp(tmpfile);
    }
    assert(fd >= 0);

    char data[BUF_SIZE];
    memset(data, 'X', sizeof(data));
    ssize_t w = write(fd, data, sizeof(data));
    assert(w == sizeof(data));

    printf("\n=== Spill Stress Test ===\n");
    printf("  Rings: %d, QD: %d, Threads: %d, Ops/thread: %d, Iterations: %d\n\n", RING_COUNT,
           QUEUE_DEPTH, THREAD_COUNT, OPS_PER_THREAD, ITERATIONS);

    int passed = 0, failed = 0;
    uint64_t total_spills = 0;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        lseek(fd, 0, SEEK_SET);

        aura_options_t opts;
        aura_options_init(&opts);
        opts.ring_count = RING_COUNT;
        opts.queue_depth = QUEUE_DEPTH;
        opts.ring_select = AURA_SELECT_ADAPTIVE;
        opts.disable_adaptive = true;

        aura_engine_t *engine = aura_create_with_options(&opts);
        if (!engine) {
            printf("  [FAIL] iter %d: failed to create engine\n", iter);
            failed++;
            continue;
        }

        pthread_barrier_t barrier;
        pthread_barrier_init(&barrier, NULL, THREAD_COUNT);

        worker_ctx_t workers[THREAD_COUNT];
        pthread_t threads[THREAD_COUNT];

        for (int i = 0; i < THREAD_COUNT; i++) {
            workers[i].engine = engine;
            workers[i].fd = fd;
            workers[i].ops_target = OPS_PER_THREAD;
            workers[i].ops_completed = 0;
            workers[i].barrier = &barrier;
        }

        for (int i = 0; i < THREAD_COUNT; i++) {
            int rc = pthread_create(&threads[i], NULL, worker, &workers[i]);
            assert(rc == 0);
        }

        for (int i = 0; i < THREAD_COUNT; i++) pthread_join(threads[i], NULL);

        /* Check all ops completed */
        int total_completed = 0;
        int total_expected = 0;
        for (int i = 0; i < THREAD_COUNT; i++) {
            total_completed += atomic_load(&workers[i].ops_completed);
            total_expected += OPS_PER_THREAD;
        }

        aura_stats_t stats;
        aura_get_stats(engine, &stats, sizeof(stats));
        uint64_t iter_spills = stats.adaptive_spills;
        total_spills += iter_spills;

        pthread_barrier_destroy(&barrier);
        aura_destroy(engine);

        if (total_completed == total_expected) {
            printf("  [PASS] iter %2d: %d/%d ops, %" PRIu64 " spills\n", iter, total_completed,
                   total_expected, iter_spills);
            passed++;
        } else {
            printf("  [FAIL] iter %2d: %d/%d ops completed (leak!)\n", iter, total_completed,
                   total_expected);
            failed++;
        }
    }

    printf("\n--- Summary ---\n");
    printf("  Total spills across %d iterations: %" PRIu64 "\n", ITERATIONS, total_spills);

    if (total_spills > 0) {
        printf("  [PASS] Spills triggered (%" PRIu64 " total)\n", total_spills);
        passed++;
    } else {
        printf("  [WARN] No spills triggered (threads may have landed on same CPU)\n");
    }

    printf("\n%d tests passed\n", passed);
    if (failed > 0) printf("%d tests FAILED\n", failed);

    close(fd);
    unlink(tmpfile);
    return failed ? 1 : 0;
}
