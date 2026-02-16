/**
 * @file bulk_reader.c
 * @brief High-throughput bulk file reader example
 *
 * Demonstrates reading many files concurrently with adaptive tuning.
 *
 * Usage: ./bulk_reader <directory>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <limits.h>

#include <aura.h>

#define MAX_FILES 10000
#define READ_SIZE (256 * 1024) /* 256KB per file */
#define STATS_INTERVAL 1000 /* Print stats every N completions */

/* Context for tracking bulk read progress */
typedef struct {
    aura_engine_t *engine;
    int files_pending;
    int files_completed;
    long long bytes_read;
    long long errors;
    struct timeval start_time;
} bulk_ctx_t;

/* Per-file context for cleanup in callback */
typedef struct {
    bulk_ctx_t *bulk;
    int fd;
    void *buf;
} file_ctx_t;

/**
 * Callback invoked when a file read completes.
 *
 * Thread-safety: aura invokes callbacks sequentially from the polling
 * thread (aura_wait/aura_run), so no synchronization is needed for
 * ctx->bytes_read and other bulk_ctx fields.
 */
void on_file_read(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    file_ctx_t *fctx = user_data;
    bulk_ctx_t *ctx = fctx->bulk;

    if (result > 0) {
        ctx->bytes_read += result;
    } else if (result < 0) {
        ctx->errors++;
    }

    /* Clean up per-file resources */
    close(fctx->fd);
    aura_buffer_free(ctx->engine, fctx->buf);
    free(fctx);

    ctx->files_completed++;
    ctx->files_pending--;

    /* Print periodic stats */
    if (ctx->files_completed % STATS_INTERVAL == 0) {
        struct timeval now;
        gettimeofday(&now, NULL);
        double elapsed =
            (now.tv_sec - ctx->start_time.tv_sec) + (now.tv_usec - ctx->start_time.tv_usec) / 1e6;
        double mb_per_sec = (ctx->bytes_read / (1024.0 * 1024.0)) / elapsed;

        printf("\rProgress: %d files, %.2f MB, %.2f MB/s", ctx->files_completed,
               ctx->bytes_read / (1024.0 * 1024.0), mb_per_sec);
        fflush(stdout);
    }

    /* Stop when all files done */
    if (ctx->files_pending == 0) {
        aura_stop(ctx->engine);
    }
}

/**
 * Check if a file is a regular file.
 */
static int is_regular_file(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        fprintf(stderr, "\nReads all regular files in a directory using async I/O.\n");
        fprintf(stderr, "The engine self-tunes for optimal throughput.\n");
        return 1;
    }

    const char *dirname = argv[1];

    /* Create the aura engine */
    printf("Creating async I/O engine...\n");
    aura_engine_t *engine = aura_create();
    if (!engine) {
        fprintf(stderr, "Failed to create aura engine: %s\n", strerror(errno));
        return 1;
    }

    /* Initialize context */
    bulk_ctx_t ctx = {
        .engine = engine, .files_pending = 0, .files_completed = 0, .bytes_read = 0, .errors = 0
    };
    gettimeofday(&ctx.start_time, NULL);

    /* Open directory */
    DIR *dir = opendir(dirname);
    if (!dir) {
        fprintf(stderr, "Failed to open directory '%s': %s\n", dirname, strerror(errno));
        aura_destroy(engine);
        return 1;
    }

    printf("Scanning directory '%s'...\n", dirname);

    /* Scan directory and submit reads */
    struct dirent *ent;
    int submitted = 0;

    while ((ent = readdir(dir)) != NULL && submitted < MAX_FILES) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        /* Build full path */
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dirname, ent->d_name);

        /* Skip non-regular files */
        if (!is_regular_file(path)) {
            continue;
        }

        /* Open file with O_DIRECT for best performance */
        int fd = open(path, O_RDONLY | O_DIRECT);
        if (fd < 0) {
            /* Try without O_DIRECT */
            fd = open(path, O_RDONLY);
            if (fd < 0) {
                continue; /* Skip files we can't open */
            }
        }

        /* Allocate buffer */
        void *buf = aura_buffer_alloc(engine, READ_SIZE);
        if (!buf) {
            close(fd);
            continue;
        }

        /* Allocate per-file context for cleanup in callback */
        file_ctx_t *fctx = malloc(sizeof(file_ctx_t));
        if (!fctx) {
            aura_buffer_free(engine, buf);
            close(fd);
            continue;
        }
        fctx->bulk = &ctx;
        fctx->fd = fd;
        fctx->buf = buf;

        /* Submit async read */
        if (aura_read(engine, fd, aura_buf(buf), READ_SIZE, 0, on_file_read, fctx) != NULL) {
            ctx.files_pending++;
            submitted++;
        } else {
            free(fctx);
            aura_buffer_free(engine, buf);
            close(fd);
        }
    }

    closedir(dir);

    if (submitted == 0) {
        printf("No files found to read.\n");
        aura_destroy(engine);
        return 1;
    }

    printf("Submitted %d file reads\n", submitted);
    printf("Running event loop (self-tuning in progress)...\n\n");

    /* Process until all complete */
    aura_run(engine);

    /* Final stats */
    printf("\n\n");
    printf("=== Final Results ===\n");

    struct timeval end_time;
    gettimeofday(&end_time, NULL);
    double total_elapsed = (end_time.tv_sec - ctx.start_time.tv_sec) +
                           (end_time.tv_usec - ctx.start_time.tv_usec) / 1e6;

    printf("Files read:       %d\n", ctx.files_completed);
    printf("Total bytes:      %.2f MB\n", ctx.bytes_read / (1024.0 * 1024.0));
    printf("Errors:           %lld\n", ctx.errors);
    printf("Elapsed time:     %.2f seconds\n", total_elapsed);
    printf("Average speed:    %.2f MB/s\n", (ctx.bytes_read / (1024.0 * 1024.0)) / total_elapsed);

    /* Engine tuning results */
    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));
    printf("\nAdaptive tuning results:\n");
    printf("  Optimal in-flight: %d\n", stats.optimal_in_flight);
    printf("  Optimal batch:     %d\n", stats.optimal_batch_size);
    printf("  P99 latency:       %.2f ms\n", stats.p99_latency_ms);

    /* Cleanup */
    aura_destroy(engine);

    printf("\nDone!\n");
    return 0;
}
