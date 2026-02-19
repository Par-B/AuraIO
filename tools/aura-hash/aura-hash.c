// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file aura-hash.c
 * @brief aura-hash - parallel file checksum tool powered by AuraIO
 *
 * A sha256sum/md5sum replacement that uses AuraIO's pipelined async I/O
 * to read multiple files concurrently and hash data as completions arrive.
 *
 * Usage: aura-hash [OPTIONS] FILE...
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
#include <sys/stat.h>

#include <openssl/evp.h>

#include <aura.h>

// ============================================================================
// Constants
// ============================================================================

#define DEFAULT_CHUNK_SIZE (256 * 1024) /* 256 KiB */
#define DEFAULT_PIPELINE 8
#define MAX_PIPELINE 64
#define PROGRESS_INTERVAL_MS 200
#define SECTOR_SIZE 512
#define NFTW_MAX_FDS 64

// ============================================================================
// Configuration
// ============================================================================

typedef struct {
    const char **files;
    int num_files;
    const EVP_MD *md;
    const char *algorithm;
    size_t chunk_size;
    int pipeline_depth;
    bool recursive;
    bool use_direct;
    bool quiet;
    bool verbose;
} config_t;

// ============================================================================
// File task queue
// ============================================================================

typedef struct file_task {
    char *path;
    off_t file_size;
    int fd;
    off_t read_offset;
    int active_ops;
    bool reads_done;
    bool done;
    EVP_MD_CTX *md_ctx;
    off_t bytes_hashed;
    struct file_task *next;
} file_task_t;

typedef struct {
    file_task_t *head;
    file_task_t *tail;
    file_task_t *current;
    int total_files;
    int completed_files;
    off_t total_bytes;
    off_t bytes_read;
} task_queue_t;

// ============================================================================
// Pipeline buffer slots
// ============================================================================

typedef enum { BUF_FREE = 0, BUF_READING } buf_state_t;

struct hash_ctx;

typedef struct {
    void *buf;
    off_t offset;
    size_t bytes;
    buf_state_t state;
    file_task_t *task;
    struct hash_ctx *ctx;
} buf_slot_t;

typedef struct hash_ctx {
    aura_engine_t *engine;
    const config_t *config;
    task_queue_t *queue;
    buf_slot_t *slots;
    int active_ops;
    int error;
} hash_ctx_t;

// ============================================================================
// Global state
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
    case 'G':
    case 'g':
        val *= 1024.0 * 1024.0 * 1024.0;
        break;
    case 'M':
    case 'm':
        val *= 1024.0 * 1024.0;
        break;
    case 'K':
    case 'k':
        val *= 1024.0;
        break;
    case '\0':
        break;
    default:
        return -1;
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
    else if (bytes >= 1024.0 * 1024.0) snprintf(buf, bufsz, "%.1f MiB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024.0) snprintf(buf, bufsz, "%.1f KiB", bytes / 1024.0);
    else snprintf(buf, bufsz, "%.0f B", bytes);
}

static void format_rate(char *buf, size_t bufsz, double bps) {
    if (bps >= 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f GiB/s", bps / (1024.0 * 1024.0 * 1024.0));
    else if (bps >= 1024.0 * 1024.0) snprintf(buf, bufsz, "%.1f MiB/s", bps / (1024.0 * 1024.0));
    else if (bps >= 1024.0) snprintf(buf, bufsz, "%.1f KiB/s", bps / 1024.0);
    else snprintf(buf, bufsz, "%.0f B/s", bps);
}

// ============================================================================
// Progress display
// ============================================================================

static void progress_update(const hash_ctx_t *ctx, bool final) {
    if (ctx->config->quiet) return;
    if (!final && !isatty(STDERR_FILENO)) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;

    if (!final && (now_ns - g_last_progress_ns) < PROGRESS_INTERVAL_MS * 1000000ULL) return;
    g_last_progress_ns = now_ns;

    double elapsed = (double)(now.tv_sec - g_start_time.tv_sec) +
                     (double)(now.tv_nsec - g_start_time.tv_nsec) / 1e9;
    double done = (double)ctx->queue->bytes_read;
    double total = (double)ctx->queue->total_bytes;
    double pct = (total > 0) ? done / total * 100.0 : 100.0;
    double rate = (elapsed > 0.01) ? done / elapsed : 0.0;

    char done_str[32], total_str[32], rate_str[32];
    format_bytes(done_str, sizeof(done_str), done);
    format_bytes(total_str, sizeof(total_str), total);
    format_rate(rate_str, sizeof(rate_str), rate);

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

    char eta[32] = "";
    if (rate > 0 && total > done) {
        double remaining = (total - done) / rate;
        int mins = (int)(remaining / 60.0);
        int secs = (int)remaining % 60;
        snprintf(eta, sizeof(eta), "ETA %d:%02d", mins, secs);
    }

    fprintf(stderr, "\r  %s / %s  [%s]  %3.0f%%  %s  %s   ", done_str, total_str, bar, pct,
            rate_str, eta);

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
    if (q->tail) q->tail->next = task;
    else q->head = task;
    q->tail = task;
    if (!q->current) q->current = task;
    q->total_files++;
    q->total_bytes += task->file_size;
}

static void queue_free(task_queue_t *q) {
    file_task_t *t = q->head;
    while (t) {
        file_task_t *next = t->next;
        free(t->path);
        if (t->fd >= 0) close(t->fd);
        if (t->md_ctx) EVP_MD_CTX_free(t->md_ctx);
        free(t);
        t = next;
    }
    memset(q, 0, sizeof(*q));
}

// ============================================================================
// Task list building
// ============================================================================

static task_queue_t *g_walk_queue;
static const config_t *g_walk_config;
static int g_walk_errors;

static int add_file_task(task_queue_t *q, const char *path, off_t size, const config_t *config) {
    file_task_t *task = calloc(1, sizeof(*task));
    if (!task) return -1;

    task->path = strdup(path);
    if (!task->path) {
        free(task);
        return -1;
    }
    task->file_size = size;
    task->fd = -1;

    task->md_ctx = EVP_MD_CTX_new();
    if (!task->md_ctx) {
        free(task->path);
        free(task);
        return -1;
    }
    if (EVP_DigestInit_ex(task->md_ctx, config->md, NULL) != 1) {
        EVP_MD_CTX_free(task->md_ctx);
        free(task->path);
        free(task);
        return -1;
    }

    queue_push(q, task);
    return 0;
}

static int nftw_callback(const char *fpath, const struct stat *sb, int typeflag,
                         struct FTW *ftwbuf) {
    (void)ftwbuf;
    if (typeflag != FTW_F) return 0;
    if (!S_ISREG(sb->st_mode)) return 0;

    if (add_file_task(g_walk_queue, fpath, sb->st_size, g_walk_config) != 0) {
        g_walk_errors++;
    }
    return 0;
}

static int build_task_list(const config_t *config, task_queue_t *queue) {
    queue_init(queue);

    for (int i = 0; i < config->num_files; i++) {
        const char *path = config->files[i];
        struct stat st;

        if (stat(path, &st) != 0) {
            fprintf(stderr, "aura-hash: cannot stat '%s': %s\n", path, strerror(errno));
            return -1;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!config->recursive) {
                fprintf(stderr, "aura-hash: '%s' is a directory (use -r)\n", path);
                return -1;
            }
            g_walk_queue = queue;
            g_walk_config = config;
            g_walk_errors = 0;
            if (nftw(path, nftw_callback, NFTW_MAX_FDS, 0) != 0) {
                fprintf(stderr, "aura-hash: cannot walk '%s': %s\n", path, strerror(errno));
                return -1;
            }
            if (g_walk_errors) return -1;
        } else if (S_ISREG(st.st_mode)) {
            if (add_file_task(queue, path, st.st_size, config) != 0) return -1;
        } else {
            fprintf(stderr, "aura-hash: skipping special file: %s\n", path);
        }
    }

    if (queue->total_files == 0) {
        fprintf(stderr, "aura-hash: no files to hash\n");
        return -1;
    }
    return 0;
}

// ============================================================================
// Hash finalization and output
// ============================================================================

static void finalize_task(file_task_t *task, const config_t *config) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    EVP_DigestFinal_ex(task->md_ctx, digest, &digest_len);

    /* Print in coreutils format: <hex>  <filename> */
    for (unsigned int i = 0; i < digest_len; i++) {
        printf("%02x", digest[i]);
    }
    printf("  %s\n", task->path);

    (void)config;
}

// ============================================================================
// I/O callbacks
// ============================================================================

static void on_read_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    buf_slot_t *slot = (buf_slot_t *)user_data;
    hash_ctx_t *ctx = slot->ctx;
    file_task_t *task = slot->task;

    if (result <= 0) {
        if (result < 0 && ctx->error == 0) {
            fprintf(stderr, "aura-hash: read error on '%s': %s\n", task->path,
                    strerror(-(int)result));
            ctx->error = (int)result;
        }
        slot->state = BUF_FREE;
        slot->task = NULL;
        ctx->active_ops--;
        task->active_ops--;
        return;
    }

    /* Feed data into hash context */
    EVP_DigestUpdate(task->md_ctx, slot->buf, (size_t)result);
    task->bytes_hashed += result;
    ctx->queue->bytes_read += result;

    slot->state = BUF_FREE;
    slot->task = NULL;
    ctx->active_ops--;
    task->active_ops--;

    /* Check if this file is fully hashed */
    if (task->reads_done && task->active_ops == 0 && !task->done) {
        finalize_task(task, ctx->config);
        close(task->fd);
        task->fd = -1;
        task->done = true;
        ctx->queue->completed_files++;
    }
}

// ============================================================================
// Pipeline core
// ============================================================================

static void submit_next_read(hash_ctx_t *ctx, buf_slot_t *slot) {
    task_queue_t *q = ctx->queue;

    file_task_t *task = q->current;
    while (task && (task->reads_done || task->done)) {
        task = task->next;
    }
    if (!task) return;

    /* Open file if needed */
    if (task->fd < 0) {
        int flags = O_RDONLY;
        if (ctx->config->use_direct) flags |= O_DIRECT;

        task->fd = open(task->path, flags);
        if (task->fd < 0) {
            fprintf(stderr, "aura-hash: cannot open '%s': %s\n", task->path, strerror(errno));
            task->reads_done = true;
            task->done = true;
            q->completed_files++;
            q->current = task->next;
            submit_next_read(ctx, slot);
            return;
        }

        /* Handle zero-length file */
        if (task->file_size == 0) {
            task->reads_done = true;
            finalize_task(task, ctx->config);
            close(task->fd);
            task->fd = -1;
            task->done = true;
            q->completed_files++;
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

    aura_request_t *rreq = aura_read(ctx->engine, task->fd, aura_buf(slot->buf), chunk,
                                     slot->offset, on_read_complete, slot);
    if (!rreq) {
        if (ctx->error == 0) {
            fprintf(stderr, "aura-hash: read submit failed on '%s': %s\n", task->path,
                    strerror(errno));
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

static int hash_pipeline(hash_ctx_t *ctx) {
    int depth = ctx->config->pipeline_depth;

    /* Submit initial batch */
    for (int i = 0; i < depth && ctx->error == 0 && !g_interrupted; i++) {
        submit_next_read(ctx, &ctx->slots[i]);
    }

    /* Main event loop */
    while (!all_tasks_done(ctx->queue) && ctx->error == 0 && !g_interrupted) {
        int n = aura_wait(ctx->engine, 100);
        if (n < 0 && errno != EINTR && errno != ETIME && errno != ETIMEDOUT) {
            if (ctx->error == 0) ctx->error = -errno;
            break;
        }

        for (int i = 0; i < depth; i++) {
            if (ctx->slots[i].state == BUF_FREE && ctx->error == 0 && !g_interrupted) {
                submit_next_read(ctx, &ctx->slots[i]);
            }
        }

        progress_update(ctx, false);
    }

    /* Drain remaining ops */
    while (ctx->active_ops > 0) {
        aura_wait(ctx->engine, 100);
    }

    return ctx->error;
}

// ============================================================================
// CLI parsing
// ============================================================================

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [OPTIONS] FILE...\n"
            "\n"
            "Parallel file checksum tool powered by AuraIO.\n"
            "\n"
            "Options:\n"
            "  -a, --algorithm ALG  Hash algorithm: sha256 (default), sha1, md5\n"
            "  -r, --recursive      Hash directories recursively\n"
            "  -d, --direct         Use O_DIRECT (bypass page cache)\n"
            "  -b, --block-size N   Read chunk size (default: 256K). Suffixes: K, M, G\n"
            "  -p, --pipeline N     In-flight read slots (default: %d, max: %d)\n"
            "  -q, --quiet          Suppress progress\n"
            "  -v, --verbose        Show AuraIO stats after hashing\n"
            "  -h, --help           Show this help\n",
            argv0, DEFAULT_PIPELINE, MAX_PIPELINE);
}

static const EVP_MD *resolve_algorithm(const char *name) {
    if (strcmp(name, "sha256") == 0) return EVP_sha256();
    if (strcmp(name, "sha1") == 0) return EVP_sha1();
    if (strcmp(name, "md5") == 0) return EVP_md5();
    return NULL;
}

static int parse_args(int argc, char **argv, config_t *config) {
    memset(config, 0, sizeof(*config));
    config->chunk_size = DEFAULT_CHUNK_SIZE;
    config->pipeline_depth = DEFAULT_PIPELINE;
    config->algorithm = "sha256";

    static struct option long_opts[] = { { "algorithm", required_argument, 0, 'a' },
                                         { "recursive", no_argument, 0, 'r' },
                                         { "direct", no_argument, 0, 'd' },
                                         { "block-size", required_argument, 0, 'b' },
                                         { "pipeline", required_argument, 0, 'p' },
                                         { "quiet", no_argument, 0, 'q' },
                                         { "verbose", no_argument, 0, 'v' },
                                         { "help", no_argument, 0, 'h' },
                                         { 0, 0, 0, 0 } };

    int opt;
    while ((opt = getopt_long(argc, argv, "a:rdb:p:qvh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'a':
            config->algorithm = optarg;
            break;
        case 'r':
            config->recursive = true;
            break;
        case 'd':
            config->use_direct = true;
            break;
        case 'b': {
            ssize_t sz = parse_size(optarg);
            if (sz <= 0) {
                fprintf(stderr, "aura-hash: invalid block size: %s\n", optarg);
                return -1;
            }
            config->chunk_size = (size_t)sz;
            break;
        }
        case 'p': {
            char *end;
            long val = strtol(optarg, &end, 10);
            if (*end != '\0' || val < 1 || val > MAX_PIPELINE) {
                fprintf(stderr, "aura-hash: pipeline must be 1-%d\n", MAX_PIPELINE);
                return -1;
            }
            config->pipeline_depth = (int)val;
            break;
        }
        case 'q':
            config->quiet = true;
            break;
        case 'v':
            config->verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    int remaining = argc - optind;
    if (remaining < 1) {
        fprintf(stderr, "aura-hash: expected FILE... arguments\n");
        print_usage(argv[0]);
        return -1;
    }

    config->files = (const char **)&argv[optind];
    config->num_files = remaining;

    config->md = resolve_algorithm(config->algorithm);
    if (!config->md) {
        fprintf(stderr, "aura-hash: unknown algorithm '%s' (use sha256, sha1, md5)\n",
                config->algorithm);
        return -1;
    }

    if (config->use_direct && (config->chunk_size % SECTOR_SIZE) != 0) {
        fprintf(stderr, "aura-hash: block size must be sector-aligned (%d) with --direct\n",
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
    if (parse_args(argc, argv, &config) != 0) return 1;

    /* Install SIGINT handler */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    /* Build task list */
    task_queue_t queue;
    if (build_task_list(&config, &queue) != 0) return 1;

    if (!config.quiet) {
        char total_str[32];
        format_bytes(total_str, sizeof(total_str), (double)queue.total_bytes);
        fprintf(stderr, "aura-hash: %d file%s (%s) [%s]\n", queue.total_files,
                queue.total_files == 1 ? "" : "s", total_str, config.algorithm);
    }

    /* Create AuraIO engine */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = config.pipeline_depth * 4;
    if (opts.queue_depth < 64) opts.queue_depth = 64;
    opts.single_thread = true;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "aura-hash: failed to create I/O engine: %s\n", strerror(errno));
        queue_free(&queue);
        return 1;
    }

    /* Allocate pipeline buffer slots */
    buf_slot_t *slots = calloc((size_t)config.pipeline_depth, sizeof(buf_slot_t));
    if (!slots) {
        fprintf(stderr, "aura-hash: out of memory\n");
        aura_destroy(engine);
        queue_free(&queue);
        return 1;
    }

    hash_ctx_t ctx = {
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
            fprintf(stderr, "aura-hash: buffer allocation failed\n");
            for (int j = 0; j < i; j++) aura_buffer_free(engine, slots[j].buf);
            free(slots);
            aura_destroy(engine);
            queue_free(&queue);
            return 1;
        }
        slots[i].state = BUF_FREE;
        slots[i].ctx = &ctx;
    }

    /* Run hash pipeline */
    clock_gettime(CLOCK_MONOTONIC, &g_start_time);
    int err = hash_pipeline(&ctx);

    /* Final progress */
    if (!config.quiet) progress_update(&ctx, true);

    /* Verbose stats */
    if (config.verbose && err == 0 && !g_interrupted) {
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        double elapsed = (double)(end.tv_sec - g_start_time.tv_sec) +
                         (double)(end.tv_nsec - g_start_time.tv_nsec) / 1e9;

        char size_str[32], rate_str[32];
        format_bytes(size_str, sizeof(size_str), (double)queue.bytes_read);
        format_rate(rate_str, sizeof(rate_str),
                    elapsed > 0 ? (double)queue.bytes_read / elapsed : 0);

        fprintf(stderr, "Hashed %d file%s (%s) in %.2fs\n", queue.completed_files,
                queue.completed_files == 1 ? "" : "s", size_str, elapsed);
        fprintf(stderr, "Throughput: %s\n", rate_str);
        fprintf(stderr, "Algorithm: %s, pipeline: %d slots\n", config.algorithm,
                config.pipeline_depth);

        aura_ring_stats_t rstats;
        if (aura_get_ring_stats(engine, 0, &rstats, sizeof(rstats)) == 0) {
            fprintf(stderr, "AuraIO: depth=%d, phase=%s, p99=%.2fms\n", rstats.in_flight_limit,
                    aura_phase_name(rstats.aimd_phase), rstats.p99_latency_ms);
        }
    }

    /* Cleanup */
    for (int i = 0; i < config.pipeline_depth; i++) {
        if (slots[i].buf) aura_buffer_free(engine, slots[i].buf);
    }
    free(slots);
    aura_destroy(engine);
    queue_free(&queue);

    if (err != 0 || g_interrupted) {
        if (g_interrupted) {
            fprintf(stderr, "\naura-hash: interrupted\n");
            return 130;
        }
        return 1;
    }

    return 0;
}
