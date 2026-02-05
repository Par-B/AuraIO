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

#include "auraio.h"

#define READ_SIZE (1024 * 1024)  /* 1MB */

/* Completion state */
typedef struct {
    int done;
    ssize_t result;
} read_state_t;

/**
 * Callback invoked when read completes.
 */
void on_read_done(auraio_request_t *req, ssize_t result, void *user_data) {
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

    /* Create the auraio engine */
    printf("Creating async I/O engine...\n");
    auraio_engine_t *engine = auraio_create();
    if (!engine) {
        fprintf(stderr, "Failed to create auraio engine: %s\n", strerror(errno));
        return 1;
    }

    /* Open file with O_DIRECT for best async I/O performance */
    int fd = open(filename, O_RDONLY | O_DIRECT);
    if (fd < 0) {
        /* Fall back to non-direct if O_DIRECT not supported */
        fd = open(filename, O_RDONLY);
        if (fd < 0) {
            fprintf(stderr, "Failed to open '%s': %s\n", filename, strerror(errno));
            auraio_destroy(engine);
            return 1;
        }
        printf("Note: O_DIRECT not available, using buffered I/O\n");
    }

    /* Allocate aligned buffer from engine's pool */
    void *buf = auraio_buffer_alloc(engine, READ_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer: %s\n", strerror(errno));
        close(fd);
        auraio_destroy(engine);
        return 1;
    }

    /* Track completion state */
    read_state_t state = { .done = 0, .result = 0 };

    /* Submit async read */
    printf("Submitting async read of %d bytes...\n", READ_SIZE);
    auraio_request_t *req = auraio_read(engine, fd, auraio_buf(buf), READ_SIZE, 0, on_read_done, &state);
    if (!req) {
        fprintf(stderr, "Failed to submit read: %s\n", strerror(errno));
        auraio_buffer_free(engine, buf, READ_SIZE);
        close(fd);
        auraio_destroy(engine);
        return 1;
    }
    (void)req;  /* Request handle can be used for cancellation if needed */

    /* Wait for completion */
    printf("Waiting for completion...\n");
    while (!state.done) {
        auraio_wait(engine, 100);  /* 100ms timeout */
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

    /* Get statistics */
    auraio_stats_t stats;
    auraio_get_stats(engine, &stats);
    printf("\nEngine statistics:\n");
    printf("  Ops completed:     %lld\n", stats.ops_completed);
    printf("  Bytes transferred: %lld\n", stats.bytes_transferred);
    printf("  Throughput:        %.2f MB/s\n", stats.current_throughput_bps / (1024 * 1024));
    printf("  P99 latency:       %.2f ms\n", stats.p99_latency_ms);
    printf("  Optimal in-flight: %d\n", stats.optimal_in_flight);
    printf("  Optimal batch:     %d\n", stats.optimal_batch_size);

    /* Cleanup */
    auraio_buffer_free(engine, buf, READ_SIZE);
    close(fd);
    auraio_destroy(engine);

    printf("\nDone!\n");
    return (state.result > 0) ? 0 : 1;
}
