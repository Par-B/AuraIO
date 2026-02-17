/**
 * @file auracp.c
 * @brief auracp - async pipelined file copy powered by AuraIO
 *
 * A production-quality cp replacement that uses AuraIO's pipelined async I/O
 * and AIMD adaptive tuning for high-throughput file copying. Supports single
 * file, multi-file, and recursive directory copy with cross-file pipelining.
 *
 * Usage: auracp [OPTIONS] SOURCE... DEST
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <ftw.h>
#include <libgen.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <aura.h>

// ============================================================================
// Constants
// ============================================================================

#define DEFAULT_CHUNK_SIZE  (256 * 1024)   /* 256 KiB */
#define DEFAULT_PIPELINE    8
#define MAX_PIPELINE        64
#define PROGRESS_INTERVAL_MS 200
#define SECTOR_SIZE         512
#define NFTW_MAX_FDS        64

// ============================================================================
// Configuration
// ============================================================================

typedef struct {
    const char **sources;
    int num_sources;
    const char *dest;
    size_t chunk_size;
    int pipeline_depth;
    bool recursive;
    bool use_direct;
    bool quiet;
    bool no_progress;
    bool no_fsync;
    bool preserve;
    bool verbose;
    bool keep_partial;
} config_t;

// ============================================================================
// File task queue
// ============================================================================

typedef struct file_task {
    char *src_path;
    char *dst_path;
    off_t file_size;
    mode_t mode;
    int src_fd;
    int dst_fd;
    off_t read_offset;
    off_t bytes_written;
    int active_ops;
    bool reads_done;
    bool done;
    struct file_task *next;
} file_task_t;

typedef struct {
    file_task_t *head;
    file_task_t *tail;
    file_task_t *current;   /* next task needing reads */
    int total_files;
    int completed_files;
    off_t total_bytes;
    off_t total_written;
} task_queue_t;

// ============================================================================
// Pipeline buffer slots
// ============================================================================

typedef enum {
    BUF_FREE = 0,
    BUF_READING,
    BUF_WRITING
} buf_state_t;

struct copy_ctx;

typedef struct {
    void *buf;
    off_t offset;
    size_t bytes;
    buf_state_t state;
    file_task_t *task;
    struct copy_ctx *ctx;
} buf_slot_t;

typedef struct copy_ctx {
    aura_engine_t *engine;
    const config_t *config;
    task_queue_t *queue;
    buf_slot_t *slots;
    int active_ops;
    int error;
} copy_ctx_t;

// ============================================================================
// Global state for signal handling
// ============================================================================

static volatile sig_atomic_t g_interrupted = 0;
static struct timespec g_start_time;
static uint64_t g_last_progress_ns = 0;

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

// ============================================================================
// Size parsing
// ============================================================================

static ssize_t parse_size(const char *str) {
    char *endp;
    double val = strtod(str, &endp);
    if (endp == str || val < 0) return -1;

    switch (*endp) {
    case 'G': case 'g': val *= 1024.0 * 1024.0 * 1024.0; break;
    case 'M': case 'm': val *= 1024.0 * 1024.0; break;
    case 'K': case 'k': val *= 1024.0; break;
    case '\0': break;
    default: return -1;
    }

    if (val > (double)SSIZE_MAX) return -1;
    return (ssize_t)val;
}

// ============================================================================
// Formatting helpers
// ============================================================================

static void format_bytes(char *buf, size_t bufsz, double bytes) {
    if (bytes >= 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f MiB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024.0)
        snprintf(buf, bufsz, "%.1f KiB", bytes / 1024.0);
    else
        snprintf(buf, bufsz, "%.0f B", bytes);
}

static void format_rate(char *buf, size_t bufsz, double bps) {
    if (bps >= 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f GiB/s", bps / (1024.0 * 1024.0 * 1024.0));
    else if (bps >= 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f MiB/s", bps / (1024.0 * 1024.0));
    else if (bps >= 1024.0)
        snprintf(buf, bufsz, "%.1f KiB/s", bps / 1024.0);
    else
        snprintf(buf, bufsz, "%.0f B/s", bps);
}

// ============================================================================
// Progress display
// ============================================================================

static void progress_update(const copy_ctx_t *ctx, bool final) {
    if (ctx->config->quiet || ctx->config->no_progress) return;
    if (!final && !isatty(STDERR_FILENO)) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;

    if (!final && (now_ns - g_last_progress_ns) < PROGRESS_INTERVAL_MS * 1000000ULL)
        return;
    g_last_progress_ns = now_ns;

    double elapsed = (double)(now.tv_sec - g_start_time.tv_sec)
                   + (double)(now.tv_nsec - g_start_time.tv_nsec) / 1e9;
    double done = (double)ctx->queue->total_written;
    double total = (double)ctx->queue->total_bytes;
    double pct = (total > 0) ? done / total * 100.0 : 100.0;
    double rate = (elapsed > 0.01) ? done / elapsed : 0.0;

    char done_str[32], total_str[32], rate_str[32];
    format_bytes(done_str, sizeof(done_str), done);
    format_bytes(total_str, sizeof(total_str), total);
    format_rate(rate_str, sizeof(rate_str), rate);

    /* Progress bar */
    int bar_width = 30;
    int filled = (total > 0) ? (int)(pct / 100.0 * bar_width) : bar_width;
    if (filled > bar_width) filled = bar_width;

    char bar[64];
    int i;
    for (i = 0; i < filled && i < bar_width; i++) bar[i] = '=';
    if (filled < bar_width) {
        bar[filled] = '>';
        for (i = filled + 1; i < bar_width; i++) bar[i] = ' ';
    }
    bar[bar_width] = '\0';

    /* ETA */
    char eta[32] = "";
    if (rate > 0 && total > done) {
        double remaining = (total - done) / rate;
        int mins = (int)(remaining / 60.0);
        int secs = (int)remaining % 60;
        snprintf(eta, sizeof(eta), "ETA %d:%02d", mins, secs);
    }

    fprintf(stderr, "\r  %s / %s  [%s]  %3.0f%%  %s  %s   ",
            done_str, total_str, bar, pct, rate_str, eta);

    if (final) fprintf(stderr, "\n");
}

// ============================================================================
// Task queue management
// ============================================================================

static void queue_init(task_queue_t *q) {
    memset(q, 0, sizeof(*q));
}

static void queue_push(task_queue_t *q, file_task_t *task) {
    task->next = NULL;
    if (q->tail)
        q->tail->next = task;
    else
        q->head = task;
    q->tail = task;
    if (!q->current) q->current = task;
    q->total_files++;
    q->total_bytes += task->file_size;
}

static void queue_free(task_queue_t *q) {
    file_task_t *t = q->head;
    while (t) {
        file_task_t *next = t->next;
        free(t->src_path);
        free(t->dst_path);
        if (t->src_fd >= 0) close(t->src_fd);
        if (t->dst_fd >= 0) close(t->dst_fd);
        free(t);
        t = next;
    }
    memset(q, 0, sizeof(*q));
}

// ============================================================================
// Path helpers
// ============================================================================

static char *path_join(const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    /* Trim trailing slash from dir */
    while (dlen > 1 && dir[dlen - 1] == '/') dlen--;
    char *out = malloc(dlen + 1 + nlen + 1);
    if (!out) return NULL;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen + 1);
    return out;
}

/* Given src root, dst root, and a full src path, compute dst path.
 * E.g., src_root="/tmp/src", dst_root="/tmp/dst", fpath="/tmp/src/a/b.txt"
 * → "/tmp/dst/a/b.txt" */
static char *remap_path(const char *src_root, const char *dst_root,
                        const char *fpath) {
    size_t rootlen = strlen(src_root);
    /* Skip trailing slashes on src_root */
    while (rootlen > 1 && src_root[rootlen - 1] == '/') rootlen--;
    const char *suffix = fpath + rootlen;
    if (*suffix == '/') suffix++;

    size_t dlen = strlen(dst_root);
    while (dlen > 1 && dst_root[dlen - 1] == '/') dlen--;

    size_t slen = strlen(suffix);
    char *out = malloc(dlen + 1 + slen + 1);
    if (!out) return NULL;
    memcpy(out, dst_root, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, suffix, slen + 1);
    return out;
}

// ============================================================================
// Task list building
// ============================================================================

/* Context for nftw callback */
typedef struct {
    const char *src_root;
    const char *dst_root;
    task_queue_t *queue;
    const config_t *config;
    int errors;
} walk_ctx_t;

static walk_ctx_t *g_walk_ctx; /* nftw doesn't support user_data */

static int nftw_callback(const char *fpath, const struct stat *sb,
                         int typeflag, struct FTW *ftwbuf) {
    (void)ftwbuf;
    walk_ctx_t *wc = g_walk_ctx;

    if (typeflag == FTW_D) {
        /* Create corresponding directory in destination */
        char *dst = remap_path(wc->src_root, wc->dst_root, fpath);
        if (!dst) { wc->errors++; return 0; }

        /* Skip the root destination dir itself (created by caller or existing) */
        if (strcmp(fpath, wc->src_root) == 0) {
            struct stat dst_st;
            if (stat(dst, &dst_st) != 0) {
                if (mkdir(dst, sb->st_mode & 07777) != 0 && errno != EEXIST) {
                    fprintf(stderr, "auracp: cannot create directory '%s': %s\n",
                            dst, strerror(errno));
                    wc->errors++;
                }
            }
        } else {
            if (mkdir(dst, sb->st_mode & 07777) != 0 && errno != EEXIST) {
                fprintf(stderr, "auracp: cannot create directory '%s': %s\n",
                        dst, strerror(errno));
                wc->errors++;
            }
        }
        free(dst);
        return 0;
    }

    if (typeflag == FTW_F) {
        char *dst = remap_path(wc->src_root, wc->dst_root, fpath);
        if (!dst) { wc->errors++; return 0; }

        file_task_t *task = calloc(1, sizeof(*task));
        if (!task) { free(dst); wc->errors++; return 0; }
        task->src_path = strdup(fpath);
        task->dst_path = dst;
        task->file_size = sb->st_size;
        task->mode = sb->st_mode;
        task->src_fd = -1;
        task->dst_fd = -1;
        queue_push(wc->queue, task);
        return 0;
    }

    /* Skip special files (symlinks to non-regular, devices, etc.) */
    if (typeflag != FTW_SL) {
        if (!wc->config->quiet)
            fprintf(stderr, "auracp: skipping special file: %s\n", fpath);
    }
    return 0;
}

static int build_recursive(const char *src, const char *dst,
                           task_queue_t *queue, const config_t *config) {
    walk_ctx_t wc = {
        .src_root = src,
        .dst_root = dst,
        .queue = queue,
        .config = config,
        .errors = 0,
    };
    g_walk_ctx = &wc;

    if (nftw(src, nftw_callback, NFTW_MAX_FDS, 0) != 0) {
        fprintf(stderr, "auracp: cannot walk '%s': %s\n", src, strerror(errno));
        return -1;
    }
    return wc.errors ? -1 : 0;
}

static int build_task_list(const config_t *config, task_queue_t *queue) {
    queue_init(queue);

    struct stat dst_st;
    bool dst_is_dir = (stat(config->dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode));

    /* Multi-source requires directory destination */
    if (config->num_sources > 1 && !dst_is_dir) {
        fprintf(stderr, "auracp: target '%s' is not a directory\n", config->dest);
        return -1;
    }

    for (int i = 0; i < config->num_sources; i++) {
        const char *src = config->sources[i];
        struct stat src_st;

        if (stat(src, &src_st) != 0) {
            fprintf(stderr, "auracp: cannot stat '%s': %s\n", src, strerror(errno));
            return -1;
        }

        if (S_ISDIR(src_st.st_mode)) {
            if (!config->recursive) {
                fprintf(stderr, "auracp: -r not specified; omitting directory '%s'\n", src);
                return -1;
            }
            /* Recursive: walk source tree into dest */
            char *dst_dir;
            if (dst_is_dir) {
                /* auracp -r src/ existing_dir/ → existing_dir/src/... */
                char *src_copy = strdup(src);
                char *base = basename(src_copy);
                dst_dir = path_join(config->dest, base);
                free(src_copy);
            } else {
                dst_dir = strdup(config->dest);
            }
            if (!dst_dir) return -1;

            /* Create top-level destination directory */
            if (mkdir(dst_dir, src_st.st_mode & 07777) != 0 && errno != EEXIST) {
                fprintf(stderr, "auracp: cannot create directory '%s': %s\n",
                        dst_dir, strerror(errno));
                free(dst_dir);
                return -1;
            }

            int rc = build_recursive(src, dst_dir, queue, config);
            free(dst_dir);
            if (rc != 0) return -1;
        } else if (S_ISREG(src_st.st_mode)) {
            char *dst_path;
            if (dst_is_dir) {
                char *src_copy = strdup(src);
                char *base = basename(src_copy);
                dst_path = path_join(config->dest, base);
                free(src_copy);
            } else {
                dst_path = strdup(config->dest);
            }
            if (!dst_path) return -1;

            /* Check same file */
            if (dst_is_dir) {
                struct stat check_st;
                if (stat(dst_path, &check_st) == 0 &&
                    src_st.st_dev == check_st.st_dev &&
                    src_st.st_ino == check_st.st_ino) {
                    fprintf(stderr, "auracp: '%s' and '%s' are the same file\n",
                            src, dst_path);
                    free(dst_path);
                    return -1;
                }
            }

            file_task_t *task = calloc(1, sizeof(*task));
            if (!task) { free(dst_path); return -1; }
            task->src_path = strdup(src);
            task->dst_path = dst_path;
            task->file_size = src_st.st_size;
            task->mode = src_st.st_mode;
            task->src_fd = -1;
            task->dst_fd = -1;
            queue_push(queue, task);
        } else {
            fprintf(stderr, "auracp: skipping special file: %s\n", src);
        }
    }

    if (queue->total_files == 0) {
        fprintf(stderr, "auracp: no files to copy\n");
        return -1;
    }
    return 0;
}

// ============================================================================
// I/O callbacks
// ============================================================================

static void on_write_complete(aura_request_t *req, ssize_t result, void *user_data);
static void finish_task(copy_ctx_t *ctx, file_task_t *task);

static void on_read_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    buf_slot_t *slot = (buf_slot_t *)user_data;
    copy_ctx_t *ctx = slot->ctx;
    file_task_t *task = slot->task;

    if (result <= 0) {
        if (result < 0 && ctx->error == 0) {
            fprintf(stderr, "auracp: read error on '%s': %s\n",
                    task->src_path, strerror(-(int)result));
            ctx->error = (int)result;
        }
        slot->state = BUF_FREE;
        slot->task = NULL;
        ctx->active_ops--;
        task->active_ops--;
        return;
    }

    /* Submit write for the data we just read */
    slot->bytes = (size_t)result;
    slot->state = BUF_WRITING;

    aura_request_t *wreq = aura_write(ctx->engine, task->dst_fd,
                                       aura_buf(slot->buf), slot->bytes,
                                       slot->offset, on_write_complete, slot);
    if (!wreq) {
        if (ctx->error == 0) {
            fprintf(stderr, "auracp: write submit failed on '%s': %s\n",
                    task->dst_path, strerror(errno));
            ctx->error = -errno;
        }
        slot->state = BUF_FREE;
        slot->task = NULL;
        ctx->active_ops--;
        task->active_ops--;
    }
}

static void on_fsync_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    file_task_t *task = (file_task_t *)user_data;
    (void)result; /* fsync failure is non-fatal for pipeline flow */

    /* Close both fds */
    close(task->src_fd);
    close(task->dst_fd);
    task->src_fd = -1;
    task->dst_fd = -1;
    task->done = true;
}

static void finish_task(copy_ctx_t *ctx, file_task_t *task) {
    ctx->queue->completed_files++;

    /* Preserve timestamps if requested */
    if (ctx->config->preserve && task->dst_fd >= 0) {
        struct stat src_st;
        if (fstat(task->src_fd, &src_st) == 0) {
            struct timespec times[2] = { src_st.st_atim, src_st.st_mtim };
            futimens(task->dst_fd, times);
        }
    }

    /* Fsync if requested, then close */
    if (!ctx->config->no_fsync && task->dst_fd >= 0) {
        aura_request_t *freq = aura_fsync(ctx->engine, task->dst_fd,
                                           AURA_FSYNC_DATASYNC,
                                           on_fsync_complete, task);
        if (!freq) {
            /* Fallback: sync and close synchronously */
            fdatasync(task->dst_fd);
            close(task->src_fd);
            close(task->dst_fd);
            task->src_fd = -1;
            task->dst_fd = -1;
            task->done = true;
        }
        /* else: on_fsync_complete will close fds */
    } else {
        close(task->src_fd);
        close(task->dst_fd);
        task->src_fd = -1;
        task->dst_fd = -1;
        task->done = true;
    }
}

static void on_write_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    buf_slot_t *slot = (buf_slot_t *)user_data;
    copy_ctx_t *ctx = slot->ctx;
    file_task_t *task = slot->task;

    if (result < 0) {
        if (ctx->error == 0) {
            fprintf(stderr, "auracp: write error on '%s': %s\n",
                    task->dst_path, strerror(-(int)result));
            ctx->error = (int)result;
        }
    } else if ((size_t)result != slot->bytes) {
        if (ctx->error == 0) {
            fprintf(stderr, "auracp: short write on '%s'\n", task->dst_path);
            ctx->error = -EIO;
        }
    } else {
        task->bytes_written += result;
        ctx->queue->total_written += result;
    }

    slot->state = BUF_FREE;
    slot->task = NULL;
    ctx->active_ops--;
    task->active_ops--;

    /* Check if this file is fully done */
    if (task->reads_done && task->active_ops == 0 && !task->done) {
        finish_task(ctx, task);
    }
}

// ============================================================================
// Pipeline core
// ============================================================================

static int open_task(file_task_t *task, const config_t *config) {
    int src_flags = O_RDONLY;
    if (config->use_direct) src_flags |= O_DIRECT;

    task->src_fd = open(task->src_path, src_flags);
    if (task->src_fd < 0) {
        fprintf(stderr, "auracp: cannot open '%s': %s\n",
                task->src_path, strerror(errno));
        return -1;
    }

    int dst_flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (config->use_direct) dst_flags |= O_DIRECT;

    task->dst_fd = open(task->dst_path, dst_flags, task->mode & 07777);
    if (task->dst_fd < 0) {
        fprintf(stderr, "auracp: cannot create '%s': %s\n",
                task->dst_path, strerror(errno));
        close(task->src_fd);
        task->src_fd = -1;
        return -1;
    }

    /* Pre-allocate destination (best-effort) */
    if (task->file_size > 0)
        posix_fallocate(task->dst_fd, 0, task->file_size);

    /* Copy permissions */
    fchmod(task->dst_fd, task->mode & 07777);

    return 0;
}

static void submit_next_read(copy_ctx_t *ctx, buf_slot_t *slot) {
    task_queue_t *q = ctx->queue;

    /* Find a task that needs reads */
    file_task_t *task = q->current;
    while (task && (task->reads_done || task->done)) {
        task = task->next;
    }
    if (!task) return; /* all reads submitted */

    /* Open file if needed */
    if (task->src_fd < 0) {
        if (open_task(task, ctx->config) != 0) {
            /* Skip this file */
            task->reads_done = true;
            task->done = true;
            q->completed_files++;
            q->current = task->next;
            /* Try next task */
            submit_next_read(ctx, slot);
            return;
        }

        /* Handle zero-length file */
        if (task->file_size == 0) {
            task->reads_done = true;
            finish_task(ctx, task);
            q->current = task->next;
            submit_next_read(ctx, slot);
            return;
        }
    }

    size_t chunk = ctx->config->chunk_size;
    if (task->read_offset + (off_t)chunk > task->file_size)
        chunk = (size_t)(task->file_size - task->read_offset);

    slot->offset = task->read_offset;
    slot->bytes = chunk;
    slot->state = BUF_READING;
    slot->task = task;
    task->read_offset += (off_t)chunk;

    if (task->read_offset >= task->file_size) {
        task->reads_done = true;
        q->current = task->next;
    }

    aura_request_t *req = aura_read(ctx->engine, task->src_fd,
                                     aura_buf(slot->buf), chunk,
                                     slot->offset, on_read_complete, slot);
    if (!req) {
        if (ctx->error == 0) {
            fprintf(stderr, "auracp: read submit failed on '%s': %s\n",
                    task->src_path, strerror(errno));
            ctx->error = -errno;
        }
        slot->state = BUF_FREE;
        slot->task = NULL;
        task->read_offset -= (off_t)chunk;
        return;
    }
    ctx->active_ops++;
    task->active_ops++;
}

static bool all_tasks_done(const task_queue_t *q) {
    for (file_task_t *t = q->head; t; t = t->next) {
        if (!t->done) return false;
    }
    return true;
}

static int copy_pipeline(copy_ctx_t *ctx) {
    int depth = ctx->config->pipeline_depth;

    /* Submit initial batch */
    for (int i = 0; i < depth && ctx->error == 0 && !g_interrupted; i++) {
        submit_next_read(ctx, &ctx->slots[i]);
    }

    /* Main event loop */
    while (!all_tasks_done(ctx->queue) && ctx->error == 0 && !g_interrupted) {
        int n = aura_wait(ctx->engine, 100);
        if (n < 0 && errno != EINTR && errno != ETIME) {
            if (ctx->error == 0) ctx->error = -errno;
            break;
        }

        /* Fill pipeline with new reads for FREE slots */
        for (int i = 0; i < depth; i++) {
            if (ctx->slots[i].state == BUF_FREE && ctx->error == 0 && !g_interrupted) {
                submit_next_read(ctx, &ctx->slots[i]);
            }
        }

        progress_update(ctx, false);
    }

    /* Drain remaining ops if we're exiting due to error/interrupt */
    if (ctx->active_ops > 0) {
        while (ctx->active_ops > 0) {
            aura_wait(ctx->engine, 100);
        }
    }

    return ctx->error;
}

// ============================================================================
// Cleanup helpers
// ============================================================================

static void cleanup_partial_files(const task_queue_t *q, bool keep) {
    if (keep) return;
    for (file_task_t *t = q->head; t; t = t->next) {
        if (!t->done && t->dst_path) {
            unlink(t->dst_path);
        }
    }
}

// ============================================================================
// CLI parsing
// ============================================================================

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [OPTIONS] SOURCE... DEST\n"
        "\n"
        "Async pipelined file copy powered by AuraIO.\n"
        "\n"
        "Options:\n"
        "  -r, --recursive      Copy directories recursively\n"
        "  -b, --block-size N   I/O block size (default: 256K). Suffixes: K, M, G\n"
        "  -d, --direct         Use O_DIRECT (bypass page cache)\n"
        "  -p, --pipeline N     In-flight I/O buffer slots (default: %d, max: %d)\n"
        "  -q, --quiet          Suppress all output\n"
        "  --no-fsync           Skip per-file fsync\n"
        "  --no-progress        Disable progress bar\n"
        "  --preserve           Preserve timestamps (mtime, atime)\n"
        "  --keep-partial       Don't delete partial files on error\n"
        "  -v, --verbose        Show AuraIO stats after copy\n"
        "  -h, --help           Show this help\n",
        argv0, DEFAULT_PIPELINE, MAX_PIPELINE);
}

static int parse_args(int argc, char **argv, config_t *config) {
    memset(config, 0, sizeof(*config));
    config->chunk_size = DEFAULT_CHUNK_SIZE;
    config->pipeline_depth = DEFAULT_PIPELINE;

    static struct option long_opts[] = {
        {"recursive",    no_argument,       0, 'r'},
        {"block-size",   required_argument, 0, 'b'},
        {"direct",       no_argument,       0, 'd'},
        {"pipeline",     required_argument, 0, 'p'},
        {"quiet",        no_argument,       0, 'q'},
        {"no-fsync",     no_argument,       0, 'F'},
        {"no-progress",  no_argument,       0, 'P'},
        {"preserve",     no_argument,       0, 'T'},
        {"keep-partial", no_argument,       0, 'K'},
        {"verbose",      no_argument,       0, 'v'},
        {"help",         no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "rb:dp:qvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'r': config->recursive = true; break;
        case 'b': {
            ssize_t sz = parse_size(optarg);
            if (sz <= 0) {
                fprintf(stderr, "auracp: invalid block size: %s\n", optarg);
                return -1;
            }
            config->chunk_size = (size_t)sz;
            break;
        }
        case 'd': config->use_direct = true; break;
        case 'p':
            config->pipeline_depth = atoi(optarg);
            if (config->pipeline_depth < 1 || config->pipeline_depth > MAX_PIPELINE) {
                fprintf(stderr, "auracp: pipeline must be 1-%d\n", MAX_PIPELINE);
                return -1;
            }
            break;
        case 'q': config->quiet = true; config->no_progress = true; break;
        case 'v': config->verbose = true; break;
        case 'F': config->no_fsync = true; break;
        case 'P': config->no_progress = true; break;
        case 'T': config->preserve = true; break;
        case 'K': config->keep_partial = true; break;
        case 'h': print_usage(argv[0]); exit(0);
        default:  print_usage(argv[0]); return -1;
        }
    }

    int remaining = argc - optind;
    if (remaining < 2) {
        fprintf(stderr, "auracp: expected SOURCE... DEST arguments\n");
        print_usage(argv[0]);
        return -1;
    }

    config->sources = (const char **)&argv[optind];
    config->num_sources = remaining - 1;
    config->dest = argv[argc - 1];

    if (config->use_direct && (config->chunk_size % SECTOR_SIZE) != 0) {
        fprintf(stderr,
                "auracp: block size must be sector-aligned (%d) with --direct\n",
                SECTOR_SIZE);
        return -1;
    }

    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    config_t config;
    if (parse_args(argc, argv, &config) != 0)
        return 1;

    /* Install SIGINT handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* Build task list */
    task_queue_t queue;
    if (build_task_list(&config, &queue) != 0)
        return 1;

    if (!config.quiet) {
        char total_str[32];
        format_bytes(total_str, sizeof(total_str), (double)queue.total_bytes);
        fprintf(stderr, "auracp: %d file%s (%s)\n",
                queue.total_files, queue.total_files == 1 ? "" : "s", total_str);
    }

    /* Create AuraIO engine */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = config.pipeline_depth * 4;
    if (opts.queue_depth < 64) opts.queue_depth = 64;
    opts.single_thread = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "auracp: failed to create I/O engine: %s\n", strerror(errno));
        queue_free(&queue);
        return 1;
    }

    /* Allocate pipeline buffer slots */
    buf_slot_t *slots = calloc((size_t)config.pipeline_depth, sizeof(buf_slot_t));
    if (!slots) {
        fprintf(stderr, "auracp: out of memory\n");
        aura_destroy(engine);
        queue_free(&queue);
        return 1;
    }

    copy_ctx_t ctx = {
        .engine = engine,
        .config = &config,
        .queue = &queue,
        .slots = slots,
        .active_ops = 0,
        .error = 0,
    };

    for (int i = 0; i < config.pipeline_depth; i++) {
        slots[i].buf = aura_buffer_alloc(engine, config.chunk_size);
        if (!slots[i].buf) {
            fprintf(stderr, "auracp: buffer allocation failed\n");
            for (int j = 0; j < i; j++)
                aura_buffer_free(engine, slots[j].buf);
            free(slots);
            aura_destroy(engine);
            queue_free(&queue);
            return 1;
        }
        slots[i].state = BUF_FREE;
        slots[i].ctx = &ctx;
    }

    /* Run copy */
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    int err = copy_pipeline(&ctx);

    /* Final progress */
    progress_update(&ctx, true);

    /* Verbose stats */
    if (config.verbose && err == 0 && !g_interrupted) {
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (double)(end.tv_sec - g_start_time.tv_sec)
                       + (double)(end.tv_nsec - g_start_time.tv_nsec) / 1e9;

        char size_str[32], rate_str[32], chunk_str[32], pipe_str[32];
        format_bytes(size_str, sizeof(size_str), (double)queue.total_written);
        format_rate(rate_str, sizeof(rate_str),
                    elapsed > 0 ? (double)queue.total_written / elapsed : 0);
        format_bytes(chunk_str, sizeof(chunk_str), (double)config.chunk_size);
        format_bytes(pipe_str, sizeof(pipe_str),
                     (double)config.chunk_size * config.pipeline_depth);

        fprintf(stderr, "Copied %d file%s (%s) in %.2fs\n",
                queue.completed_files, queue.completed_files == 1 ? "" : "s",
                size_str, elapsed);
        fprintf(stderr, "Throughput: %s\n", rate_str);
        fprintf(stderr, "Pipeline: %d buffers x %s = %s\n",
                config.pipeline_depth, chunk_str, pipe_str);

        aura_ring_stats_t rstats;
        if (aura_get_ring_stats(engine, 0, &rstats, sizeof(rstats)) == 0) {
            fprintf(stderr, "AuraIO: depth=%d, phase=%s, p99=%.2fms\n",
                    rstats.in_flight_limit,
                    aura_phase_name(rstats.aimd_phase),
                    rstats.p99_latency_ms);
        }
    }

    /* Cleanup */
    for (int i = 0; i < config.pipeline_depth; i++) {
        if (slots[i].buf) aura_buffer_free(engine, slots[i].buf);
    }
    free(slots);
    aura_destroy(engine);

    /* Handle errors / interrupts */
    if (err != 0 || g_interrupted) {
        cleanup_partial_files(&queue, config.keep_partial);
        queue_free(&queue);
        if (g_interrupted) {
            fprintf(stderr, "\nauracp: interrupted\n");
            return 130;
        }
        return 1;
    }

    queue_free(&queue);
    return 0;
}
