// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


#ifndef BFFIO_WORKLOAD_H
#define BFFIO_WORKLOAD_H

#include "job_parser.h"
#include "stats.h"
#include <aura.h>
#include <pthread.h>

/* I/O callback context - pre-allocated pool, zero malloc on hot path */
typedef struct {
    thread_stats_t *stats;
    uint64_t submit_time_ns;
    size_t io_size;
    void *buffer; /* pre-allocated, reused across I/Os */
    aura_engine_t *engine;
    int is_write;
    _Atomic int *ramping;  /* skip stats during warmup */
    void *pool;            /* owning io_ctx_pool_t* (for callback) */
    int pool_idx;          /* index in pool's slots array */
    _Atomic int next_free; /* lock-free list link (-1 = end) */
} io_ctx_t;

/* Pre-allocated pool of io_ctx_t (one per thread, sized to iodepth).
 * Lock-free Treiber stack: get() pops, put() pushes via CAS.
 * MPSC safe: get() is single-consumer (owning thread),
 * put() can be called from any thread (cross-thread completions). */
typedef struct {
    io_ctx_t *slots;       /* pre-allocated array [capacity] */
    _Atomic int free_head; /* head of lock-free free list (-1 = empty) */
    int capacity;          /* == iodepth */
} io_ctx_pool_t;

/* Initialize pool. Returns 0 on success, -1 on error. */
int io_ctx_pool_init(io_ctx_pool_t *pool, int capacity);

/* Destroy pool and free memory. */
void io_ctx_pool_destroy(io_ctx_pool_t *pool);

/* Get a free io_ctx_t. Returns NULL if pool exhausted. O(1). */
io_ctx_t *io_ctx_pool_get(io_ctx_pool_t *pool);

/* Return io_ctx_t to pool. O(1). */
void io_ctx_pool_put(io_ctx_pool_t *pool, io_ctx_t *ctx);

/* Per-worker thread context */
typedef struct {
    int thread_id;
    const job_config_t *config;
    aura_engine_t *engine;
    int *fds; /* file descriptors array */
    int fd_count;
    thread_stats_t *stats;
    _Atomic int *running;      /* global run flag */
    _Atomic int *ramping;      /* true during ramp_time */
    _Atomic int *workers_done; /* count of finished workers */
    io_ctx_pool_t pool;        /* pre-allocated io_ctx pool */
    int effective_depth;       /* submission cap: iodepth (bench) or engine depth (p99) */

    /* Multi-file support */
    int nrfiles;             /* number of files available */
    uint64_t per_file_size;  /* size of each individual file */
    uint64_t *seq_offsets;   /* per-file sequential offsets [nrfiles] */
    int current_file_idx;    /* current file for RR/sequential */
    uint64_t file_ios_done;  /* I/Os on current file (sequential mode) */
    uint64_t file_ios_limit; /* I/Os per file before switching */
    file_service_type_t file_service_type;

    /* Adaptive latency sampling: budget-based recalibration + PRNG selection.
     * Caps timestamp overhead at ~100K samples/sec regardless of IOPS. */
    uint32_t sample_interval; /* 1 = every I/O, N = every Nth */
    uint64_t sample_calib_ns; /* last recalibration timestamp */
    uint64_t sample_io_count; /* I/Os since last recalibration */
} thread_ctx_t;

/* File management results */
typedef struct {
    int *fds; /* opened file descriptors */
    int fd_count;
    char **created_paths; /* paths of files we created (for cleanup) */
    int created_count;
} file_set_t;

/*
 * Run a single job's workload.
 *
 * Creates/opens files, spawns numjobs worker threads, runs for the
 * configured duration, collects stats, and cleans up.
 *
 * @param job     Job configuration
 * @param engine  Aura engine (shared across threads)
 * @param stats   Array of thread_stats_t[numjobs] (pre-allocated by caller)
 * @param runtime_ms  Output: actual measurement runtime in ms (excluding ramp)
 * @return 0 on success, -1 on error
 */
int workload_run(const job_config_t *job, aura_engine_t *engine, thread_stats_t *stats,
                 uint64_t *runtime_ms);

#endif /* BFFIO_WORKLOAD_H */
