// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file registered_buffers.c
 * @brief Demonstrate registered (fixed) buffers for zero-copy I/O
 *
 * Pre-registering buffers with the kernel eliminates per-operation mapping
 * overhead. Best for workloads with:
 * - Same buffers reused across 1000+ I/O operations
 * - High-frequency small I/O (< 16KB) where mapping overhead is significant
 * - Zero-copy is critical for performance
 *
 * Build: make examples
 * Run:   ./examples/C/registered_buffers
 */

#define _POSIX_C_SOURCE 199309L
#include <aura.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define TEST_FILE "/tmp/aura_reg_buf_test.dat"
#define FILE_SIZE (1 * 1024 * 1024) /* 1 MB */
#define BUF_SIZE 4096
#define NUM_BUFFERS 4
#define NUM_OPS 50

static _Atomic int completed = 0;

void completion_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    if (result < 0) {
        fprintf(stderr, "I/O error: %zd\n", result);
    }
    atomic_fetch_add_explicit(&completed, 1, memory_order_relaxed);
}

double run_benchmark(aura_engine_t *engine, int fd, int use_registered, void *unreg_bufs[]) {
    struct timespec start, end;

    completed = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Submit NUM_OPS reads using registered or unregistered buffers with proper pacing */
    int submitted = 0;
    int max_inflight = use_registered ? NUM_BUFFERS : 8;

    while (submitted < NUM_OPS || completed < NUM_OPS) {
        /* Submit new operations while under the concurrency limit */
        while (submitted < NUM_OPS && (submitted - completed) < max_inflight) {
            int buf_idx = submitted % NUM_BUFFERS;
            off_t offset = (rand() % (FILE_SIZE / BUF_SIZE)) * BUF_SIZE;

            aura_request_t *req;
            if (use_registered) {
                /* Use registered buffer by index */
                req = aura_read(engine, fd, aura_buf_fixed(buf_idx, 0), BUF_SIZE, offset, 0,
                                completion_callback, NULL);
            } else {
                /* Use pre-allocated unregistered buffer */
                req = aura_read(engine, fd, aura_buf(unreg_bufs[buf_idx]), BUF_SIZE, offset, 0,
                                completion_callback, NULL);
            }

            if (!req) {
                perror("aura_read");
                break;
            }
            submitted++;
        }

        /* Poll for completions */
        int poll_rc_ = aura_poll(engine);
        (void)poll_rc_;

        /* If we're done submitting, wait for remaining completions */
        if (submitted >= NUM_OPS && completed < NUM_OPS) {
            int wait_rc_ = aura_wait(engine, 1);
            (void)wait_rc_;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    return (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;
}

int main(void) {
    printf("AuraIO Registered Buffers Example\n");
    printf("==================================\n\n");

    /* Create test file */
    printf("Creating test file (%d MB)...\n", FILE_SIZE / (1024 * 1024));
    int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        perror("create test file");
        return 1;
    }

    char *data = calloc(1, FILE_SIZE);
    if (!data || write(wfd, data, FILE_SIZE) != FILE_SIZE) {
        perror("write test file");
        close(wfd);
        unlink(TEST_FILE);
        free(data);
        return 1;
    }
    free(data);
    close(wfd);

    /* Open for reading */
    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open test file");
        unlink(TEST_FILE);
        return 1;
    }

    /* ===================================================================
     * Part 1: Unregistered Buffers (Baseline)
     * ================================================================ */
    printf("\nPart 1: Baseline with unregistered buffers\n");
    printf("Running %d operations...\n", NUM_OPS);

    aura_engine_t *engine_unreg = aura_create();
    if (!engine_unreg) {
        perror("aura_create");
        close(fd);
        unlink(TEST_FILE);
        return 1;
    }

    /* Pre-allocate buffers for unregistered benchmark */
    void *unreg_bufs[NUM_BUFFERS];
    for (int i = 0; i < NUM_BUFFERS; i++) {
        unreg_bufs[i] = aura_buffer_alloc(engine_unreg, BUF_SIZE);
        if (!unreg_bufs[i]) {
            perror("aura_buffer_alloc");
            for (int j = 0; j < i; j++) {
                aura_buffer_free(engine_unreg, unreg_bufs[j]);
            }
            aura_destroy(engine_unreg);
            close(fd);
            unlink(TEST_FILE);
            return 1;
        }
    }

    double unreg_time = run_benchmark(engine_unreg, fd, 0, unreg_bufs);

    aura_stats_t unreg_stats;
    aura_get_stats(engine_unreg, &unreg_stats, sizeof(unreg_stats));

    printf("  Time: %.2f ms\n", unreg_time);
    printf("  Throughput: %.2f MB/s\n", unreg_stats.current_throughput_bps / (1024.0 * 1024.0));
    printf("  P99 Latency: %.3f ms\n", unreg_stats.p99_latency_ms);

    for (int i = 0; i < NUM_BUFFERS; i++) {
        aura_buffer_free(engine_unreg, unreg_bufs[i]);
    }
    aura_destroy(engine_unreg);

    /* ===================================================================
     * Part 2: Registered Buffers (Zero-Copy)
     * ================================================================ */
    printf("\nPart 2: Zero-copy with registered buffers\n");
    printf("Registering %d buffers of %d bytes each...\n", NUM_BUFFERS, BUF_SIZE);

    aura_engine_t *engine_reg = aura_create();
    if (!engine_reg) {
        perror("aura_create");
        close(fd);
        unlink(TEST_FILE);
        return 1;
    }

    /* Allocate and register buffers */
    void *buffers[NUM_BUFFERS];
    struct iovec iovs[NUM_BUFFERS];

    for (int i = 0; i < NUM_BUFFERS; i++) {
        buffers[i] = aura_buffer_alloc(engine_reg, BUF_SIZE);
        if (!buffers[i]) {
            perror("aura_buffer_alloc");
            for (int j = 0; j < i; j++) {
                aura_buffer_free(engine_reg, buffers[j]);
            }
            aura_destroy(engine_reg);
            close(fd);
            unlink(TEST_FILE);
            return 1;
        }
        iovs[i].iov_base = buffers[i];
        iovs[i].iov_len = BUF_SIZE;
    }

    /* Register buffers with kernel */
    if (aura_register_buffers(engine_reg, iovs, NUM_BUFFERS) < 0) {
        perror("aura_register_buffers");
        for (int i = 0; i < NUM_BUFFERS; i++) {
            aura_buffer_free(engine_reg, buffers[i]);
        }
        aura_destroy(engine_reg);
        close(fd);
        unlink(TEST_FILE);
        return 1;
    }

    printf("Buffers registered successfully.\n");
    printf("Running %d operations with zero-copy I/O...\n", NUM_OPS);

    double reg_time = run_benchmark(engine_reg, fd, 1, NULL);

    aura_stats_t reg_stats;
    aura_get_stats(engine_reg, &reg_stats, sizeof(reg_stats));

    printf("  Time: %.2f ms\n", reg_time);
    printf("  Throughput: %.2f MB/s\n", reg_stats.current_throughput_bps / (1024.0 * 1024.0));
    printf("  P99 Latency: %.3f ms\n", reg_stats.p99_latency_ms);

    /* ===================================================================
     * Part 3: Performance Comparison
     * ================================================================ */
    printf("\n======================================\n");
    printf("Performance Comparison:\n");
    printf("  Unregistered: %.2f ms\n", unreg_time);
    printf("  Registered:   %.2f ms\n", reg_time);
    printf("  Speedup:      %.2fx\n", unreg_time / reg_time);
    printf("  Improvement:  %.1f%%\n", ((unreg_time - reg_time) / unreg_time) * 100.0);

    /* ===================================================================
     * Part 4: Deferred Buffer Unregister (Callback-Safe Pattern)
     * ================================================================ */
    printf("\nPart 4: Deferred buffer unregister\n");
    printf("This pattern allows safe unregister from callback context...\n");

    /* Request deferred unregister (returns immediately) */
    if (aura_request_unregister(engine_reg, AURA_REG_BUFFERS) < 0) {
        perror("aura_request_unregister");
    } else {
        printf("  Unregister requested (will complete when in-flight ops drain)\n");
    }

    /* Wait a bit to ensure unregister completes */
    int wait_rc_ = aura_wait(engine_reg, 100);
    (void)wait_rc_;

    printf("  Buffers unregistered.\n");

    /* Free buffer memory */
    for (int i = 0; i < NUM_BUFFERS; i++) {
        aura_buffer_free(engine_reg, buffers[i]);
    }

    /* Cleanup */
    aura_destroy(engine_reg);
    close(fd);
    unlink(TEST_FILE);

    printf("\n======================================\n");
    printf("When to Use Registered Buffers:\n");
    printf("  ✓ Same buffers reused 1000+ times\n");
    printf("  ✓ High-frequency small I/O (< 16KB)\n");
    printf("  ✓ Zero-copy is critical\n");
    printf("  ✗ One-off or infrequent operations\n");
    printf("  ✗ Dynamic buffer count\n");
    printf("  ✗ Simpler code without registration\n");

    return 0;
}
