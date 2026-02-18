// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file simple_read.c
 * @brief Simple async file read example
 *
 * Demonstrates basic usage of the AuraIO library.
 *
 * Usage: ./simple_read <file>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <aura.h>

#define READ_SIZE (1024 * 1024) /* 1MB */

/* Completion state */
typedef struct {
    int done;
    ssize_t result;
} read_state_t;

/**
 * Callback invoked when read completes.
 */
void on_read_done(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    read_state_t *state = user_data;

    if (result < 0) {
        fprintf(stderr, "Read failed: %s\n", strerror(-result));
    } else {
        printf("Read %zd bytes successfully\n", result);
    }

    state->result = result;
    state->done = 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        fprintf(stderr, "\nReads the first 1MB of a file asynchronously.\n");
        return 1;
    }

    const char *filename = argv[1];

    /* Create the aura engine */
    printf("Creating async I/O engine...\n");
    aura_engine_t *engine = aura_create();
    if (!engine) {
        fprintf(stderr, "Failed to create aura engine: %s\n", strerror(errno));
        return 1;
    }

    /* Open file with O_DIRECT for best async I/O performance */
    int fd = open(filename, O_RDONLY | O_DIRECT);
    if (fd < 0 && errno == EINVAL) {
        /* O_DIRECT not supported on this filesystem - fall back */
        fd = open(filename, O_RDONLY);
        printf("Note: O_DIRECT not available, using buffered I/O\n");
    }
    if (fd < 0) {
        fprintf(stderr, "Failed to open '%s': %s\n", filename, strerror(errno));
        aura_destroy(engine);
        return 1;
    }

    /* Allocate aligned buffer from engine's pool */
    void *buf = aura_buffer_alloc(engine, READ_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer: %s\n", strerror(errno));
        close(fd);
        aura_destroy(engine);
        return 1;
    }

    /* Track completion state */
    read_state_t state = { .done = 0, .result = 0 };

    /* Submit async read */
    printf("Submitting async read of %d bytes...\n", READ_SIZE);
    aura_request_t *req = aura_read(engine, fd, aura_buf(buf), READ_SIZE, 0, on_read_done, &state);
    if (!req) {
        fprintf(stderr, "Failed to submit read: %s\n", strerror(errno));
        aura_buffer_free(engine, buf);
        close(fd);
        aura_destroy(engine);
        return 1;
    }
    (void)req; /* Request handle can be used for cancellation if needed */

    /* Wait for completion */
    printf("Waiting for completion...\n");
    while (!state.done) {
        aura_wait(engine, 100); /* 100ms timeout */
    }

    /* Show first few bytes of data */
    if (state.result > 0) {
        printf("\nFirst 64 bytes of file:\n");
        for (int i = 0; i < 64 && i < state.result; i++) {
            unsigned char c = ((unsigned char *)buf)[i];
            if (c >= 32 && c < 127) {
                putchar(c);
            } else {
                printf("\\x%02x", c);
            }
        }
        printf("\n");
    }

    /* Get aggregate statistics */
    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));
    printf("\nEngine statistics:\n");
    printf("  Ops completed:     %lld\n", (long long)stats.ops_completed);
    printf("  Bytes transferred: %lld\n", (long long)stats.bytes_transferred);
    printf("  Throughput:        %.2f MB/s\n", stats.current_throughput_bps / (1024.0 * 1024.0));
    printf("  P99 latency:       %.2f ms\n", stats.p99_latency_ms);
    printf("  Optimal in-flight: %d\n", stats.optimal_in_flight);
    printf("  Optimal batch:     %d\n", stats.optimal_batch_size);

    /* Per-ring statistics (demonstrates aura_get_ring_stats) */
    int rings = aura_get_ring_count(engine);
    for (int i = 0; i < rings; i++) {
        aura_ring_stats_t rs;
        if (aura_get_ring_stats(engine, i, &rs, sizeof(rs)) == 0) {
            printf("  Ring %d: phase=%s depth=%d/%d\n", i, aura_phase_name(rs.aimd_phase),
                   rs.pending_count, rs.in_flight_limit);
        }
    }

    /* Latency histogram (demonstrates aura_get_histogram) */
    if (rings > 0) {
        aura_histogram_t hist;
        if (aura_get_histogram(engine, 0, &hist, sizeof(hist)) == 0 && hist.total_count > 0) {
            printf("\nLatency Histogram (Ring 0):\n");
            printf("  Total samples: %u\n", hist.total_count);
            printf("  Bucket width:  %d μs\n", hist.bucket_width_us);
            printf("  Max tracked:   %d μs\n", hist.max_tracked_us);

            /* Calculate percentiles from histogram */
            uint32_t cumulative = 0;
            uint32_t p50_threshold = hist.total_count / 2;
            uint32_t p90_threshold = (hist.total_count * 90) / 100;
            uint32_t p99_threshold = (hist.total_count * 99) / 100;
            uint32_t p999_threshold = (hist.total_count * 999) / 1000;
            int p50 = -1, p90 = -1, p99 = -1, p999 = -1;

            for (int i = 0; i < AURA_HISTOGRAM_BUCKETS; i++) {
                cumulative += hist.buckets[i];
                if (p50 == -1 && cumulative >= p50_threshold) p50 = i;
                if (p90 == -1 && cumulative >= p90_threshold) p90 = i;
                if (p99 == -1 && cumulative >= p99_threshold) p99 = i;
                if (p999 == -1 && cumulative >= p999_threshold) p999 = i;
            }

            if (p50 >= 0)
                printf("  P50 latency:   %.2f ms\n", (p50 * hist.bucket_width_us) / 1000.0);
            if (p90 >= 0)
                printf("  P90 latency:   %.2f ms\n", (p90 * hist.bucket_width_us) / 1000.0);
            if (p99 >= 0)
                printf("  P99 latency:   %.2f ms\n", (p99 * hist.bucket_width_us) / 1000.0);
            if (p999 >= 0)
                printf("  P99.9 latency: %.2f ms\n", (p999 * hist.bucket_width_us) / 1000.0);
            if (hist.overflow > 0) {
                printf("  Overflow:      %u samples (> %d μs)\n", hist.overflow,
                       hist.max_tracked_us);
            }
        }
    }

    /* Buffer pool statistics (demonstrates aura_get_buffer_stats) */
    aura_buffer_stats_t buf_stats;
    if (aura_get_buffer_stats(engine, &buf_stats) == 0) {
        printf("\nBuffer Pool Statistics:\n");
        printf("  Total allocated:  %zu bytes\n", buf_stats.total_allocated_bytes);
        printf("  Buffer count:     %zu\n", buf_stats.total_buffers);
        printf("  Shard count:      %d\n", buf_stats.shard_count);
    }

    /* Cleanup */
    aura_buffer_free(engine, buf);
    close(fd);
    aura_destroy(engine);

    printf("\nDone!\n");
    return (state.result > 0) ? 0 : 1;
}
