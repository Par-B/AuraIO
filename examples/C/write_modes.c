/**
 * @file write_modes.c
 * @brief Demonstrates O_DIRECT vs buffered I/O with AuraIO
 *
 * Shows that AuraIO is agnostic to I/O mode - it works with both
 * O_DIRECT and buffered writes. The adaptive tuning measures actual
 * completion latency regardless of how the kernel handles the I/O.
 *
 * Usage: ./write_modes <file>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include "auraio.h"

#define WRITE_SIZE  (64 * 1024)   /* 64KB per write */
#define NUM_WRITES  16            /* Number of writes per test */

/* Completion tracking */
typedef struct {
    int completed;
    int total;
    int64_t start_ns;
    int64_t end_ns;
} write_state_t;

static int64_t get_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * Callback invoked when write completes.
 */
void on_write_done(auraio_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    write_state_t *state = user_data;

    if (result < 0) {
        fprintf(stderr, "Write failed: %s\n", strerror(-result));
    }

    state->completed++;
    if (state->completed == state->total) {
        state->end_ns = get_time_ns();
    }
}

/**
 * Run write test with specified mode.
 */
int run_write_test(const char *filename, int use_direct) {
    const char *mode_name = use_direct ? "O_DIRECT" : "Buffered";
    printf("\n=== Testing %s writes ===\n", mode_name);

    /* Create engine */
    auraio_engine_t *engine = auraio_create();
    if (!engine) {
        fprintf(stderr, "Failed to create engine: %s\n", strerror(errno));
        return -1;
    }

    /* Open file with appropriate flags */
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (use_direct) {
        flags |= O_DIRECT;
    }

    int fd = open(filename, flags, 0644);
    if (fd < 0) {
        if (use_direct && errno == EINVAL) {
            printf("O_DIRECT not supported on this filesystem, skipping\n");
            auraio_destroy(engine);
            return 0;
        }
        fprintf(stderr, "Failed to open '%s': %s\n", filename, strerror(errno));
        auraio_destroy(engine);
        return -1;
    }

    /* Allocate buffer - use aligned for O_DIRECT, regular malloc for buffered */
    void *buf;
    if (use_direct) {
        buf = auraio_buffer_alloc(engine, WRITE_SIZE);
        printf("Using aligned buffer from auraio_buffer_alloc()\n");
    } else {
        buf = malloc(WRITE_SIZE);
        printf("Using regular malloc() buffer\n");
    }

    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        close(fd);
        auraio_destroy(engine);
        return -1;
    }

    /* Fill buffer with pattern */
    memset(buf, 'A', WRITE_SIZE);

    /* Track completion */
    write_state_t state = {
        .completed = 0,
        .total = NUM_WRITES,
        .start_ns = get_time_ns(),
        .end_ns = 0
    };

    /* Submit all writes */
    printf("Submitting %d async writes of %d bytes each...\n", NUM_WRITES, WRITE_SIZE);
    for (int i = 0; i < NUM_WRITES; i++) {
        off_t offset = i * WRITE_SIZE;
        auraio_request_t *req = auraio_write(engine, fd, auraio_buf(buf), WRITE_SIZE, offset, on_write_done, &state);
        if (!req) {
            fprintf(stderr, "Failed to submit write %d: %s\n", i, strerror(errno));
            break;
        }
        (void)req;  /* Request handle can be used for cancellation if needed */
    }

    /* Wait for all completions */
    while (state.completed < state.total) {
        auraio_wait(engine, 100);
    }

    /* Calculate results */
    double elapsed_sec = (double)(state.end_ns - state.start_ns) / 1e9;
    double total_mb = (double)(NUM_WRITES * WRITE_SIZE) / (1024 * 1024);
    double throughput = total_mb / elapsed_sec;

    printf("Completed %d writes in %.3f seconds\n", state.completed, elapsed_sec);
    printf("Total: %.2f MB, Throughput: %.2f MB/s\n", total_mb, throughput);

    /* Get engine stats */
    auraio_stats_t stats;
    auraio_get_stats(engine, &stats);
    printf("P99 latency: %.2f ms\n", stats.p99_latency_ms);
    printf("Optimal in-flight: %d\n", stats.optimal_in_flight);

    /* Fsync to ensure data is on disk */
    printf("Fsyncing...\n");
    fsync(fd);

    /* Cleanup */
    if (use_direct) {
        auraio_buffer_free(engine, buf, WRITE_SIZE);
    } else {
        free(buf);
    }
    close(fd);
    auraio_destroy(engine);

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        fprintf(stderr, "\nDemonstrates O_DIRECT vs buffered async writes.\n");
        fprintf(stderr, "The file will be created/overwritten.\n");
        return 1;
    }

    const char *filename = argv[1];

    printf("AuraIO Write Modes Example\n");
    printf("==========================\n");
    printf("File: %s\n", filename);
    printf("Write size: %d bytes\n", WRITE_SIZE);
    printf("Number of writes: %d\n", NUM_WRITES);

    /* Test buffered I/O first */
    if (run_write_test(filename, 0) < 0) {
        return 1;
    }

    /* Test O_DIRECT */
    if (run_write_test(filename, 1) < 0) {
        return 1;
    }

    /* Cleanup test file */
    unlink(filename);

    printf("\n=== Summary ===\n");
    printf("Both modes use the same AuraIO API.\n");
    printf("The library is agnostic - it just submits to io_uring.\n");
    printf("O_DIRECT: Requires aligned buffers, bypasses page cache.\n");
    printf("Buffered: Any buffer works, uses page cache.\n");

    return 0;
}
