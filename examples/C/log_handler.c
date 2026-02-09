/**
 * @file log_handler.c
 * @brief Demonstrate AuraIO custom log handler
 *
 * Shows how to install a custom log callback that formats library
 * messages with timestamps and severity levels, and how to emit
 * application-level messages through the same pipeline using
 * auraio_log_emit().
 *
 * Build: make examples
 * Run:   ./examples/C/log_handler
 */

#define _POSIX_C_SOURCE 200112L
#include <auraio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

#define TEST_FILE "/tmp/auraio_log_test.dat"
#define FILE_SIZE (64 * 1024) /* 64 KB */
#define BUF_SIZE 4096

/* ---------------------------------------------------------------------------
 * Log context: passed as userdata to the log callback
 * ---------------------------------------------------------------------------
 * Applications typically wrap this in their own logging framework.
 * Here we show a minimal but practical setup with:
 *   - Configurable output stream (stderr, a file, etc.)
 *   - Application name prefix
 *   - Minimum severity filter
 * --------------------------------------------------------------------------- */

typedef struct {
    FILE *output; /* Where to write (stderr, fopen'd file, etc.) */
    const char *prefix; /* Application name prepended to each line */
    int min_level; /* Maximum level to emit (lower = more severe) */
} log_context_t;

static const char *level_name(int level) {
    switch (level) {
    case AURAIO_LOG_ERR:
        return "ERR";
    case AURAIO_LOG_WARN:
        return "WARN";
    case AURAIO_LOG_NOTICE:
        return "NOTICE";
    case AURAIO_LOG_INFO:
        return "INFO";
    case AURAIO_LOG_DEBUG:
        return "DEBUG";
    default:
        return "???";
    }
}

/**
 * Custom log handler — formats messages with local-time timestamps.
 *
 * Output format:
 *   2025-02-08 14:30:05.123 [myapp] ERR: some error message
 */
static void my_log_handler(int level, const char *msg, void *userdata) {
    log_context_t *ctx = userdata;

    /* Filter by severity (lower number = more severe) */
    if (level > ctx->min_level) return;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    localtime_r(&ts.tv_sec, &tm);

    fprintf(ctx->output, "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s] %s: %s\n", tm.tm_year + 1900,
            tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
            ctx->prefix, level_name(level), msg);
}

/* Simple I/O completion tracking */
static atomic_int io_completed = 0;

static void on_complete(auraio_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    (void)user_data;
    if (result < 0) fprintf(stderr, "  I/O error: %zd\n", result);
    io_completed++;
}

int main(void) {
    printf("AuraIO Log Handler Example\n");
    printf("==========================\n\n");

    /* --- Step 1: Set up log context ------------------------------------ */
    log_context_t log_ctx = {
        .output = stderr,
        .prefix = "myapp",
        .min_level = AURAIO_LOG_DEBUG, /* Show all levels */
    };

    /* Install handler BEFORE creating the engine so we capture any
     * startup diagnostics. */
    auraio_set_log_handler(my_log_handler, &log_ctx);

    /* --- Step 2: Emit application-level messages ----------------------- */
    auraio_log_emit(AURAIO_LOG_INFO, "log handler installed, creating engine");

    /* --- Step 3: Create engine and do I/O ------------------------------ */
    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    if (!engine) {
        auraio_log_emit(AURAIO_LOG_ERR, "failed to create engine: %s", strerror(errno));
        auraio_set_log_handler(NULL, NULL);
        return 1;
    }

    auraio_log_emit(AURAIO_LOG_NOTICE, "engine created (1 ring, depth 64)");

    /* Create test file */
    int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) {
        perror("create test file");
        auraio_destroy(engine);
        auraio_set_log_handler(NULL, NULL);
        return 1;
    }

    char data[FILE_SIZE];
    memset(data, 'A', sizeof(data));
    if (write(wfd, data, sizeof(data)) != sizeof(data)) {
        perror("write test file");
        close(wfd);
        unlink(TEST_FILE);
        auraio_destroy(engine);
        auraio_set_log_handler(NULL, NULL);
        return 1;
    }
    close(wfd);

    /* Read the file back using AuraIO */
    int fd = open(TEST_FILE, O_RDONLY);
    if (fd < 0) {
        perror("open test file");
        unlink(TEST_FILE);
        auraio_destroy(engine);
        auraio_set_log_handler(NULL, NULL);
        return 1;
    }

    void *buf = auraio_buffer_alloc(engine, BUF_SIZE);
    if (!buf) {
        perror("auraio_buffer_alloc");
        close(fd);
        unlink(TEST_FILE);
        auraio_destroy(engine);
        auraio_set_log_handler(NULL, NULL);
        return 1;
    }

    auraio_log_emit(AURAIO_LOG_DEBUG, "submitting read: fd=%d offset=0 size=%d", fd, BUF_SIZE);

    io_completed = 0;
    auraio_request_t *req =
        auraio_read(engine, fd, auraio_buf(buf), BUF_SIZE, 0, on_complete, NULL);
    if (!req) {
        auraio_log_emit(AURAIO_LOG_ERR, "auraio_read failed: %s", strerror(errno));
    } else {
        while (!io_completed) auraio_wait(engine, 100);
        auraio_log_emit(AURAIO_LOG_INFO, "read completed successfully");
    }

    /* --- Step 4: Show stats -------------------------------------------- */
    auraio_stats_t stats;
    auraio_get_stats(engine, &stats);
    printf("\nEngine stats:\n");
    printf("  Operations completed: %lld\n", (long long)stats.ops_completed);
    printf("  P99 latency: %.3f ms\n", stats.p99_latency_ms);

    /* --- Step 5: Clean up ---------------------------------------------- */
    auraio_buffer_free(engine, buf, BUF_SIZE);
    close(fd);
    unlink(TEST_FILE);

    auraio_log_emit(AURAIO_LOG_NOTICE, "shutting down");

    /* Destroy engine while handler is still installed so we capture any
     * shutdown diagnostics (e.g. timeout with pending ops). */
    auraio_destroy(engine);

    /* Handler no longer needed after engine is gone. */
    auraio_set_log_handler(NULL, NULL);

    printf("\n--- Summary ---\n");
    printf("The log handler captured all library and application messages\n");
    printf("on stderr with timestamps, severity levels, and an app prefix.\n");
    printf("In production, replace my_log_handler() with your framework's\n");
    printf("logging function (syslog, log4c, journald, etc.).\n");

    return 0;
}
