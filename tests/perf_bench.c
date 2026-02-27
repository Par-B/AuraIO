// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file perf_bench.c
 * @brief Performance benchmarks for AuraIO library
 *
 * Designed for use with perf, flamegraphs, and other profiling tools.
 * Tests throughput, latency, and syscall efficiency.
 *
 * Build: make -C tests perf_bench
 * Run:   ./tests/perf_bench [test_name] [duration_sec]
 *
 * With perf:
 *   sudo perf stat ./tests/perf_bench throughput 10
 *   sudo perf record -g ./tests/perf_bench throughput 10 && sudo perf report
 */

#define _GNU_SOURCE
#include <aura.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/statvfs.h>

// ============================================================================
// Fast thread-local PRNG (xorshift64)
// ============================================================================

static _Thread_local uint64_t rng_state;

static void rng_seed(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    rng_state = (uint64_t)ts.tv_nsec ^ ((uint64_t)gettid() * 2654435761ULL);
    if (rng_state == 0) rng_state = 1;
}

static inline uint64_t fast_rand(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

// ============================================================================
// Timing utilities
// ============================================================================

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline double ns_to_ms(uint64_t ns) {
    return (double)ns / 1000000.0;
}

static inline double ns_to_sec(uint64_t ns) {
    return (double)ns / 1000000000.0;
}

// ============================================================================
// Statistics tracking
// ============================================================================

typedef struct {
    _Atomic uint64_t ops_completed;
    _Atomic uint64_t ops_failed;
    _Atomic uint64_t bytes_transferred;
    _Atomic int inflight;

    // Latency histogram (50us buckets, 200 buckets = 10ms max)
    _Atomic uint64_t latency_hist[200];
    _Atomic uint64_t latency_over_max;
} bench_stats_t;

static void stats_init(bench_stats_t *s) {
    memset(s, 0, sizeof(*s));
}

static void stats_record_latency(bench_stats_t *s, uint64_t latency_ns) {
    uint64_t bucket = latency_ns / 50000; // 50us buckets
    if (bucket < 200) {
        atomic_fetch_add(&s->latency_hist[bucket], 1);
    } else {
        atomic_fetch_add(&s->latency_over_max, 1);
    }
}

static uint64_t stats_p99_latency_us(bench_stats_t *s) {
    uint64_t total = 0;
    for (int i = 0; i < 200; i++) {
        total += atomic_load(&s->latency_hist[i]);
    }
    total += atomic_load(&s->latency_over_max);

    if (total == 0) return 0;

    uint64_t p99_threshold = (total * 99) / 100;
    uint64_t cumulative = 0;

    for (int i = 0; i < 200; i++) {
        cumulative += atomic_load(&s->latency_hist[i]);
        if (cumulative >= p99_threshold) {
            return (uint64_t)(i + 1) * 50; // Return upper bound of bucket in us
        }
    }
    return 10000; // Over max (>10ms)
}

// ============================================================================
// Test file management
// ============================================================================

#define DEFAULT_TEST_DIR "/tmp/aura_perf"
#define NUM_FILES 8
#define FILE_SIZE (1024 * 1024 * 1024ULL) // 1GB per file — match FIO's --size=1024M

static const char *test_dir = DEFAULT_TEST_DIR;
static int test_fds[NUM_FILES];
static char *test_paths[NUM_FILES];
static int apples_mode = 0; // When set, force ring_count=1 for FIO-comparable runs

static int setup_test_files(void) {
    mkdir(test_dir, 0755);

    // Check available disk space before creating test files
    struct statvfs vfs;
    if (statvfs(test_dir, &vfs) == 0) {
        unsigned long long avail = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
        unsigned long long needed = (unsigned long long)NUM_FILES * FILE_SIZE;
        if (avail < needed) {
            fprintf(stderr,
                    "ERROR: Not enough space in %s\n"
                    "  Available: %llu MB\n"
                    "  Required:  %llu MB (%d files x %llu MB)\n"
                    "  Set AURA_BENCH_DIR to a directory with more space.\n",
                    test_dir, avail / (1024 * 1024), needed / (1024 * 1024), NUM_FILES,
                    (unsigned long long)(FILE_SIZE / (1024 * 1024)));
            return -1;
        }
    }

    // Create random data buffer
    char *data = aligned_alloc(4096, 1024 * 1024);
    if (!data) return -1;

    for (int i = 0; i < 1024 * 1024; i++) {
        data[i] = (char)(fast_rand() & 0xFF);
    }

    for (int i = 0; i < NUM_FILES; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/bench_%d.dat", test_dir, i);
        test_paths[i] = strdup(path);

        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            perror("Failed to create test file");
            free(data);
            return -1;
        }

        // Write file
        for (size_t written = 0; written < FILE_SIZE; written += 1024 * 1024) {
            if (write(fd, data, 1024 * 1024) < 0) {
                perror("Write failed");
                close(fd);
                free(data);
                return -1;
            }
        }
        fsync(fd);
        close(fd);
    }

    free(data);

    // Open for reading (O_DIRECT for true async)
    for (int i = 0; i < NUM_FILES; i++) {
        test_fds[i] = open(test_paths[i], O_RDONLY | O_DIRECT);
        if (test_fds[i] < 0) {
            test_fds[i] = open(test_paths[i], O_RDONLY);
            if (i == 0)
                fprintf(stderr, "  [warn] O_DIRECT not supported, using buffered I/O "
                                "(results not comparable to FIO --direct=1)\n");
        }
    }

    printf("Created %d test files of %llu MB each\n", NUM_FILES,
           (unsigned long long)(FILE_SIZE / (1024 * 1024)));
    return 0;
}

static void cleanup_test_files(void) {
    for (int i = 0; i < NUM_FILES; i++) {
        if (test_fds[i] >= 0) close(test_fds[i]);
        if (test_paths[i]) {
            unlink(test_paths[i]);
            free(test_paths[i]);
        }
    }
    rmdir(test_dir);
}

// ============================================================================
// Callback context for tracking request timing
// ============================================================================

typedef struct {
    bench_stats_t *stats;
    uint64_t submit_time;
    size_t size;
    void *buffer;
    aura_engine_t *engine;
} request_ctx_t;

// Callback signature: (req, result, user_data)
static void read_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    request_ctx_t *ctx = user_data;

    uint64_t latency = now_ns() - ctx->submit_time;
    stats_record_latency(ctx->stats, latency);

    if (result > 0) {
        atomic_fetch_add(&ctx->stats->ops_completed, 1);
        atomic_fetch_add(&ctx->stats->bytes_transferred, result);
    } else {
        atomic_fetch_add(&ctx->stats->ops_failed, 1);
    }

    atomic_fetch_sub(&ctx->stats->inflight, 1);

    if (ctx->buffer && ctx->engine) {
        aura_buffer_free(ctx->engine, ctx->buffer);
    }
    free(ctx);
}

// ============================================================================
// Benchmark: Maximum Throughput
// ============================================================================

static void bench_throughput(int duration_sec, int max_inflight, size_t buf_size) {
    printf("\n=== Throughput Benchmark ===\n");
    printf("Duration: %d sec, Max inflight: %d, Buffer: %zu KB\n", duration_sec, max_inflight,
           buf_size / 1024);

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = apples_mode ? (unsigned)max_inflight : 1024;
    opts.ring_count = apples_mode ? 1 : 0;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return;
    }

    bench_stats_t stats;
    stats_init(&stats);

    uint64_t start = now_ns();
    uint64_t end_time = start + (uint64_t)duration_sec * 1000000000ULL;
    uint64_t drain_deadline = end_time + 2000000000ULL; // 2s drain timeout

    // Submit initial batch
    while (atomic_load(&stats.inflight) < max_inflight && now_ns() < end_time) {
        void *buf = aura_buffer_alloc(engine, buf_size);
        if (!buf) break;

        int fd_idx = fast_rand() % NUM_FILES;
        off_t max_off = FILE_SIZE - buf_size;
        off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

        request_ctx_t *ctx = malloc(sizeof(*ctx));
        ctx->stats = &stats;
        ctx->submit_time = now_ns();
        ctx->size = buf_size;
        ctx->buffer = buf;
        ctx->engine = engine;

        atomic_fetch_add(&stats.inflight, 1);

        if (aura_read(engine, test_fds[fd_idx], aura_buf(buf), buf_size, offset, 0, read_callback,
                      ctx) == NULL) {
            atomic_fetch_sub(&stats.inflight, 1);
            aura_buffer_free(engine, buf);
            free(ctx);
        }
    }

    // Main loop: process completions and submit new requests
    while (now_ns() < end_time || atomic_load(&stats.inflight) > 0) {
        if (now_ns() > drain_deadline) {
            int stuck = atomic_load(&stats.inflight);
            if (stuck > 0) fprintf(stderr, "  [warn] drain timeout, %d ops stuck\n", stuck);
            break;
        }

        // Process completions
        aura_wait(engine, 1);

        // Submit more if under limit and time remains
        while (atomic_load(&stats.inflight) < max_inflight && now_ns() < end_time) {
            void *buf = aura_buffer_alloc(engine, buf_size);
            if (!buf) break;

            int fd_idx = fast_rand() % NUM_FILES;
            off_t max_off = FILE_SIZE - buf_size;
            off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

            request_ctx_t *ctx = malloc(sizeof(*ctx));
            ctx->stats = &stats;
            ctx->submit_time = now_ns();
            ctx->size = buf_size;
            ctx->buffer = buf;
            ctx->engine = engine;

            atomic_fetch_add(&stats.inflight, 1);

            if (aura_read(engine, test_fds[fd_idx], aura_buf(buf), buf_size, offset, 0,
                          read_callback, ctx) == NULL) {
                atomic_fetch_sub(&stats.inflight, 1);
                aura_buffer_free(engine, buf);
                free(ctx);
                break;
            }
        }
    }

    uint64_t elapsed = now_ns() - start;
    double elapsed_sec = ns_to_sec(elapsed);

    uint64_t ops = atomic_load(&stats.ops_completed);
    uint64_t bytes = atomic_load(&stats.bytes_transferred);
    uint64_t failed = atomic_load(&stats.ops_failed);

    // Get engine stats
    aura_stats_t engine_stats;
    aura_get_stats(engine, &engine_stats, sizeof(engine_stats));

    printf("\nResults:\n");
    printf("  Operations:      %lu completed, %lu failed\n", ops, failed);
    printf("  Throughput:      %.2f MB/s\n", (bytes / (1024.0 * 1024.0)) / elapsed_sec);
    printf("  IOPS:            %.0f\n", ops / elapsed_sec);
    printf("  P99 latency:     %lu us (measured)\n", stats_p99_latency_us(&stats));
    printf("  P99 latency:     %.2f ms (engine)\n", engine_stats.p99_latency_ms);
    printf("  Optimal depth:   %d (tuned by AIMD)\n", engine_stats.optimal_in_flight);

    aura_destroy(engine);
}

// ============================================================================
// Benchmark: Latency Focus (low concurrency)
// ============================================================================

typedef struct {
    _Atomic int *done;
    ssize_t *result;
} latency_sync_ctx_t;

static void latency_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    latency_sync_ctx_t *ctx = user_data;
    *ctx->result = result;
    atomic_store(ctx->done, 1);
}

static void bench_latency(int duration_sec, size_t buf_size) {
    printf("\n=== Latency Benchmark (Serial) ===\n");
    printf("Duration: %d sec, Buffer: %zu KB\n", duration_sec, buf_size / 1024);

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = 64;
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return;
    }

    bench_stats_t stats;
    stats_init(&stats);

    void *buf = aura_buffer_alloc(engine, buf_size);
    if (!buf) {
        aura_destroy(engine);
        return;
    }

    uint64_t start = now_ns();
    uint64_t end_time = start + (uint64_t)duration_sec * 1000000000ULL;

    // Track min/max/sum for average calculation
    uint64_t min_lat = UINT64_MAX, max_lat = 0, sum_lat = 0;
    uint64_t count = 0;

    while (now_ns() < end_time) {
        int fd_idx = fast_rand() % NUM_FILES;
        off_t max_off = FILE_SIZE - buf_size;
        off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

        uint64_t op_start = now_ns();

        // Synchronous-style: submit and wait immediately
        _Atomic int done = 0;
        ssize_t result = 0;

        latency_sync_ctx_t sync_ctx = { &done, &result };

        if (aura_read(engine, test_fds[fd_idx], aura_buf(buf), buf_size, offset, 0,
                      latency_callback, &sync_ctx) != NULL) {
            while (!atomic_load(&done)) {
                aura_wait(engine, 1);
            }

            uint64_t lat = now_ns() - op_start;
            stats_record_latency(&stats, lat);

            if (lat < min_lat) min_lat = lat;
            if (lat > max_lat) max_lat = lat;
            sum_lat += lat;
            count++;

            if (result > 0) {
                atomic_fetch_add(&stats.ops_completed, 1);
            } else {
                atomic_fetch_add(&stats.ops_failed, 1);
            }
        }
    }

    uint64_t elapsed = now_ns() - start;
    double elapsed_sec = ns_to_sec(elapsed);
    uint64_t ops = atomic_load(&stats.ops_completed);

    printf("\nResults:\n");
    printf("  Operations:      %lu completed\n", ops);
    printf("  IOPS:            %.0f\n", ops / elapsed_sec);
    printf("  Min latency:     %.2f us\n", (double)min_lat / 1000.0);
    printf("  Avg latency:     %.2f us\n", count > 0 ? (double)sum_lat / count / 1000.0 : 0);
    printf("  Max latency:     %.2f us\n", (double)max_lat / 1000.0);
    printf("  P99 latency:     %lu us\n", stats_p99_latency_us(&stats));

    aura_buffer_free(engine, buf);
    aura_destroy(engine);
}

// ============================================================================
// Benchmark: Buffer Pool Performance
// ============================================================================

typedef struct {
    aura_engine_t *engine;
    _Atomic uint64_t *allocs;
    _Atomic uint64_t *frees;
    _Atomic int *running;
} buffer_thread_data_t;

static void *buffer_thread_fn(void *arg) {
    buffer_thread_data_t *td = arg;
    rng_seed(); // Each thread gets unique TLS seed
    size_t sizes[] = { 4096, 8192, 16384, 32768, 65536, 131072 };
    int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    typedef struct {
        void *ptr;
        size_t size;
    } buf_entry_t;

    buf_entry_t buffers[32];
    int buf_count = 0;

    while (atomic_load(td->running)) {
        // Allocate
        if (buf_count < 32) {
            size_t size = sizes[fast_rand() % num_sizes];
            void *buf = aura_buffer_alloc(td->engine, size);
            if (buf) {
                buffers[buf_count].ptr = buf;
                buffers[buf_count].size = size;
                buf_count++;
                atomic_fetch_add(td->allocs, 1);
            }
        }

        // Free some (50% chance)
        if (buf_count > 0 && (fast_rand() % 2)) {
            buf_count--;
            aura_buffer_free(td->engine, buffers[buf_count].ptr);
            atomic_fetch_add(td->frees, 1);
        }
    }

    // Cleanup
    while (buf_count > 0) {
        buf_count--;
        aura_buffer_free(td->engine, buffers[buf_count].ptr);
        atomic_fetch_add(td->frees, 1);
    }

    return NULL;
}

static void bench_buffer_pool(int duration_sec, int num_threads) {
    printf("\n=== Buffer Pool Benchmark ===\n");
    printf("Duration: %d sec, Threads: %d\n", duration_sec, num_threads);

    aura_engine_t *engine = aura_create();
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return;
    }

    _Atomic uint64_t total_allocs = 0;
    _Atomic uint64_t total_frees = 0;
    _Atomic int running = 1;

    pthread_t threads[16]; // Max 16 threads

    buffer_thread_data_t tdata = {
        .engine = engine, .allocs = &total_allocs, .frees = &total_frees, .running = &running
    };

    uint64_t start = now_ns();

    for (int i = 0; i < num_threads && i < 16; i++) {
        pthread_create(&threads[i], NULL, buffer_thread_fn, &tdata);
    }

    sleep(duration_sec);
    atomic_store(&running, 0);

    for (int i = 0; i < num_threads && i < 16; i++) {
        pthread_join(threads[i], NULL);
    }

    uint64_t elapsed = now_ns() - start;
    double elapsed_sec = ns_to_sec(elapsed);

    uint64_t allocs = atomic_load(&total_allocs);
    uint64_t frees = atomic_load(&total_frees);

    printf("\nResults:\n");
    printf("  Total allocs:    %lu\n", allocs);
    printf("  Total frees:     %lu\n", frees);
    printf("  Alloc rate:      %.0f ops/sec\n", allocs / elapsed_sec);
    printf("  Free rate:       %.0f ops/sec\n", frees / elapsed_sec);
    printf("  Combined:        %.0f ops/sec\n", (allocs + frees) / elapsed_sec);
    printf("  Per-thread:      %.0f ops/sec\n", (allocs + frees) / elapsed_sec / num_threads);

    aura_destroy(engine);
}

// ============================================================================
// Benchmark: Scalability (varying queue depth)
// ============================================================================

static void bench_scalability(int duration_sec) {
    printf("\n=== Scalability Benchmark ===\n");
    printf("Testing throughput vs queue depth\n\n");

    // Test various in-flight depths (capped at 128 to avoid hangs)
    int depths[] = { 4, 8, 16, 32, 64, 128 };
    int num_depths = sizeof(depths) / sizeof(depths[0]);

    printf("%-12s %-12s %-12s %-12s\n", "Inflight", "IOPS", "MB/s", "P99(us)");
    printf("%-12s %-12s %-12s %-12s\n", "--------", "----", "----", "-------");

    for (int d = 0; d < num_depths; d++) {
        int depth = depths[d];
        fflush(stdout);

        aura_options_t opts;
        aura_options_init(&opts);
        opts.queue_depth = 512;
        opts.ring_count = 1;

        aura_engine_t *engine = aura_create_with_options(&opts);
        if (!engine) {
            printf("%-12d FAILED (engine create)\n", depth);
            continue;
        }

        bench_stats_t stats;
        stats_init(&stats);

        size_t buf_size = 64 * 1024;
        uint64_t start = now_ns();
        uint64_t end_time = start + (uint64_t)duration_sec * 1000000000ULL;
        // Run benchmark at this depth
        uint64_t drain_deadline = end_time + 2000000000ULL; // 2s drain timeout
        while (now_ns() < end_time || atomic_load(&stats.inflight) > 0) {
            if (now_ns() > drain_deadline) {
                int stuck = atomic_load(&stats.inflight);
                if (stuck > 0) fprintf(stderr, "  [warn] drain timeout, %d ops stuck\n", stuck);
                break;
            }

            // Submit up to depth
            while (atomic_load(&stats.inflight) < depth && now_ns() < end_time) {
                void *buf = aura_buffer_alloc(engine, buf_size);
                if (!buf) break;

                int fd_idx = fast_rand() % NUM_FILES;
                off_t max_off = FILE_SIZE - buf_size;
                off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

                request_ctx_t *ctx = malloc(sizeof(*ctx));
                ctx->stats = &stats;
                ctx->submit_time = now_ns();
                ctx->size = buf_size;
                ctx->buffer = buf;
                ctx->engine = engine;

                atomic_fetch_add(&stats.inflight, 1);

                if (aura_read(engine, test_fds[fd_idx], aura_buf(buf), buf_size, offset, 0,
                              read_callback, ctx) == NULL) {
                    atomic_fetch_sub(&stats.inflight, 1);
                    aura_buffer_free(engine, buf);
                    free(ctx);
                    break;
                }
            }

            aura_wait(engine, 1);
        }

        uint64_t elapsed = now_ns() - start;
        double elapsed_sec = ns_to_sec(elapsed);
        uint64_t ops = atomic_load(&stats.ops_completed);
        uint64_t bytes = atomic_load(&stats.bytes_transferred);

        printf("%-12d %-12.0f %-12.2f %-12lu\n", depth, ops / elapsed_sec,
               (bytes / (1024.0 * 1024.0)) / elapsed_sec, stats_p99_latency_us(&stats));

        aura_destroy(engine);
    }
}

// ============================================================================
// Benchmark: Syscall Efficiency
// ============================================================================

static void bench_syscall_batching(int duration_sec) {
    printf("\n=== Syscall Batching Efficiency ===\n");
    printf("Measuring operations per io_uring_enter syscall\n\n");

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = 512;
    opts.ring_count = 1;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return;
    }

    bench_stats_t stats;
    stats_init(&stats);

    size_t buf_size = 4096; // Small buffers for high IOPS
    int max_inflight = 256;

    uint64_t start = now_ns();
    uint64_t end_time = start + (uint64_t)duration_sec * 1000000000ULL;
    uint64_t drain_deadline = end_time + 2000000000ULL; // 2s drain timeout

    while (now_ns() < end_time || atomic_load(&stats.inflight) > 0) {
        if (now_ns() > drain_deadline) {
            int stuck = atomic_load(&stats.inflight);
            if (stuck > 0) fprintf(stderr, "  [warn] drain timeout, %d ops stuck\n", stuck);
            break;
        }

        // Submit batch
        int submitted = 0;
        while (atomic_load(&stats.inflight) < max_inflight && now_ns() < end_time &&
               submitted < 32) {
            void *buf = aura_buffer_alloc(engine, buf_size);
            if (!buf) break;

            int fd_idx = fast_rand() % NUM_FILES;
            off_t max_off = FILE_SIZE - buf_size;
            off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

            request_ctx_t *ctx = malloc(sizeof(*ctx));
            ctx->stats = &stats;
            ctx->submit_time = now_ns();
            ctx->size = buf_size;
            ctx->buffer = buf;
            ctx->engine = engine;

            atomic_fetch_add(&stats.inflight, 1);

            if (aura_read(engine, test_fds[fd_idx], aura_buf(buf), buf_size, offset, 0,
                          read_callback, ctx) == NULL) {
                atomic_fetch_sub(&stats.inflight, 1);
                aura_buffer_free(engine, buf);
                free(ctx);
                break;
            }
            submitted++;
        }

        aura_wait(engine, 1);
    }

    uint64_t elapsed = now_ns() - start;
    double elapsed_sec = ns_to_sec(elapsed);

    aura_stats_t engine_stats;
    aura_get_stats(engine, &engine_stats, sizeof(engine_stats));

    uint64_t ops = atomic_load(&stats.ops_completed);
    uint64_t bytes = atomic_load(&stats.bytes_transferred);

    printf("Results:\n");
    printf("  Operations:      %lu completed\n", ops);
    printf("  IOPS:            %.0f\n", ops / elapsed_sec);
    printf("  Throughput:      %.2f MB/s\n", (bytes / (1024.0 * 1024.0)) / elapsed_sec);
    printf("  P99 latency:     %.2f ms\n", engine_stats.p99_latency_ms);
    printf("  Optimal depth:   %d\n", engine_stats.optimal_in_flight);
    printf("\nNote: Use 'strace -c' to see actual syscall counts\n");

    aura_destroy(engine);
}

// ============================================================================
// Benchmark: Mixed Workload
// ============================================================================

static void mixed_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    request_ctx_t *ctx = user_data;

    uint64_t latency = now_ns() - ctx->submit_time;
    stats_record_latency(ctx->stats, latency);

    if (result >= 0) {
        atomic_fetch_add(&ctx->stats->ops_completed, 1);
        if (ctx->size > 0 && result > 0) {
            atomic_fetch_add(&ctx->stats->bytes_transferred, result);
        }
    } else {
        atomic_fetch_add(&ctx->stats->ops_failed, 1);
    }

    atomic_fetch_sub(&ctx->stats->inflight, 1);

    if (ctx->buffer && ctx->engine) {
        aura_buffer_free(ctx->engine, ctx->buffer);
    }
    free(ctx);
}

static void bench_mixed_workload(int duration_sec) {
    printf("\n=== Mixed Workload Benchmark ===\n");
    printf("70%% reads, 25%% writes, 5%% fsync\n");

    // Need writable files (O_DIRECT for fair comparison with FIO --direct=1)
    int write_fds[NUM_FILES];
    for (int i = 0; i < NUM_FILES; i++) {
        write_fds[i] = open(test_paths[i], O_RDWR | O_DIRECT);
        if (write_fds[i] < 0) {
            write_fds[i] = open(test_paths[i], O_RDWR);
            if (i == 0)
                fprintf(stderr, "  [warn] O_DIRECT not supported for writes, using buffered I/O\n");
        }
    }

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = apples_mode ? 128 : 512;
    opts.ring_count = apples_mode ? 1 : 0;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "Failed to create engine\n");
        return;
    }

    bench_stats_t stats;
    stats_init(&stats);

    size_t buf_size = 32 * 1024;
    int max_inflight = 128;

    uint64_t reads = 0, writes = 0, fsyncs = 0;

    uint64_t start = now_ns();
    uint64_t end_time = start + (uint64_t)duration_sec * 1000000000ULL;
    uint64_t drain_deadline = end_time + 2000000000ULL; // 2s drain timeout

    while (now_ns() < end_time || atomic_load(&stats.inflight) > 0) {
        if (now_ns() > drain_deadline) {
            int stuck = atomic_load(&stats.inflight);
            if (stuck > 0) fprintf(stderr, "  [warn] drain timeout, %d ops stuck\n", stuck);
            break;
        }

        while (atomic_load(&stats.inflight) < max_inflight && now_ns() < end_time) {
            int op = fast_rand() % 100;
            int fd_idx = fast_rand() % NUM_FILES;

            if (op < 70) {
                // Read
                void *buf = aura_buffer_alloc(engine, buf_size);
                if (!buf) break;

                off_t max_off = FILE_SIZE - buf_size;
                off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

                request_ctx_t *ctx = malloc(sizeof(*ctx));
                ctx->stats = &stats;
                ctx->submit_time = now_ns();
                ctx->size = buf_size;
                ctx->buffer = buf;
                ctx->engine = engine;

                atomic_fetch_add(&stats.inflight, 1);
                reads++;

                if (aura_read(engine, test_fds[fd_idx], aura_buf(buf), buf_size, offset, 0,
                              mixed_callback, ctx) == NULL) {
                    atomic_fetch_sub(&stats.inflight, 1);
                    aura_buffer_free(engine, buf);
                    free(ctx);
                    reads--;
                }
            } else if (op < 95) {
                // Write
                void *buf = aura_buffer_alloc(engine, buf_size);
                if (!buf) break;

                memset(buf, 'W', buf_size);
                off_t max_off = FILE_SIZE - buf_size;
                off_t offset = ((fast_rand() % (max_off / 4096)) * 4096);

                request_ctx_t *ctx = malloc(sizeof(*ctx));
                ctx->stats = &stats;
                ctx->submit_time = now_ns();
                ctx->size = buf_size;
                ctx->buffer = buf;
                ctx->engine = engine;

                atomic_fetch_add(&stats.inflight, 1);
                writes++;

                if (aura_write(engine, write_fds[fd_idx], aura_buf(buf), buf_size, offset, 0,
                               mixed_callback, ctx) == NULL) {
                    atomic_fetch_sub(&stats.inflight, 1);
                    aura_buffer_free(engine, buf);
                    free(ctx);
                    writes--;
                }
            } else {
                // Fsync
                request_ctx_t *ctx = malloc(sizeof(*ctx));
                ctx->stats = &stats;
                ctx->submit_time = now_ns();
                ctx->size = 0;
                ctx->buffer = NULL;
                ctx->engine = engine;

                atomic_fetch_add(&stats.inflight, 1);
                fsyncs++;

                if (aura_fsync(engine, write_fds[fd_idx], AURA_FSYNC_DEFAULT, 0, mixed_callback,
                               ctx) == NULL) {
                    atomic_fetch_sub(&stats.inflight, 1);
                    free(ctx);
                    fsyncs--;
                }
            }
        }

        aura_wait(engine, 1);
    }

    uint64_t elapsed = now_ns() - start;
    double elapsed_sec = ns_to_sec(elapsed);

    aura_stats_t engine_stats;
    aura_get_stats(engine, &engine_stats, sizeof(engine_stats));

    uint64_t ops = atomic_load(&stats.ops_completed);
    uint64_t bytes = atomic_load(&stats.bytes_transferred);

    printf("\nResults:\n");
    printf("  Reads:           %lu\n", reads);
    printf("  Writes:          %lu\n", writes);
    printf("  Fsyncs:          %lu\n", fsyncs);
    printf("  Total ops:       %lu completed\n", ops);
    printf("  Throughput:      %.2f MB/s\n", (bytes / (1024.0 * 1024.0)) / elapsed_sec);
    printf("  IOPS:            %.0f\n", ops / elapsed_sec);
    printf("  P99 latency:     %.2f ms\n", engine_stats.p99_latency_ms);

    for (int i = 0; i < NUM_FILES; i++) {
        close(write_fds[i]);
    }
    aura_destroy(engine);
}

// ============================================================================
// Main
// ============================================================================

static void print_usage(const char *prog) {
    printf("Usage: %s [benchmark] [duration_sec]\n\n", prog);
    printf("Benchmarks:\n");
    printf("  throughput   Maximum throughput test (default)\n");
    printf("  latency      Latency-focused test (serial operations)\n");
    printf("  buffer       Buffer pool allocation performance\n");
    printf("  scalability  Throughput vs queue depth\n");
    printf("  syscall      Syscall batching efficiency\n");
    printf("  mixed        Mixed read/write/fsync workload\n");
    printf("  all          Run all benchmarks\n");
    printf("\nDefault duration: 5 seconds\n");
    printf("\nWith perf:\n");
    printf("  sudo perf stat %s throughput 10\n", prog);
    printf("  sudo perf record -g %s throughput 10 && sudo perf report\n", prog);
}

int main(int argc, char **argv) {
    const char *test = "throughput";
    int duration = 5;

    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        test = argv[1];
    }
    if (argc > 2) {
        duration = atoi(argv[2]);
        if (duration < 1) duration = 1;
        if (duration > 300) duration = 300;
    }

    const char *env_dir = getenv("AURA_BENCH_DIR");
    if (env_dir && env_dir[0]) test_dir = env_dir;

    const char *env_apples = getenv("AURA_BENCH_APPLES");
    apples_mode = (env_apples && env_apples[0] == '1');

    printf("AuraIO Performance Benchmark\n");
    printf("============================\n");
    printf("CPUs: %d, Duration: %d seconds\n", get_nprocs(), duration);

    rng_seed();

    if (setup_test_files() < 0) {
        fprintf(stderr, "Failed to setup test files\n");
        return 1;
    }

    // Flush before potential fork() so children don't re-emit buffered output
    fflush(stdout);

    if (strcmp(test, "throughput") == 0) {
        bench_throughput(duration, 256, 64 * 1024);
    } else if (strcmp(test, "latency") == 0) {
        bench_latency(duration, 4 * 1024);
    } else if (strcmp(test, "buffer") == 0) {
        bench_buffer_pool(duration, 4);
    } else if (strcmp(test, "scalability") == 0) {
        bench_scalability(duration);
    } else if (strcmp(test, "syscall") == 0) {
        bench_syscall_batching(duration);
    } else if (strcmp(test, "mixed") == 0) {
        bench_mixed_workload(duration);
    } else if (strcmp(test, "all") == 0) {
        // Fork each benchmark into its own process to avoid state
        // contamination between sequential benchmarks (io_uring kernel
        // state, library globals, etc.)
        struct {
            const char *name;
            int dur;
        } benchmarks[] = {
            { "throughput", duration },      { "latency", duration }, { "buffer", duration },
            { "scalability", duration / 2 }, { "syscall", duration }, { "mixed", duration },
        };
        int num_benchmarks = (int)(sizeof(benchmarks) / sizeof(benchmarks[0]));
        int all_ok = 1;

        for (int i = 0; i < num_benchmarks; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                all_ok = 0;
                continue;
            }
            if (pid == 0) {
                // Child: run single benchmark and exit
                if (strcmp(benchmarks[i].name, "throughput") == 0)
                    bench_throughput(benchmarks[i].dur, 256, 64 * 1024);
                else if (strcmp(benchmarks[i].name, "latency") == 0)
                    bench_latency(benchmarks[i].dur, 4 * 1024);
                else if (strcmp(benchmarks[i].name, "buffer") == 0)
                    bench_buffer_pool(benchmarks[i].dur, 4);
                else if (strcmp(benchmarks[i].name, "scalability") == 0)
                    bench_scalability(benchmarks[i].dur);
                else if (strcmp(benchmarks[i].name, "syscall") == 0)
                    bench_syscall_batching(benchmarks[i].dur);
                else if (strcmp(benchmarks[i].name, "mixed") == 0)
                    bench_mixed_workload(benchmarks[i].dur);
                fflush(stdout);
                fflush(stderr);
                // Don't cleanup_test_files() — parent owns them
                _exit(0);
            }

            // Parent: wait with timeout.  Scalability runs 6 sub-tests
            // sequentially, so needs dur * #depths + drain headroom.
            // Use dur*8+10 for all benchmarks (generous, simple).
            int timeout_sec = benchmarks[i].dur * 8 + 10;
            int status;
            pid_t ret = 0;
            for (int t = 0; t < timeout_sec * 10; t++) {
                ret = waitpid(pid, &status, WNOHANG);
                if (ret != 0) break;
                usleep(100000); // 100ms poll
            }
            if (ret == 0) {
                fprintf(stderr, "\n  [warn] %s timed out after %ds, killing\n", benchmarks[i].name,
                        timeout_sec);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                all_ok = 0;
            } else if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                fprintf(stderr, "\n  [warn] %s exited with status %d\n", benchmarks[i].name,
                        WEXITSTATUS(status));
                all_ok = 0;
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "\n  [warn] %s killed by signal %d\n", benchmarks[i].name,
                        WTERMSIG(status));
                all_ok = 0;
            }
        }

        if (!all_ok) {
            cleanup_test_files();
            printf("\nBenchmark complete (with warnings).\n");
            return 1;
        }
    } else {
        fprintf(stderr, "Unknown benchmark: %s\n", test);
        print_usage(argv[0]);
        cleanup_test_files();
        return 1;
    }

    cleanup_test_files();
    printf("\nBenchmark complete.\n");
    return 0;
}
