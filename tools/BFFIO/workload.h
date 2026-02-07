#ifndef BFFIO_WORKLOAD_H
#define BFFIO_WORKLOAD_H

#include "job_parser.h"
#include "stats.h"
#include <auraio.h>
#include <pthread.h>

/* I/O callback context - pre-allocated pool, zero malloc on hot path */
typedef struct {
    thread_stats_t  *stats;
    uint64_t         submit_time_ns;
    size_t           io_size;
    void            *buffer;
    auraio_engine_t *engine;
    int              is_write;
    _Atomic int     *ramping;      /* skip stats during warmup */
    void            *pool;         /* owning io_ctx_pool_t* (for callback) */
    int              pool_idx;     /* index for return to free-stack */
} io_ctx_t;

/* Pre-allocated pool of io_ctx_t (one per thread, sized to iodepth).
 * Thread-safe: auraio_wait() can fire callbacks on any thread, so
 * put() may be called from a different thread than get(). */
typedef struct {
    io_ctx_t *slots;              /* pre-allocated array [capacity] */
    int      *free_stack;         /* indices of available slots */
    int       free_count;         /* current free count */
    int       capacity;           /* == iodepth */
    pthread_spinlock_t lock;      /* protects free_stack/free_count */
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
    int                thread_id;
    const job_config_t *config;
    auraio_engine_t   *engine;
    int               *fds;        /* file descriptors array */
    int                fd_count;
    thread_stats_t    *stats;
    _Atomic int       *running;      /* global run flag */
    _Atomic int       *ramping;      /* true during ramp_time */
    _Atomic int       *workers_done; /* count of finished workers */
    io_ctx_pool_t      pool;       /* pre-allocated io_ctx pool */

    /* Offset tracking */
    uint64_t           seq_offset; /* for sequential patterns */
    uint64_t           file_size;  /* actual file size for offset generation */
} thread_ctx_t;

/* File management results */
typedef struct {
    int  *fds;          /* opened file descriptors */
    int   fd_count;
    char **created_paths; /* paths of files we created (for cleanup) */
    int   created_count;
} file_set_t;

/*
 * Run a single job's workload.
 *
 * Creates/opens files, spawns numjobs worker threads, runs for the
 * configured duration, collects stats, and cleans up.
 *
 * @param job     Job configuration
 * @param engine  AuraIO engine (shared across threads)
 * @param stats   Array of thread_stats_t[numjobs] (pre-allocated by caller)
 * @param runtime_ms  Output: actual measurement runtime in ms (excluding ramp)
 * @return 0 on success, -1 on error
 */
int workload_run(const job_config_t *job, auraio_engine_t *engine,
                 thread_stats_t *stats, uint64_t *runtime_ms);

#endif /* BFFIO_WORKLOAD_H */
