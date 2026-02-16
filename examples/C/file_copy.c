/**
 * @file file_copy.c
 * @brief Synchronous file copy using AuraIO async I/O
 *
 * Demonstrates copying a file using blocking async operations.
 * Uses read-then-write pattern with completion polling.
 *
 * Usage: ./file_copy <source> <destination>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#include <aura.h>

#define CHUNK_SIZE (256 * 1024) /* 256KB chunks */

typedef struct {
    int done;
    ssize_t result;
} io_state_t;

static void on_io_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    io_state_t *state = (io_state_t *)user_data;
    state->result = result;
    state->done = 1;
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <source> <destination>\n", argv[0]);
        fprintf(stderr, "\nCopies a file using async I/O.\n");
        return 1;
    }

    const char *src_path = argv[1];
    const char *dst_path = argv[2];

    printf("AuraIO File Copy (C)\n");
    printf("====================\n");
    printf("Source:      %s\n", src_path);
    printf("Destination: %s\n", dst_path);

    /* Open source file */
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        fprintf(stderr, "Failed to open source '%s': %s\n", src_path, strerror(errno));
        return 1;
    }

    /* Get source file size */
    struct stat st;
    if (fstat(src_fd, &st) < 0) {
        fprintf(stderr, "Failed to stat '%s': %s\n", src_path, strerror(errno));
        close(src_fd);
        return 1;
    }
    off_t file_size = st.st_size;

    printf("File size:   %lld bytes (%.2f MB)\n", (long long)file_size,
           (double)file_size / (1024.0 * 1024.0));

    /* Create destination file */
    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        fprintf(stderr, "Failed to create destination '%s': %s\n", dst_path, strerror(errno));
        close(src_fd);
        return 1;
    }

    /* Create engine */
    aura_engine_t *engine = aura_create();
    if (!engine) {
        fprintf(stderr, "Failed to create engine: %s\n", strerror(errno));
        close(src_fd);
        close(dst_fd);
        return 1;
    }

    /* Allocate buffer */
    void *buf = aura_buffer_alloc(engine, CHUNK_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        aura_destroy(engine);
        close(src_fd);
        close(dst_fd);
        return 1;
    }

    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    printf("\nCopying...\n");

    off_t offset = 0;
    size_t total_copied = 0;

    while (offset < file_size) {
        size_t chunk = CHUNK_SIZE;
        if ((offset + (off_t)chunk) > file_size) {
            chunk = (size_t)(file_size - offset);
        }

        /* Read chunk from source */
        io_state_t read_state = { .done = 0, .result = 0 };
        aura_request_t *req = aura_read(engine, src_fd, aura_buf(buf), chunk, offset,
                                            on_io_complete, &read_state);
        if (!req) {
            fprintf(stderr, "\nRead submission failed at offset %lld: %s\n", (long long)offset,
                    strerror(errno));
            break;
        }

        /* Wait for read to complete */
        while (!read_state.done) {
            aura_wait(engine, 100);
        }

        if (read_state.result <= 0) {
            if (read_state.result < 0) {
                fprintf(stderr, "\nRead failed at offset %lld: %s\n", (long long)offset,
                        strerror(-(int)read_state.result));
            }
            break;
        }

        size_t bytes_read = (size_t)read_state.result;

        /* Write chunk to destination */
        io_state_t write_state = { .done = 0, .result = 0 };
        req = aura_write(engine, dst_fd, aura_buf(buf), bytes_read, offset, on_io_complete,
                           &write_state);
        if (!req) {
            fprintf(stderr, "\nWrite submission failed at offset %lld: %s\n", (long long)offset,
                    strerror(errno));
            break;
        }

        /* Wait for write to complete */
        while (!write_state.done) {
            aura_wait(engine, 100);
        }

        if (write_state.result < 0) {
            fprintf(stderr, "\nWrite failed at offset %lld: %s\n", (long long)offset,
                    strerror(-(int)write_state.result));
            break;
        }

        if ((size_t)write_state.result != bytes_read) {
            fprintf(stderr, "\nShort write at offset %lld: wrote %zd of %zu bytes\n",
                    (long long)offset, write_state.result, bytes_read);
            break;
        }

        offset += bytes_read;
        total_copied += bytes_read;

        /* Progress indicator (every 10MB) */
        if (total_copied % (10 * 1024 * 1024) == 0 || offset >= file_size) {
            double pct = (double)offset / (double)file_size * 100.0;
            printf("  Progress: %.1f%% (%zu / %lld bytes)\n", pct, total_copied,
                   (long long)file_size);
        }
    }

    /* Flush data to disk */
    if (total_copied == (size_t)file_size) {
        printf("\nFlushing to disk...\n");
        io_state_t fsync_state = { .done = 0, .result = 0 };
        aura_request_t *req =
            aura_fsync(engine, dst_fd, AURA_FSYNC_DEFAULT, on_io_complete, &fsync_state);
        if (!req) {
            fprintf(stderr, "fsync submission failed: %s\n", strerror(errno));
        } else {
            while (!fsync_state.done) {
                aura_wait(engine, 100);
            }
            if (fsync_state.result < 0) {
                fprintf(stderr, "fsync failed: %s\n", strerror(-(int)fsync_state.result));
            }
        }
    }

    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed =
        (end_time.tv_sec - start_time.tv_sec) + (end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    printf("\nCopy complete!\n");
    printf("  Bytes copied: %zu / %lld\n", total_copied, (long long)file_size);
    printf("  Time elapsed: %.2f seconds\n", elapsed);
    if (elapsed > 0) {
        double throughput = (double)total_copied / elapsed / (1024.0 * 1024.0);
        printf("  Throughput:   %.2f MB/s\n", throughput);
    }

    /* Cleanup */
    aura_buffer_free(engine, buf, CHUNK_SIZE);
    aura_destroy(engine);
    close(src_fd);
    close(dst_fd);

    return (total_copied == (size_t)file_size) ? 0 : 1;
}
