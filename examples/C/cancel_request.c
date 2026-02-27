// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file cancel_request.c
 * @brief Demonstrates cancelling an in-flight I/O operation
 *
 * Shows how to use aura_cancel() to abort a pending read.
 * The cancelled operation's callback receives result = -ECANCELED.
 *
 * Usage: ./cancel_request <file>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <aura.h>

#define READ_SIZE 4096

static int completed = 0;
static ssize_t read_result = 0;

static void on_read(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    read_result = result;
    completed = 1;

    if (result == -ECANCELED) {
        printf("Read was cancelled (result = -ECANCELED)\n");
    } else if (result < 0) {
        printf("Read failed: %s\n", strerror(-(int)result));
    } else {
        printf("Read completed: %zd bytes\n", result);
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);
        return 1;
    }

    aura_engine_t *engine = aura_create();
    if (!engine) {
        fprintf(stderr, "Failed to create engine: %s\n", strerror(errno));
        return 1;
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open '%s': %s\n", argv[1], strerror(errno));
        aura_destroy(engine);
        return 1;
    }

    void *buf = aura_buffer_alloc(engine, READ_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        close(fd);
        aura_destroy(engine);
        return 1;
    }

    /* Submit an async read */
    printf("Submitting async read...\n");
    aura_request_t *req = aura_read(engine, fd, aura_buf(buf), READ_SIZE, 0, 0, on_read, NULL);
    if (!req) {
        fprintf(stderr, "Failed to submit read: %s\n", strerror(errno));
        aura_buffer_free(engine, buf);
        close(fd);
        aura_destroy(engine);
        return 1;
    }

    /* Attempt to cancel the request.
     * Note: cancellation is best-effort. If the I/O already completed
     * by the time cancel is processed, the original result is returned
     * instead of -ECANCELED. */
    printf("Attempting to cancel...\n");
    int cancel_ret = aura_cancel(engine, req);
    if (cancel_ret == 0) {
        printf("Cancel request submitted successfully\n");
    } else {
        printf("Cancel submission failed (operation may have already completed)\n");
    }

    /* Wait for the completion callback */
    while (!completed) {
        aura_wait(engine, 100);
    }

    printf("Final result: %zd (%s)\n", read_result,
           read_result == -ECANCELED ? "cancelled"
           : read_result < 0         ? strerror(-(int)read_result)
                                     : "success");

    aura_buffer_free(engine, buf);
    close(fd);
    aura_destroy(engine);
    return 0;
}
