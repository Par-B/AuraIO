// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file workload.c
 * @brief Core I/O loop, file management, and worker threads for BFFIO
 *
 * Drives the benchmark workload: opens/creates files, spawns worker threads,
 * submits async I/O via Aura, collects completions via callbacks, and
 * manages the ramp/measurement lifecycle.
 *
 * The hot path (submit + callback) is zero-malloc: each thread pre-allocates
 * an io_ctx_pool_t sized to iodepth, managed as a free-stack.
 */

#define _GNU_SOURCE
#include "workload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

/* ============================================================================
 * Timing utilities
 * ============================================================================ */

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================================
 * Fast PRNG (xorshift64)
 * ============================================================================ */

static inline uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

/* ============================================================================
 * Alignment helper
 * ============================================================================ */

static inline uint64_t align_down(uint64_t val, uint64_t align) {
    return val & ~(align - 1);
}

/* ============================================================================
 * I/O pattern helpers
 * ============================================================================ */

static inline int pattern_is_random(rw_pattern_t rw) {
    return rw == RW_RANDREAD || rw == RW_RANDWRITE || rw == RW_RANDRW;
}

static inline int pattern_is_read_only(rw_pattern_t rw) {
    return rw == RW_READ || rw == RW_RANDREAD;
}

/* ============================================================================
 * io_ctx_pool -- pre-allocated pool of I/O callback contexts
 * ============================================================================ */

int io_ctx_pool_init(io_ctx_pool_t *pool, int capacity) {
    pool->capacity = capacity;

    pool->slots = calloc((size_t)capacity, sizeof(io_ctx_t));
    if (!pool->slots) {
        return -1;
    }

    /* Build free list: slot[0]→slot[1]→...→slot[N-1]→-1
     * free_head points to slot[capacity-1] (LIFO). */
    for (int i = 0; i < capacity; i++) {
        pool->slots[i].pool_idx = i;
        atomic_store_explicit(&pool->slots[i].next_free, i - 1, memory_order_relaxed);
    }
    atomic_store_explicit(&pool->free_head, capacity - 1, memory_order_release);

    return 0;
}

void io_ctx_pool_destroy(io_ctx_pool_t *pool) {
    free(pool->slots);
    pool->slots = NULL;
    pool->capacity = 0;
}

io_ctx_t *io_ctx_pool_get(io_ctx_pool_t *pool) {
    int idx = atomic_load_explicit(&pool->free_head, memory_order_acquire);
    while (idx >= 0) {
        int next = atomic_load_explicit(&pool->slots[idx].next_free, memory_order_relaxed);
        if (atomic_compare_exchange_weak_explicit(&pool->free_head, &idx, next,
                                                  memory_order_acq_rel, memory_order_relaxed)) {
            return &pool->slots[idx];
        }
        /* idx updated by CAS failure, retry */
    }
    return NULL;
}

void io_ctx_pool_put(io_ctx_pool_t *pool, io_ctx_t *ctx) {
    int idx = ctx->pool_idx;
    int old_head = atomic_load_explicit(&pool->free_head, memory_order_relaxed);
    do {
        atomic_store_explicit(&ctx->next_free, old_head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&pool->free_head, &old_head, idx,
                                                    memory_order_release, memory_order_relaxed));
}

/* ============================================================================
 * file_set -- file creation, open, close, cleanup
 * ============================================================================ */

/* Forward declaration: file_set_close is called by file_set_open on error */
static void file_set_close(file_set_t *fset);

/**
 * Create parent directories recursively (like mkdir -p).
 * Returns 0 on success (or if directory already exists), -1 on error.
 */
static int mkdirp(const char *path, mode_t mode) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX) return -1;

    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    return mkdir(tmp, mode) == 0 || errno == EEXIST ? 0 : -1;
}

/**
 * Open a file with the given flags, with O_DIRECT fallback.
 * Returns fd >= 0 on success, -1 on error.
 */
static int open_with_direct_fallback(const char *path, int base_flags, int use_direct) {
    int flags = base_flags;
    if (use_direct) {
        flags |= O_DIRECT;
    }

    int fd = open(path, flags);
    if (fd < 0 && use_direct) {
        /* O_DIRECT fallback */
        fd = open(path, base_flags);
        if (fd >= 0) {
            fprintf(stderr,
                    "BFFIO: O_DIRECT not supported on '%s', "
                    "falling back to buffered I/O\n",
                    path);
        }
    }
    return fd;
}

static int file_set_open(file_set_t *fset, const job_config_t *config) {
    memset(fset, 0, sizeof(*fset));

    /* Determine open flags */
    int base_flags;
    if (pattern_is_read_only(config->rw)) {
        base_flags = O_RDONLY;
    } else {
        base_flags = O_RDWR;
    }

    /* Number of files: nrfiles for directory mode, 1 for explicit filename */
    int numfiles;
    if (config->filename[0] != '\0') {
        numfiles = 1;
    } else {
        numfiles = config->nrfiles > 0 ? config->nrfiles : 1;
    }

    /* Per-file size for directory mode file creation */
    uint64_t per_file_size =
        config->filesize > 0
            ? config->filesize
            : (numfiles > 1 && config->size > 0 ? config->size / (uint64_t)numfiles : config->size);

    /* Allocate arrays */
    fset->fds = calloc((size_t)numfiles, sizeof(int));
    fset->created_paths = calloc((size_t)numfiles, sizeof(char *));
    if (!fset->fds || !fset->created_paths) {
        free(fset->fds);
        free(fset->created_paths);
        memset(fset, 0, sizeof(*fset));
        return -1;
    }

    for (int i = 0; i < numfiles; i++) {
        fset->fds[i] = -1;
    }

    if (config->filename[0] != '\0') {
        /* Explicit file/device: open directly, shared by all threads */
        int fd = open_with_direct_fallback(config->filename, base_flags, config->direct);
        if (fd < 0) {
            fprintf(stderr, "BFFIO: failed to open '%s': %s\n", config->filename, strerror(errno));
            free(fset->fds);
            free(fset->created_paths);
            memset(fset, 0, sizeof(*fset));
            return -1;
        }

        fset->fds[0] = fd;
        fset->fd_count = 1;
        return 0;
    }

    if (config->directory[0] != '\0') {
        /* Create directory and files */
        if (mkdirp(config->directory, 0755) != 0) {
            fprintf(stderr, "BFFIO: failed to create directory '%s': %s\n", config->directory,
                    strerror(errno));
            free(fset->fds);
            free(fset->created_paths);
            memset(fset, 0, sizeof(*fset));
            return -1;
        }

        for (int i = 0; i < numfiles; i++) {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/BFFIO.%d.tmp", config->directory, i);

            /* Create the file */
            int create_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (create_fd < 0) {
                fprintf(stderr, "BFFIO: failed to create '%s': %s\n", path, strerror(errno));
                goto cleanup_error;
            }

            /* Write real data to the file.  posix_fallocate() alone creates
             * unwritten extents — ext4 returns zeros without disk I/O on
             * read, defeating O_DIRECT benchmarks.  We write non-zero data
             * in 1 MiB chunks so every block is actually allocated on disk. */
            if (per_file_size > 0) {
                const size_t CHUNK = 1024 * 1024;
                char *fill = malloc(CHUNK);
                if (!fill) {
                    close(create_fd);
                    goto cleanup_error;
                }
                /* Fill with a non-zero pattern (0xA5) */
                memset(fill, 0xA5, CHUNK);

                uint64_t remaining = per_file_size;
                while (remaining > 0) {
                    size_t to_write = remaining < CHUNK ? (size_t)remaining : CHUNK;
                    ssize_t w = write(create_fd, fill, to_write);
                    if (w < 0) {
                        free(fill);
                        close(create_fd);
                        goto cleanup_error;
                    }
                    remaining -= (uint64_t)w;
                }
                free(fill);
            }
            fsync(create_fd);
            close(create_fd);

            fset->created_paths[fset->created_count] = strdup(path);
            fset->created_count++;

            /* Re-open with desired flags */
            int fd = open_with_direct_fallback(path, base_flags, config->direct);
            if (fd < 0) {
                fprintf(stderr, "BFFIO: failed to open '%s': %s\n", path, strerror(errno));
                goto cleanup_error;
            }

            fset->fds[i] = fd;
            fset->fd_count++;
        }

        return 0;
    }

    fprintf(stderr, "BFFIO: no filename or directory specified\n");
    free(fset->fds);
    free(fset->created_paths);
    memset(fset, 0, sizeof(*fset));
    return -1;

cleanup_error:
    file_set_close(fset);
    return -1;
}

static void file_set_close(file_set_t *fset) {
    if (!fset) return;

    /* Close file descriptors.
     * For explicit filename mode, all fds[] may point to the same fd,
     * so track and avoid double-close. */
    if (fset->fds) {
        int prev_fd = -1;
        for (int i = 0; i < fset->fd_count; i++) {
            if (fset->fds[i] >= 0 && fset->fds[i] != prev_fd) {
                close(fset->fds[i]);
                prev_fd = fset->fds[i];
            }
        }
        free(fset->fds);
    }

    /* Unlink created temp files */
    if (fset->created_paths) {
        for (int i = 0; i < fset->created_count; i++) {
            if (fset->created_paths[i]) {
                unlink(fset->created_paths[i]);
                free(fset->created_paths[i]);
            }
        }
        free(fset->created_paths);
    }

    memset(fset, 0, sizeof(*fset));
}

/* ============================================================================
 * I/O completion callback (the hot path)
 *
 * Callbacks run inside aura_wait() on the worker thread, so tls_pool
 * points to the correct per-thread pool.
 *
 * Completion timestamp is cached per poll cycle via tls_completion_ns
 * to avoid calling clock_gettime for every individual callback.
 * Reset to 0 before each aura_poll()/aura_wait() call; the first
 * callback in the batch calls now_ns() and subsequent callbacks reuse it.
 * ============================================================================ */

static _Thread_local uint64_t tls_completion_ns;

static void io_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    io_ctx_t *ctx = user_data;

    /* Record stats only outside the ramp period */
    if (!atomic_load(ctx->ramping)) {
        if (ctx->submit_time_ns != 0) {
            if (tls_completion_ns == 0) tls_completion_ns = now_ns();
            uint64_t latency = tls_completion_ns - ctx->submit_time_ns;
            stats_record_io(ctx->stats, latency, ctx->io_size, ctx->is_write);
        } else {
            stats_record_io_count(ctx->stats, ctx->io_size, ctx->is_write);
        }
    }

    if (result < 0) {
        atomic_fetch_add(&ctx->stats->errors, 1);
    }

    /* Buffer is pre-allocated per slot and reused — not freed here. */

    /* Decrement inflight counter */
    atomic_fetch_sub(&ctx->stats->inflight, 1);

    /* Return context to its owning pool */
    io_ctx_pool_put((io_ctx_pool_t *)ctx->pool, ctx);
}

/* ============================================================================
 * Fsync completion callback
 * ============================================================================ */

static void fsync_callback(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    io_ctx_t *ctx = user_data;

    if (!atomic_load(ctx->ramping)) {
        if (ctx->submit_time_ns != 0) {
            if (tls_completion_ns == 0) tls_completion_ns = now_ns();
            uint64_t latency = tls_completion_ns - ctx->submit_time_ns;
            stats_record_io(ctx->stats, latency, 0, 1);
        } else {
            stats_record_io_count(ctx->stats, 0, 1);
        }
    }

    if (result < 0) {
        atomic_fetch_add(&ctx->stats->errors, 1);
    }

    /* fsync has no buffer to free */

    atomic_fetch_sub(&ctx->stats->inflight, 1);
    io_ctx_pool_put((io_ctx_pool_t *)ctx->pool, ctx);
}

/* ============================================================================
 * Multi-file selection (hot path)
 * ============================================================================ */

/**
 * Select the next file index for I/O based on file_service_type.
 * Returns an index into the fds[] and seq_offsets[] arrays.
 *
 * Hot path: nrfiles<=1 short-circuits immediately. Each multi-file
 * branch is a simple integer operation — no locks, no allocations.
 */
static inline int select_file(thread_ctx_t *tctx, uint64_t *rng) {
    if (tctx->nrfiles <= 1) {
        return 0;
    }

    switch (tctx->file_service_type) {
    case FST_ROUNDROBIN: {
        int idx = tctx->current_file_idx;
        tctx->current_file_idx = (idx + 1) % tctx->nrfiles;
        return idx;
    }
    case FST_SEQUENTIAL: {
        int idx = tctx->current_file_idx;
        tctx->file_ios_done++;
        if (tctx->file_ios_done >= tctx->file_ios_limit) {
            tctx->file_ios_done = 0;
            tctx->current_file_idx = (idx + 1) % tctx->nrfiles;
        }
        return idx;
    }
    case FST_RANDOM:
        return (int)(xorshift64(rng) % (uint64_t)tctx->nrfiles);
    }
    return 0;
}

/* ============================================================================
 * Worker thread
 * ============================================================================ */

static void *worker_thread(void *arg) {
    thread_ctx_t *tctx = arg;
    const job_config_t *config = tctx->config;
    aura_engine_t *engine = tctx->engine;
    thread_stats_t *stats = tctx->stats;

    /* Initialize per-thread io_ctx pool */
    if (io_ctx_pool_init(&tctx->pool, tctx->effective_depth) != 0) {
        fprintf(stderr, "BFFIO: thread %d: failed to init io_ctx pool\n", tctx->thread_id);
        return NULL;
    }

    /* Pre-allocate aligned buffers for each pool slot (zero-malloc hot path).
     * Fill with non-zero data once to avoid zero-page optimization on writes.
     * FIO also fills once at init (--refill_buffers is opt-in). */
    for (int s = 0; s < tctx->effective_depth; s++) {
        tctx->pool.slots[s].buffer = aura_buffer_alloc(engine, (size_t)config->bs);
        if (tctx->pool.slots[s].buffer) {
            memset(tctx->pool.slots[s].buffer, 0xA5 ^ (s & 0xFF), (size_t)config->bs);
        }
        if (!tctx->pool.slots[s].buffer) {
            fprintf(stderr, "BFFIO: thread %d: failed to pre-allocate buffer %d\n", tctx->thread_id,
                    s);
            for (int f = 0; f < s; f++) {
                aura_buffer_free(engine, tctx->pool.slots[f].buffer);
                tctx->pool.slots[f].buffer = NULL;
            }
            io_ctx_pool_destroy(&tctx->pool);
            return NULL;
        }
    }

    /* Seed PRNG: unique per thread */
    uint64_t rng = (uint64_t)(tctx->thread_id + 1) * 2654435761ULL;
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        rng ^= (uint64_t)ts.tv_nsec;
    }
    if (rng == 0) rng = 1;

    /* Initialize adaptive latency sampling */
    tctx->sample_interval = 1;
    tctx->sample_calib_ns = now_ns();
    tctx->sample_io_count = 0;

    int iodepth = tctx->effective_depth;
    uint64_t bs = config->bs;
    uint64_t per_file_size = tctx->per_file_size;
    int is_random = pattern_is_random(config->rw);
    int is_mixed = (config->rw == RW_RANDRW || config->rw == RW_READWRITE);
    int is_write_only = (config->rw == RW_WRITE || config->rw == RW_RANDWRITE);

    /* Size-based completion: track total bytes submitted per thread */
    uint64_t total_bytes = 0;
    uint64_t per_thread_size = config->size / (uint64_t)(config->numjobs > 0 ? config->numjobs : 1);

    /* Fsync tracking */
    int writes_since_fsync = 0;

    /* Maximum valid offset for random I/O (per-file) */
    uint64_t max_offset = (per_file_size > bs) ? per_file_size - bs : 0;

    /* Main I/O loop */
    while (atomic_load(tctx->running)) {
        /* Size-based termination */
        if (!config->time_based && per_thread_size > 0) {
            if (total_bytes >= per_thread_size) {
                break;
            }
        }

        /* Submit I/O up to iodepth.
         * Batch the submit timestamp: one clock_gettime per submission
         * batch instead of per I/O.  All I/Os in a batch are submitted
         * within microseconds so the error is well within one histogram
         * bucket (50us). */
        uint64_t batch_submit_ns = 0;
        while (atomic_load(&stats->inflight) < iodepth) {
            if (!atomic_load(tctx->running)) break;
            if (!config->time_based && per_thread_size > 0 && total_bytes >= per_thread_size) {
                break;
            }

            io_ctx_t *ctx = io_ctx_pool_get(&tctx->pool);
            if (!ctx) break;

            void *buf = ctx->buffer;

            /* Select file for this I/O */
            int file_idx = select_file(tctx, &rng);
            int fd = tctx->fds[file_idx];

            /* Generate offset (per-file) */
            uint64_t offset;
            if (is_random) {
                if (max_offset > 0) {
                    offset = xorshift64(&rng) % max_offset;
                    offset = align_down(offset, bs);
                } else {
                    offset = 0;
                }
            } else {
                offset = tctx->seq_offsets[file_idx];
                if (offset + bs > per_file_size) {
                    offset = 0;
                    tctx->seq_offsets[file_idx] = bs;
                } else {
                    tctx->seq_offsets[file_idx] = offset + bs;
                }
            }

            /* Decide read vs write */
            int do_write;
            if (is_mixed) {
                do_write = (int)(xorshift64(&rng) % 100) >= config->rwmixread;
            } else if (is_write_only) {
                do_write = 1;
            } else {
                do_write = 0;
            }

            /* Adaptive latency sampling: recalibrate every ~65K I/Os to
             * maintain ~100K samples/sec budget. Use PRNG to select which
             * I/Os to sample, avoiding aliasing with power-of-2 batches. */
            tctx->sample_io_count++;
            if ((tctx->sample_io_count & 0xFFFF) == 0) {
                uint64_t cal_now = now_ns();
                uint64_t elapsed = cal_now - tctx->sample_calib_ns;
                if (elapsed > 0) {
                    uint64_t iops_est = 65536ULL * 1000000000ULL / elapsed;
                    tctx->sample_interval = (iops_est > 100000) ? (uint32_t)(iops_est / 100000) : 1;
                }
                tctx->sample_calib_ns = cal_now;
            }

            bool do_sample =
                (tctx->sample_interval <= 1) || (xorshift64(&rng) % tctx->sample_interval == 0);

            /* Fill callback context */
            ctx->stats = stats;
            if (do_sample) {
                if (batch_submit_ns == 0) batch_submit_ns = now_ns();
                ctx->submit_time_ns = batch_submit_ns;
            } else {
                ctx->submit_time_ns = 0;
            }
            ctx->io_size = (size_t)bs;
            ctx->buffer = buf;
            ctx->engine = engine;
            ctx->is_write = do_write;
            ctx->ramping = tctx->ramping;
            ctx->pool = &tctx->pool;

            /* Submit the I/O */
            atomic_fetch_add(&stats->inflight, 1);

            aura_request_t *req;
            if (do_write) {
                req = aura_write(engine, fd, aura_buf(buf), (size_t)bs, (off_t)offset, 0,
                                 io_callback, ctx);
            } else {
                req = aura_read(engine, fd, aura_buf(buf), (size_t)bs, (off_t)offset, 0,
                                io_callback, ctx);
            }

            if (!req) {
                atomic_fetch_sub(&stats->inflight, 1);
                io_ctx_pool_put(&tctx->pool, ctx);
                break;
            }

            total_bytes += bs;

            /* Periodic fsync */
            if (do_write && config->fsync_freq > 0) {
                writes_since_fsync++;
                if (writes_since_fsync >= config->fsync_freq) {
                    writes_since_fsync = 0;

                    io_ctx_t *fsync_ctx = io_ctx_pool_get(&tctx->pool);
                    if (fsync_ctx) {
                        fsync_ctx->stats = stats;
                        fsync_ctx->submit_time_ns = do_sample ? batch_submit_ns : 0;
                        fsync_ctx->io_size = 0;
                        fsync_ctx->buffer = NULL;
                        fsync_ctx->engine = engine;
                        fsync_ctx->is_write = 1;
                        fsync_ctx->ramping = tctx->ramping;
                        fsync_ctx->pool = &tctx->pool;

                        atomic_fetch_add(&stats->inflight, 1);

                        aura_request_t *fsync_req = aura_fsync(engine, fd, AURA_FSYNC_DEFAULT, 0,
                                                               fsync_callback, fsync_ctx);
                        if (!fsync_req) {
                            atomic_fetch_sub(&stats->inflight, 1);
                            io_ctx_pool_put(&tctx->pool, fsync_ctx);
                        }
                    }
                }
            }
        }

        /* Reset completion timestamp cache before processing completions.
         * The first callback in this poll cycle will call now_ns() once;
         * subsequent callbacks reuse that timestamp. */
        tls_completion_ns = 0;
        int poll_rc_ = aura_poll(engine);
        (void)poll_rc_;
    }

    /* Drain remaining inflight I/O for this thread */
    while (atomic_load(&stats->inflight) > 0) {
        tls_completion_ns = 0;
        int wait_rc_ = aura_wait(engine, 1);
        (void)wait_rc_;
    }

    /* Pool destruction is deferred to workload_run (after aura_drain)
     * to avoid use-after-free when cross-thread completions arrive. */

    atomic_fetch_add(tctx->workers_done, 1);
    return NULL;
}

/* ============================================================================
 * workload_run -- top-level entry point
 * ============================================================================ */

int workload_run(const job_config_t *config, aura_engine_t *engine, thread_stats_t *stats,
                 uint64_t *runtime_ms) {
    int num_threads = config->numjobs > 0 ? config->numjobs : 1;

    /* Open/create files */
    file_set_t fset;
    if (file_set_open(&fset, config) != 0) {
        return -1;
    }

    /* Determine per-file size for offset generation */
    uint64_t per_file_size;
    if (config->filesize > 0) {
        per_file_size = config->filesize;
    } else if (config->size > 0 && fset.fd_count > 0) {
        per_file_size = config->size / (uint64_t)fset.fd_count;
    } else if (fset.fd_count > 0 && fset.fds[0] >= 0) {
        struct stat st;
        if (fstat(fset.fds[0], &st) == 0 && st.st_size > 0) {
            per_file_size = (uint64_t)st.st_size;
        } else {
            per_file_size = 0;
        }
    } else {
        per_file_size = 0;
    }
    if (per_file_size == 0) {
        fprintf(stderr, "BFFIO: cannot determine file size\n");
        file_set_close(&fset);
        return -1;
    }

    /* Shared atomic flags */
    _Atomic int running = 1;
    _Atomic int ramping = (config->ramp_time_sec > 0) ? 1 : 0;
    _Atomic int workers_done = 0;

    /* Initialize per-thread stats */
    for (int i = 0; i < num_threads; i++) {
        stats_init(&stats[i]);
    }

    /* Determine effective submission depth per thread.
     * Benchmark mode: user's iodepth is the hard cap (like FIO).
     * Target-p99 mode: let AIMD own the depth — use engine queue depth
     * so the worker never self-limits below the AIMD ceiling. */
    int effective_depth = config->iodepth;
    if (config->target_p99_ms > 0.0) {
        aura_ring_stats_t rstats;
        if (aura_get_ring_stats(engine, 0, &rstats, sizeof(rstats)) == 0 &&
            rstats.queue_depth > 0) {
            effective_depth = rstats.queue_depth;
        }
    }

    /* Create thread contexts */
    thread_ctx_t *tctx = calloc((size_t)num_threads, sizeof(thread_ctx_t));
    if (!tctx) {
        file_set_close(&fset);
        return -1;
    }

    for (int i = 0; i < num_threads; i++) {
        tctx[i].thread_id = i;
        tctx[i].config = config;
        tctx[i].engine = engine;
        tctx[i].fds = fset.fds;
        tctx[i].fd_count = fset.fd_count;
        tctx[i].stats = &stats[i];
        tctx[i].running = &running;
        tctx[i].ramping = &ramping;
        tctx[i].workers_done = &workers_done;
        tctx[i].nrfiles = fset.fd_count;
        tctx[i].per_file_size = per_file_size;
        tctx[i].file_service_type = config->file_service_type;
        tctx[i].effective_depth = effective_depth;

        /* Allocate per-file sequential offsets */
        tctx[i].seq_offsets = calloc((size_t)fset.fd_count, sizeof(uint64_t));
        if (!tctx[i].seq_offsets) {
            for (int j = 0; j < i; j++) free(tctx[j].seq_offsets);
            free(tctx);
            file_set_close(&fset);
            return -1;
        }

        /* Stagger sequential offsets within each file by thread */
        for (int f = 0; f < fset.fd_count; f++) {
            tctx[i].seq_offsets[f] = (uint64_t)i * (per_file_size / (uint64_t)num_threads);
            tctx[i].seq_offsets[f] = align_down(tctx[i].seq_offsets[f], config->bs);
        }

        /* Spread threads across files for RR/sequential */
        tctx[i].current_file_idx = i % fset.fd_count;
        tctx[i].file_ios_done = 0;
        tctx[i].file_ios_limit = (per_file_size > config->bs) ? per_file_size / config->bs : 1;
    }

    /* Always spawn pthreads so the main thread can run the timer loop */
    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) {
        free(tctx);
        file_set_close(&fset);
        return -1;
    }

    for (int i = 0; i < num_threads; i++) {
        if (pthread_create(&threads[i], NULL, worker_thread, &tctx[i]) != 0) {
            fprintf(stderr, "BFFIO: failed to create thread %d: %s\n", i, strerror(errno));
            atomic_store(&running, 0);
            for (int j = 0; j < i; j++) {
                pthread_join(threads[j], NULL);
            }
            free(threads);
            free(tctx);
            file_set_close(&fset);
            return -1;
        }
    }

    /* Main thread: timer ticks, ramp management, BW/IOPS sampling */
    uint64_t start_wall = now_ns();
    uint64_t ramp_end_ns = start_wall + (uint64_t)config->ramp_time_sec * 1000000000ULL;
    uint64_t measure_start_ns = 0;
    int total_runtime_sec = config->runtime_sec > 0 ? config->runtime_sec : 0;

    /* Compute maximum timer ticks before giving up */
    int max_ticks;
    if (total_runtime_sec > 0) {
        max_ticks = config->ramp_time_sec + total_runtime_sec + 2;
    } else {
        /* Size-based mode: cap at MAX_SAMPLES (600s) */
        max_ticks = MAX_SAMPLES;
    }

    /* Target-p99 convergence detection: when all active rings reach
     * STEADY or CONVERGED, reset stats so IOPS/BW/latency reflect
     * steady-state throughput at the found depth, not the average
     * across the probing ramp-up. */
    int converged_reset = 0;

    for (int tick = 0; tick < max_ticks; tick++) {
        struct timespec sleep_ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&sleep_ts, NULL);

        uint64_t now = now_ns();

        /* Check if ramp period ended */
        if (atomic_load(&ramping) && now >= ramp_end_ns) {
            atomic_store(&ramping, 0);
            measure_start_ns = now;

            /* Reset all stats to discard ramp-period data */
            for (int i = 0; i < num_threads; i++) {
                stats_reset(&stats[i]);
            }
        }

        /* Target-p99: detect when all active rings have converged.
         * Reset stats once so we only measure steady-state throughput
         * at the AIMD-found depth, not the probing ramp-up. */
        if (config->target_p99_ms > 0.0 && !converged_reset && !atomic_load(&ramping)) {
            int all_stable = 1;
            int ring_count = aura_get_ring_count(engine);
            for (int r = 0; r < ring_count; r++) {
                aura_ring_stats_t rs;
                if (aura_get_ring_stats(engine, r, &rs, sizeof(rs)) == 0 && rs.ops_completed > 0) {
                    if (rs.aimd_phase != AURA_PHASE_STEADY &&
                        rs.aimd_phase != AURA_PHASE_CONVERGED) {
                        all_stable = 0;
                        break;
                    }
                }
            }
            if (all_stable) {
                converged_reset = 1;
                measure_start_ns = now;
                for (int i = 0; i < num_threads; i++) {
                    stats_reset(&stats[i]);
                }
            }
        }

        /* Take BW/IOPS samples (measurement period only) */
        if (!atomic_load(&ramping)) {
            if (measure_start_ns == 0) {
                measure_start_ns = start_wall;
            }
            for (int i = 0; i < num_threads; i++) {
                stats_take_sample(&stats[i]);
            }
        }

        /* Check runtime expiry (measurement time, excluding ramp) */
        if (config->time_based && total_runtime_sec > 0 && !atomic_load(&ramping)) {
            uint64_t elapsed_measure = now - measure_start_ns;
            if (elapsed_measure >= (uint64_t)total_runtime_sec * 1000000000ULL) {
                atomic_store(&running, 0);
                break;
            }
        }

        /* For non time_based: detect when all workers have finished */
        if (!config->time_based && atomic_load(&workers_done) >= num_threads) {
            break;
        }
    }

    /* Signal workers to stop */
    atomic_store(&running, 0);

    /* Join all threads */
    for (int i = 0; i < num_threads; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Final drain to ensure all completions are processed */
    int drain_rc_ = aura_drain(engine, 5000);
    (void)drain_rc_;

    /* Free pre-allocated buffers, per-file offsets, then destroy pools */
    for (int i = 0; i < num_threads; i++) {
        for (int s = 0; s < tctx[i].pool.capacity; s++) {
            if (tctx[i].pool.slots[s].buffer) {
                aura_buffer_free(engine, tctx[i].pool.slots[s].buffer);
                tctx[i].pool.slots[s].buffer = NULL;
            }
        }
        io_ctx_pool_destroy(&tctx[i].pool);
        free(tctx[i].seq_offsets);
    }

    /* Compute actual measurement runtime */
    uint64_t end_wall = now_ns();
    if (measure_start_ns == 0) {
        measure_start_ns = start_wall;
    }
    *runtime_ms = (end_wall - measure_start_ns) / 1000000ULL;

    /* Cleanup */
    free(threads);
    free(tctx);
    file_set_close(&fset);

    return 0;
}
