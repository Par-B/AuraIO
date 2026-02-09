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
 *   journalctl -t auraio --no-pager -n 10
 *   # or: grep auraio /var/log/syslog
 */

#define _POSIX_C_SOURCE 199309L
#include <auraio.h>
#include "auraio_syslog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_FILE "/tmp/auraio_syslog_test.dat"
#define FILE_SIZE (64 * 1024)
#define BUF_SIZE 4096

static int io_done = 0;

static void on_complete(auraio_request_t *req, ssize_t result, void *ud) {
    (void)req;
    (void)ud;
    if (result < 0) auraio_log_emit(AURAIO_LOG_ERR, "I/O error: %zd", result);
    __sync_add_and_fetch(&io_done, 1);
}

int main(void) {
    printf("AuraIO Syslog Integration Example\n");
    printf("==================================\n\n");

    /* --- Install syslog handler with defaults -------------------------- */
    printf("Installing syslog handler (ident=\"auraio\", facility=LOG_USER)...\n");
    auraio_syslog_install(NULL);

    /* Emit a test message — should appear in syslog */
    auraio_log_emit(AURAIO_LOG_NOTICE, "syslog handler installed");

    /* --- Create engine ------------------------------------------------- */
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    if (!engine) {
        auraio_log_emit(AURAIO_LOG_ERR, "failed to create engine: %s", strerror(errno));
        auraio_syslog_remove();
        return 1;
    }

    auraio_log_emit(AURAIO_LOG_INFO, "engine created (1 ring, depth 64)");

    /* --- Create test file and do I/O ----------------------------------- */
    int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        auraio_log_emit(AURAIO_LOG_ERR, "create test file: %s", strerror(errno));
        auraio_destroy(engine);
        auraio_syslog_remove();
        return 1;
    }

    char data[FILE_SIZE];
    memset(data, 'B', sizeof(data));
    if (write(wfd, data, sizeof(data)) != sizeof(data)) {
        auraio_log_emit(AURAIO_LOG_ERR, "write test file: %s", strerror(errno));
        close(wfd);
        unlink(TEST_FILE);
        auraio_destroy(engine);
        auraio_syslog_remove();
        return 1;
    }
    close(wfd);

    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        auraio_log_emit(AURAIO_LOG_ERR, "open test file: %s", strerror(errno));
        unlink(TEST_FILE);
        auraio_destroy(engine);
        auraio_syslog_remove();
        return 1;
    }

    void *buf = auraio_buffer_alloc(engine, BUF_SIZE);
    if (!buf) {
        auraio_log_emit(AURAIO_LOG_ERR, "buffer alloc failed");
        close(fd);
        unlink(TEST_FILE);
        auraio_destroy(engine);
        auraio_syslog_remove();
        return 1;
    }

    auraio_log_emit(AURAIO_LOG_DEBUG, "submitting read: fd=%d size=%d", fd, BUF_SIZE);

    io_done = 0;
    auraio_request_t *req =
        auraio_read(engine, fd, auraio_buf(buf), BUF_SIZE, 0, on_complete, NULL);
    if (!req) {
        auraio_log_emit(AURAIO_LOG_ERR, "auraio_read failed: %s", strerror(errno));
    } else {
        while (!io_done) auraio_wait(engine, 100);
        auraio_log_emit(AURAIO_LOG_INFO, "read completed successfully");
    }

    /* --- Demonstrate custom ident -------------------------------------- */
    printf("\nRe-installing with custom ident \"my-daemon\"...\n");
    auraio_syslog_remove();

    auraio_syslog_options_t custom = {
        .ident = "my-daemon",
        .facility = 0, /* 0 = use default (LOG_USER) */
        .log_options = 0, /* 0 = use default (LOG_PID | LOG_NDELAY) */
    };
    auraio_syslog_install(&custom);
    auraio_log_emit(AURAIO_LOG_NOTICE, "now logging under ident \"my-daemon\"");

    /* --- Clean up ------------------------------------------------------ */
    auraio_buffer_free(engine, buf, BUF_SIZE);
    close(fd);
    unlink(TEST_FILE);

    auraio_log_emit(AURAIO_LOG_NOTICE, "shutting down");
    auraio_destroy(engine);
    auraio_syslog_remove();

    printf("\nDone. Check syslog for messages:\n");
    printf("  journalctl -t auraio --no-pager -n 10\n");
    printf("  journalctl -t my-daemon --no-pager -n 5\n");

    return 0;
}
