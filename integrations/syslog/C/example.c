// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file example.c
 * @brief Example: AuraIO syslog integration
 *
 * Demonstrates forwarding AuraIO log messages to syslog.
 * Both library-internal diagnostics and application-emitted messages
 * are captured via the same syslog handler.
 *
 * Build:
 *   make integrations
 *
 * Run:
 *   ./integrations/syslog/C/syslog_example
 *
 * Verify syslog output:
 *   journalctl -t aura --no-pager -n 10
 *   # or: grep aura /var/log/syslog
 */

#define _POSIX_C_SOURCE 200112L
#include <aura.h>
#include "aura_syslog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_FILE "/tmp/aura_syslog_test.dat"
#define FILE_SIZE (64 * 1024)
#define BUF_SIZE 4096

static atomic_int io_done = 0;

static void on_complete(aura_request_t *req, ssize_t result, void *ud) {
    (void)req;
    (void)ud;
    if (result < 0) aura_log_emit(AURA_LOG_ERR, "I/O error: %zd", result);
    io_done++;
}

int main(void) {
    printf("AuraIO Syslog Integration Example\n");
    printf("==================================\n\n");

    /* --- Install syslog handler with defaults -------------------------- */
    printf("Installing syslog handler (ident=\"aura\", facility=LOG_USER)...\n");
    aura_syslog_install(NULL);

    /* Emit a test message — should appear in syslog */
    aura_log_emit(AURA_LOG_NOTICE, "syslog handler installed");

    /* --- Create engine ------------------------------------------------- */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        aura_log_emit(AURA_LOG_ERR, "failed to create engine: %s", strerror(errno));
        aura_syslog_remove();
        return 1;
    }

    aura_log_emit(AURA_LOG_INFO, "engine created (1 ring, depth 64)");

    /* --- Create test file and do I/O ----------------------------------- */
    int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        aura_log_emit(AURA_LOG_ERR, "create test file: %s", strerror(errno));
        aura_destroy(engine);
        aura_syslog_remove();
        return 1;
    }

    char data[FILE_SIZE];
    memset(data, 'B', sizeof(data));
    if (write(wfd, data, sizeof(data)) != sizeof(data)) {
        aura_log_emit(AURA_LOG_ERR, "write test file: %s", strerror(errno));
        close(wfd);
        unlink(TEST_FILE);
        aura_destroy(engine);
        aura_syslog_remove();
        return 1;
    }
    close(wfd);

    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        aura_log_emit(AURA_LOG_ERR, "open test file: %s", strerror(errno));
        unlink(TEST_FILE);
        aura_destroy(engine);
        aura_syslog_remove();
        return 1;
    }

    void *buf = aura_buffer_alloc(engine, BUF_SIZE);
    if (!buf) {
        aura_log_emit(AURA_LOG_ERR, "buffer alloc failed");
        close(fd);
        unlink(TEST_FILE);
        aura_destroy(engine);
        aura_syslog_remove();
        return 1;
    }

    aura_log_emit(AURA_LOG_DEBUG, "submitting read: fd=%d size=%d", fd, BUF_SIZE);

    io_done = 0;
    aura_request_t *req = aura_read(engine, fd, aura_buf(buf), BUF_SIZE, 0, 0, on_complete, NULL);
    if (!req) {
        aura_log_emit(AURA_LOG_ERR, "aura_read failed: %s", strerror(errno));
    } else {
        while (!io_done) {
            int rc = aura_wait(engine, 100);
            (void)rc;
        }
        aura_log_emit(AURA_LOG_INFO, "read completed successfully");
    }

    /* --- Demonstrate custom ident -------------------------------------- */
    printf("\nRe-installing with custom ident \"my-daemon\"...\n");
    aura_syslog_remove();

    aura_syslog_options_t custom = {
        .ident = "my-daemon",
        .facility = -1, /* -1 = use default (LOG_USER) */
        .log_options = -1, /* -1 = use default (LOG_PID | LOG_NDELAY) */
    };
    aura_syslog_install(&custom);
    aura_log_emit(AURA_LOG_NOTICE, "now logging under ident \"my-daemon\"");

    /* --- Clean up ------------------------------------------------------ */
    aura_buffer_free(engine, buf);
    close(fd);
    unlink(TEST_FILE);

    aura_log_emit(AURA_LOG_NOTICE, "shutting down");
    aura_destroy(engine);
    aura_syslog_remove();

    printf("\nDone. Check syslog for messages:\n");
    printf("  journalctl -t aura --no-pager -n 10\n");
    printf("  journalctl -t my-daemon --no-pager -n 5\n");

    return 0;
}
