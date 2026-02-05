/**
 * @file bench_buffer.c
 * @brief Microbenchmark for buffer pool allocation performance
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "adaptive_buffer.h"

#define ITERATIONS 1000000
#define NUM_THREADS 4
#define MIXED_SIZES 8

static buffer_pool_t pool;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* Benchmark 1: Single-threaded alloc/free cycle */
static void bench_single_thread_cycle(void) {
    uint64_t start, end;
    void *buf;

    /* Warm up the pool */
    for (int i = 0; i < 1000; i++) {
        buf = buffer_pool_alloc(&pool, 4096);
        buffer_pool_free(&pool, buf, 4096);
    }

    start = now_ns();
    for (int i = 0; i < ITERATIONS; i++) {
        buf = buffer_pool_alloc(&pool, 4096);
        buffer_pool_free(&pool, buf, 4096);
    }
    end = now_ns();

    double ns_per_op = (double)(end - start) / ITERATIONS;
    double ops_per_sec = 1e9 / ns_per_op;

    printf("  Single-thread alloc/free (4KB):  %.1f ns/op  (%.2fM ops/sec)\n",
           ns_per_op, ops_per_sec / 1e6);
}

/* Benchmark 2: Alloc-only (cache miss path) */
static void bench_alloc_only(void) {
    uint64_t start, end;
    void **bufs = malloc(10000 * sizeof(void*));

    /* Allocate fresh buffers (no reuse) */
    start = now_ns();
    for (int i = 0; i < 10000; i++) {
        bufs[i] = buffer_pool_alloc(&pool, 4096);
    }
    end = now_ns();

    double ns_per_op = (double)(end - start) / 10000;

    printf("  Fresh allocation (posix_memalign): %.1f ns/op\n", ns_per_op);

    /* Clean up */
    for (int i = 0; i < 10000; i++) {
        buffer_pool_free(&pool, bufs[i], 4096);
    }
    free(bufs);
}

/* Benchmark 3: Cache hit path only */
static void bench_cache_hit(void) {
    uint64_t start, end;
    void *buf;

    /* Pre-populate cache */
    buf = buffer_pool_alloc(&pool, 4096);
    buffer_pool_free(&pool, buf, 4096);

    start = now_ns();
    for (int i = 0; i < ITERATIONS; i++) {
        buf = buffer_pool_alloc(&pool, 4096);
        buffer_pool_free(&pool, buf, 4096);
    }
    end = now_ns();

    double ns_per_op = (double)(end - start) / ITERATIONS / 2; /* /2 for alloc only */
    double ops_per_sec = 1e9 / ns_per_op;

    printf("  Cache hit alloc (O(1) pop):      %.1f ns/op  (%.2fM ops/sec)\n",
           ns_per_op, ops_per_sec / 1e6);
}

/* Benchmark 4: Mixed sizes */
static void bench_mixed_sizes(void) {
    uint64_t start, end;
    size_t sizes[MIXED_SIZES] = {1024, 2048, 4096, 5000, 8192, 12000, 16384, 32768};
    void *bufs[MIXED_SIZES];

    /* Warm up */
    for (int j = 0; j < 100; j++) {
        for (int i = 0; i < MIXED_SIZES; i++) {
            bufs[i] = buffer_pool_alloc(&pool, sizes[i]);
        }
        for (int i = 0; i < MIXED_SIZES; i++) {
            buffer_pool_free(&pool, bufs[i], sizes[i]);
        }
    }

    start = now_ns();
    for (int j = 0; j < ITERATIONS / MIXED_SIZES; j++) {
        for (int i = 0; i < MIXED_SIZES; i++) {
            bufs[i] = buffer_pool_alloc(&pool, sizes[i]);
        }
        for (int i = 0; i < MIXED_SIZES; i++) {
            buffer_pool_free(&pool, bufs[i], sizes[i]);
        }
    }
    end = now_ns();

    double ns_per_op = (double)(end - start) / ITERATIONS;
    printf("  Mixed sizes (1KB-32KB):          %.1f ns/op\n", ns_per_op);
}

/* Thread worker for contention benchmark */
typedef struct {
    int thread_id;
    uint64_t ops;
    uint64_t duration_ns;
} thread_result_t;

static void *contention_worker(void *arg) {
    thread_result_t *result = (thread_result_t *)arg;
    void *buf;
    uint64_t start, end;
    int ops = ITERATIONS / NUM_THREADS;

    start = now_ns();
    for (int i = 0; i < ops; i++) {
        buf = buffer_pool_alloc(&pool, 4096);
        buffer_pool_free(&pool, buf, 4096);
    }
    end = now_ns();

    result->ops = ops;
    result->duration_ns = end - start;
    return NULL;
}

/* Benchmark 5: Multi-threaded contention */
static void bench_contention(void) {
    pthread_t threads[NUM_THREADS];
    thread_result_t results[NUM_THREADS];

    /* Warm up pool */
    for (int i = 0; i < NUM_THREADS * 10; i++) {
        void *buf = buffer_pool_alloc(&pool, 4096);
        buffer_pool_free(&pool, buf, 4096);
    }

    uint64_t start = now_ns();

    for (int i = 0; i < NUM_THREADS; i++) {
        results[i].thread_id = i;
        pthread_create(&threads[i], NULL, contention_worker, &results[i]);
    }

    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t end = now_ns();

    uint64_t total_ops = 0;
    for (int i = 0; i < NUM_THREADS; i++) {
        total_ops += results[i].ops;
    }

    double wall_time_s = (double)(end - start) / 1e9;
    double throughput = total_ops / wall_time_s;
    double ns_per_op = (double)(end - start) / total_ops * NUM_THREADS;

    printf("  %d-thread contention:             %.1f ns/op  (%.2fM ops/sec total)\n",
           NUM_THREADS, ns_per_op, throughput / 1e6);
}

int main(void) {
    printf("Buffer Pool Performance Benchmark\n");
    printf("==================================\n\n");

    if (buffer_pool_init(&pool, 4096) != 0) {
        fprintf(stderr, "Failed to initialize buffer pool\n");
        return 1;
    }

    printf("Running benchmarks (%d iterations)...\n\n", ITERATIONS);

    bench_single_thread_cycle();
    bench_cache_hit();
    bench_alloc_only();
    bench_mixed_sizes();
    bench_contention();

    printf("\nKey metrics:\n");
    printf("  - O(1) allocation from cache (no linear scan)\n");
    printf("  - Power-of-2 rounding enables direct bucket access\n");
    printf("  - Single global mutex still limits multi-thread scaling\n");

    buffer_pool_destroy(&pool);

    return 0;
}
