/**
 * @file custom_config.c
 * @brief Demonstrate AuraIO custom configuration options
 *
 * Shows how to tune the engine for different workload characteristics:
 * - Ring count and queue depth
 * - In-flight limits and target latency
 * - Ring selection strategies
 *
 * Build: make examples
 * Run:   ./examples/C/custom_config
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

#define TEST_FILE "/tmp/aura_config_test.dat"
#define FILE_SIZE (4 * 1024 * 1024) /* 4 MB */
#define BUF_SIZE 4096
#define NUM_OPS 20

static _Atomic int completed = 0;

void completion_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    if (result < 0) {
        fprintf(stderr, "I/O error: %zd\n", result);
    }
    atomic_fetch_add_explicit(&completed, 1, memory_order_relaxed);
}

void print_stats(const char *config_name, aura_engine_t *engine, double elapsed_ms) {
    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));

    printf("\n%s Configuration:\n", config_name);
    printf("  Elapsed time: %.2f ms\n", elapsed_ms);
    printf("  Operations: %lld\n", (long long)stats.ops_completed);
    printf("  Throughput: %.2f MB/s\n", stats.current_throughput_bps / (1024.0 * 1024.0));
    printf("  P99 Latency: %.3f ms\n", stats.p99_latency_ms);
    printf("  Optimal in-flight: %d\n", stats.optimal_in_flight);
    printf("  Optimal batch size: %d\n", stats.optimal_batch_size);
}

void run_workload(aura_engine_t *engine, int fd, const char *config_name) {
    struct timespec start, end;

/* Allocate 16 buffers for concurrent operations */
#define CONCURRENT_BUFS 16
    void *bufs[CONCURRENT_BUFS];
    for (int i = 0; i < CONCURRENT_BUFS; i++) {
        bufs[i] = aura_buffer_alloc(engine, BUF_SIZE);
        if (!bufs[i]) {
            perror("aura_buffer_alloc");
            for (int j = 0; j < i; j++) {
                aura_buffer_free(engine, bufs[j]);
            }
            return;
        }
    }

    completed = 0;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Submit NUM_OPS reads at random offsets with proper pacing */
    int submitted = 0;
    while (submitted < NUM_OPS || completed < NUM_OPS) {
        /* Submit new operations while under the concurrency limit */
        while (submitted < NUM_OPS && (submitted - completed) < CONCURRENT_BUFS / 2) {
            off_t offset = (rand() % (FILE_SIZE / BUF_SIZE)) * BUF_SIZE;
            void *buf = bufs[submitted % CONCURRENT_BUFS];
            aura_request_t *req =
                aura_read(engine, fd, aura_buf(buf), BUF_SIZE, offset, completion_callback, NULL);
            if (!req) {
                perror("aura_read");
                break;
            }
            submitted++;
        }

        /* Poll for completions */
        aura_poll(engine);

        /* If we're done submitting, wait for remaining completions */
        if (submitted >= NUM_OPS && completed < NUM_OPS) {
            aura_wait(engine, 1);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed =
        (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1000000.0;

    print_stats(config_name, engine, elapsed);

    for (int i = 0; i < CONCURRENT_BUFS; i++) {
        aura_buffer_free(engine, bufs[i]);
    }
}

int main(void) {
    printf("AuraIO Custom Configuration Examples\n");
    printf("=====================================\n\n");

    /* Create test file */
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
     * Example 1: Default Configuration
     * ================================================================ */
    printf("Running with default configuration...\n");
    aura_engine_t *engine_default = aura_create();
    if (engine_default) {
        run_workload(engine_default, fd, "Default");
        aura_destroy(engine_default);
    }

    /* ===================================================================
     * Example 2: High-Throughput Configuration
     * - Larger queue depth for more pipelining
     * - Higher initial in-flight limit
     * - Round-robin ring selection for single-thread scaling
     * ================================================================ */
    printf("\nConfiguring for high throughput...\n");
    aura_options_t opts_throughput;
    aura_options_init(&opts_throughput);

    opts_throughput.queue_depth = 512; /* Deeper queues for more pipelining */
    opts_throughput.initial_in_flight = 128; /* Start with high concurrency */
    opts_throughput.ring_select = AURA_SELECT_ROUND_ROBIN; /* Max single-thread scaling */

    aura_engine_t *engine_throughput = aura_create_with_options(&opts_throughput);
    if (engine_throughput) {
        run_workload(engine_throughput, fd, "High Throughput");
        aura_destroy(engine_throughput);
    }

    /* ===================================================================
     * Example 3: Low-Latency Configuration
     * - Target specific P99 latency
     * - Conservative in-flight limit to reduce queuing
     * - CPU-local ring selection for best cache locality
     * ================================================================ */
    printf("\nConfiguring for low latency...\n");
    aura_options_t opts_latency;
    aura_options_init(&opts_latency);

    opts_latency.max_p99_latency_ms = 1.0; /* Target 1ms P99 */
    opts_latency.initial_in_flight = 8; /* Start conservative */
    opts_latency.min_in_flight = 4; /* Never go below 4 */
    opts_latency.ring_select = AURA_SELECT_CPU_LOCAL; /* Best cache locality */

    aura_engine_t *engine_latency = aura_create_with_options(&opts_latency);
    if (engine_latency) {
        run_workload(engine_latency, fd, "Low Latency");
        aura_destroy(engine_latency);
    }

    /* ===================================================================
     * Example 4: Adaptive Configuration (Recommended for Production)
     * - Let AIMD tuning find optimal settings
     * - Adaptive ring selection with congestion-based spilling
     * - Moderate queue depth for good balance
     * ================================================================ */
    printf("\nConfiguring for adaptive tuning (production recommended)...\n");
    aura_options_t opts_adaptive;
    aura_options_init(&opts_adaptive);

    opts_adaptive.queue_depth = 256; /* Balanced queue depth */
    opts_adaptive.ring_select = AURA_SELECT_ADAPTIVE; /* Power-of-two spilling */
    opts_adaptive.max_p99_latency_ms = 5.0; /* Reasonable latency target */

    aura_engine_t *engine_adaptive = aura_create_with_options(&opts_adaptive);
    if (engine_adaptive) {
        run_workload(engine_adaptive, fd, "Adaptive (Recommended)");
        aura_destroy(engine_adaptive);
    }

    /* ===================================================================
     * Example 5: Custom Ring Count
     * - Useful for NUMA systems or limiting resource usage
     * ================================================================ */
    printf("\nConfiguring with custom ring count...\n");
    aura_options_t opts_custom_rings;
    aura_options_init(&opts_custom_rings);

    opts_custom_rings.ring_count = 2; /* Use only 2 rings (vs default per-CPU) */
    opts_custom_rings.queue_depth = 256;

    aura_engine_t *engine_custom = aura_create_with_options(&opts_custom_rings);
    if (engine_custom) {
        run_workload(engine_custom, fd, "Custom Ring Count (2)");
        aura_destroy(engine_custom);
    }

    /* Cleanup */
    close(fd);
    unlink(TEST_FILE);

    printf("\n======================================\n");
    printf("Configuration Summary:\n");
    printf("- Default: Auto-configured for system\n");
    printf("- High Throughput: Large queues, round-robin, high concurrency\n");
    printf("- Low Latency: CPU-local, target P99, conservative limits\n");
    printf("- Adaptive: RECOMMENDED for production - automatic tuning\n");
    printf("- Custom Rings: Control resource usage\n");
    printf("\nChoose configuration based on your workload characteristics.\n");

    return 0;
}
