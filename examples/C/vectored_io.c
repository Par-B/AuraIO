// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file vectored_io.c
 * @brief Demonstrate vectored (scatter-gather) I/O operations
 *
 * Shows how to use aura_readv() and aura_writev() for efficient
 * I/O with multiple buffers in a single syscall. Useful for:
 * - Database page operations (header + data in separate buffers)
 * - Structured file formats (metadata + content sections)
 * - Log files (timestamp/metadata + message in separate buffers)
 * - Reducing syscall overhead for non-contiguous data regions
 *
 * Build: make examples
 * Run:   ./examples/C/vectored_io
 */

#include <aura.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define TEST_FILE "/tmp/aura_vectored_test.dat"
#define HEADER_SIZE 64
#define PAYLOAD_SIZE 1024

typedef struct {
    int done;
    ssize_t result;
} io_context_t;

void write_completion(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    io_context_t *ctx = (io_context_t *)user_data;
    ctx->result = result;
    ctx->done = 1;
    if (result < 0) {
        fprintf(stderr, "Write error: %zd\n", result);
    } else {
        printf("Vectored write completed: %zd bytes\n", result);
    }
}

void read_completion(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    io_context_t *ctx = (io_context_t *)user_data;
    ctx->result = result;
    ctx->done = 1;
    if (result < 0) {
        fprintf(stderr, "Read error: %zd\n", result);
    } else {
        printf("Vectored read completed: %zd bytes\n", result);
    }
}

int main(void) {
    printf("AuraIO Vectored I/O Example\n");
    printf("===========================\n\n");

    aura_engine_t *engine = aura_create();
    if (!engine) {
        perror("aura_create");
        return 1;
    }

    /* ===================================================================
     * Example 1: Vectored Write (Gather)
     * Write metadata and data in a single operation (like database page writes)
     * ================================================================ */
    printf("Example 1: Vectored Write (Gather)\n");
    printf("Writing structured data with separate metadata and payload buffers...\n");

    /* Prepare metadata buffer (like a database page header) */
    char *header = calloc(1, HEADER_SIZE);
    if (!header) {
        perror("calloc header");
        aura_destroy(engine);
        return 1;
    }
    snprintf(header, HEADER_SIZE, "METADATA: size=%d, checksum=0xABCD, version=1", PAYLOAD_SIZE);

    /* Prepare payload buffer */
    char *payload = calloc(1, PAYLOAD_SIZE);
    if (!payload) {
        perror("calloc payload");
        free(header);
        aura_destroy(engine);
        return 1;
    }
    memset(payload, 'X', PAYLOAD_SIZE - 1); /* Fill with 'X' */

    /* Create iovec array for gather write */
    struct iovec write_iov[2];
    write_iov[0].iov_base = header;
    write_iov[0].iov_len = HEADER_SIZE;
    write_iov[1].iov_base = payload;
    write_iov[1].iov_len = PAYLOAD_SIZE;

    /* Open file for writing */
    int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        perror("open for write");
        free(payload);
        free(header);
        aura_destroy(engine);
        return 1;
    }

    /* Submit vectored write */
    io_context_t write_ctx = { 0, 0 };
    aura_request_t *write_req =
        aura_writev(engine, wfd, write_iov, 2, 0, 0, write_completion, &write_ctx);
    if (!write_req) {
        perror("aura_writev");
        close(wfd);
        free(payload);
        free(header);
        aura_destroy(engine);
        return 1;
    }

    /* Wait for write completion */
    while (!write_ctx.done) {
        aura_wait(engine, 10);
    }

    printf("  Header (%zu bytes): %s\n", write_iov[0].iov_len, header);
    printf("  Payload (%zu bytes): [%zu bytes of 'X']\n", write_iov[1].iov_len,
           (size_t)PAYLOAD_SIZE);
    printf("  Total written: %zd bytes\n", write_ctx.result);

    close(wfd);
    free(payload);
    free(header);

    /* ===================================================================
     * Example 2: Vectored Read (Scatter)
     * Read metadata and data into separate buffers (like database page reads)
     * ================================================================ */
    printf("\nExample 2: Vectored Read (Scatter)\n");
    printf("Reading structured data into separate metadata and payload buffers...\n");

    /* Allocate separate buffers for header and payload */
    char *read_header = calloc(1, HEADER_SIZE);
    char *read_payload = calloc(1, PAYLOAD_SIZE);
    if (!read_header || !read_payload) {
        perror("calloc read buffers");
        free(read_header);
        free(read_payload);
        aura_destroy(engine);
        unlink(TEST_FILE);
        return 1;
    }

    /* Create iovec array for scatter read */
    struct iovec read_iov[2];
    read_iov[0].iov_base = read_header;
    read_iov[0].iov_len = HEADER_SIZE;
    read_iov[1].iov_base = read_payload;
    read_iov[1].iov_len = PAYLOAD_SIZE;

    /* Open file for reading */
    int rfd = open(TEST_FILE, O_RDONLY);
    if (rfd < 0) {
        perror("open for read");
        free(read_payload);
        free(read_header);
        aura_destroy(engine);
        unlink(TEST_FILE);
        return 1;
    }

    /* Submit vectored read */
    io_context_t read_ctx = { 0, 0 };
    aura_request_t *read_req =
        aura_readv(engine, rfd, read_iov, 2, 0, 0, read_completion, &read_ctx);
    if (!read_req) {
        perror("aura_readv");
        close(rfd);
        free(read_payload);
        free(read_header);
        aura_destroy(engine);
        unlink(TEST_FILE);
        return 1;
    }

    /* Wait for read completion */
    while (!read_ctx.done) {
        aura_wait(engine, 10);
    }

    printf("  Header (%zu bytes): %s\n", read_iov[0].iov_len, read_header);
    printf("  Payload (%zu bytes): [First 20 bytes: %.20s...]\n", read_iov[1].iov_len,
           read_payload);
    printf("  Total read: %zd bytes\n", read_ctx.result);

    /* Verify data integrity */
    int payload_ok = 1;
    for (size_t i = 0; i < PAYLOAD_SIZE - 1; i++) {
        if (read_payload[i] != 'X') {
            payload_ok = 0;
            break;
        }
    }

    printf("\nData integrity check: %s\n", payload_ok ? "PASSED" : "FAILED");

    /* Cleanup */
    close(rfd);
    free(read_payload);
    free(read_header);
    aura_destroy(engine);
    unlink(TEST_FILE);

    printf("\n======================================\n");
    printf("Vectored I/O Benefits:\n");
    printf("- Single syscall for multiple buffers\n");
    printf("- Natural separation of metadata/data (e.g., database pages)\n");
    printf("- Efficient for structured file formats\n");
    printf("- Reduces syscall overhead vs multiple read/write calls\n");
    printf("- Useful for log files with separate metadata and content sections\n");

    return 0;
}
