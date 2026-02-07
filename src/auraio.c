/**
 * @file auraio.c
 * @brief Main async I/O API implementation
 *
 * Ties together ring management, buffer pools, and adaptive control
 * into the public auraio_* API.
 */

#define _GNU_SOURCE
#include "../include/auraio.h"
#include "adaptive_buffer.h"
#include "adaptive_engine.h"
#include "adaptive_ring.h"
#include "internal.h"

#include <errno.h>
#include <liburing.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Configuration Constants
 * ============================================================================
 */

#define DEFAULT_QUEUE_DEPTH 256 /**< Default ring queue depth */
#define BUFFER_ALIGNMENT ((size_t)sysconf(_SC_PAGESIZE)) /**< Page alignment for O_DIRECT */
#define TICK_INTERVAL_MS 10 /**< Adaptive tick interval */
/**
 * Maximum number of io_uring rings (1 ring per CPU core).
 *
 * This is a sanity limit for pathological cases where sysconf(_SC_NPROCESSORS_ONLN)
 * returns an unexpectedly large or garbage value. Can safely be increased to any
 * value if you have more than 1024 cores.
 */
#define AURAIO_MAX_RINGS 1024

/* ============================================================================
 * Internal Types
 * ============================================================================
 */

/**
 * Engine structure
 *
 * Contains one ring per CPU core plus shared state.
 */
struct auraio_engine {
    /* Hot path: ring selection (every I/O submission) */
    ring_ctx_t *rings; /**< Array of ring contexts */
    int ring_count; /**< Number of rings */
    _Atomic unsigned int next_ring; /**< Round-robin ring selector (randomized init) */
    _Atomic int avg_ring_pending; /**< Tick-updated average pending across rings */

    /* Hot path: shutdown check (every I/O submission) */
    atomic_bool shutting_down; /**< Shutdown in progress - reject new submissions */
    atomic_bool running; /**< Event loop active flag */
    atomic_bool stop_requested; /**< Stop signal */
    atomic_bool tick_running; /**< Tick thread active */

    /* Configuration (read-only after init, packs with bools above) */
    int queue_depth; /**< Queue depth per ring */
    int event_fd; /**< Unified eventfd for all rings */
    auraio_ring_select_t ring_select; /**< Ring selection mode */
    bool adaptive_enabled; /**< Adaptive tuning enabled */
    bool buffers_registered; /**< True if buffers are registered */
    bool files_registered; /**< True if files are registered */
    bool sqpoll_enabled; /**< True if SQPOLL is active on any ring */

    /* 8-byte aligned fields */
    size_t buffer_alignment; /**< Buffer alignment */
    pthread_t tick_thread; /**< Tick thread handle */

    /* Aggregated statistics */
    atomic_llong total_ops; /**< Total ops completed */
    atomic_llong total_bytes; /**< Total bytes transferred */
    _Atomic uint64_t adaptive_spills; /**< Count of ADAPTIVE ring spills */

    /* Registered buffers and files (protected by reg_lock) */
    pthread_rwlock_t reg_lock; /**< Protects registered_buffers/files access */
    struct iovec *registered_buffers; /**< Copy of registered buffer iovecs */
    int *registered_files; /**< Copy of registered file descriptors */
    int registered_buffer_count; /**< Number of registered buffers */
    int registered_file_count; /**< Number of registered files */

    /* Buffer pool (largest member, placed last) */
    buffer_pool_t buffer_pool; /**< Aligned buffer pool */
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

/**
 * Get number of online CPUs.
 */
static int get_cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > AURAIO_MAX_RINGS) n = AURAIO_MAX_RINGS; /* Sanity limit, not arbitrary cap */
    return (int)n;
}

/** How often to refresh the cached CPU index (every N submissions). */
#define SELECT_RING_REFRESH 32

/** Thread-local cached CPU for ring selection. -1 = not yet cached. */
static _Thread_local int cached_cpu = -1;
/** Countdown to next sched_getcpu() refresh. */
static _Thread_local int cpu_refresh_counter = 0;

/**
 * Select the CPU-local ring.
 *
 * Caches sched_getcpu() result in TLS and refreshes every SELECT_RING_REFRESH
 * submissions. Falls back to thread-ID-based sticky assignment if CPU
 * detection fails or returns an out-of-range value.
 */
static ring_ctx_t *select_ring_cpu_local(auraio_engine_t *engine) {
    if (--cpu_refresh_counter <= 0) {
        cpu_refresh_counter = SELECT_RING_REFRESH;
        cached_cpu = sched_getcpu();
    }

    if (cached_cpu >= 0 && cached_cpu < engine->ring_count) {
        return &engine->rings[cached_cpu];
    }

    /* Fallback: thread-local sticky assignment.
     * Each thread gets assigned to one ring and stays there.
     * Better than round-robin which can cause hot spots. */
    if (cached_cpu < 0) {
        cached_cpu = (int)((uintptr_t)pthread_self() % engine->ring_count);
    }
    return &engine->rings[cached_cpu % engine->ring_count];
}

/** Spill threshold: 75% of in-flight limit (integer arithmetic). */
#define ADAPTIVE_SPILL_THRESHOLD_NUM 3
#define ADAPTIVE_SPILL_THRESHOLD_DEN 4

/** Thread-local xorshift32 PRNG state for power-of-two ring selection. */
static _Thread_local uint32_t tls_rng_state = 0;

/**
 * Fast thread-local xorshift32 PRNG.
 * Seeded lazily from pthread_self() on first use per thread.
 */
static inline uint32_t xorshift_tls(void) {
    uint32_t s = tls_rng_state;
    if (__builtin_expect(s == 0, 0)) s = (uint32_t)(uintptr_t)pthread_self() ^ 0x5A5A5A5A;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    tls_rng_state = s;
    return s;
}

/**
 * Select a ring for the next operation.
 *
 * Dispatches based on the configured ring_select mode:
 * - ADAPTIVE: CPU-local with two-gate spill + power-of-two target selection
 * - CPU_LOCAL: Always use CPU-local ring (original behavior)
 * - ROUND_ROBIN: Atomic round-robin across all rings
 */
static ring_ctx_t *select_ring(auraio_engine_t *engine) {
    if (engine->ring_count == 1) return &engine->rings[0];

    switch (engine->ring_select) {
    case AURAIO_SELECT_ROUND_ROBIN: {
        unsigned idx = atomic_fetch_add_explicit(&engine->next_ring, 1, memory_order_relaxed);
        return &engine->rings[idx % engine->ring_count];
    }

    case AURAIO_SELECT_CPU_LOCAL:
        return select_ring_cpu_local(engine);

    case AURAIO_SELECT_ADAPTIVE:
    default: {
        ring_ctx_t *local = select_ring_cpu_local(engine);
        int pending = atomic_load_explicit(&local->pending_count, memory_order_relaxed);
        int limit = adaptive_get_inflight_limit(&local->adaptive);

        /* Gate 1: Not congested → stay local */
        if (pending < (limit * ADAPTIVE_SPILL_THRESHOLD_NUM / ADAPTIVE_SPILL_THRESHOLD_DEN))
            return local;

        /* Gate 2: Local >> average means we're an outlier, not system-wide
         * pressure. Stay local for cache benefits — other rings are idle.
         * Skip when avg==0 (tick hasn't run yet — no data to judge). */
        int avg = atomic_load_explicit(&engine->avg_ring_pending, memory_order_relaxed);
        if (avg > 0 && pending > avg * 2) return local;

        /* System broadly loaded — power-of-two random choices.
         * Pick two random rings (skipping local), use the lighter one. */
        atomic_fetch_add_explicit(&engine->adaptive_spills, 1, memory_order_relaxed);
        unsigned rc = (unsigned)engine->ring_count;
        unsigned local_idx = (unsigned)(local - engine->rings);
        unsigned a = xorshift_tls() % (rc - 1);
        if (a >= local_idx) a++;
        unsigned b = xorshift_tls() % (rc - 1);
        if (b >= local_idx) b++;
        int pa = atomic_load_explicit(&engine->rings[a].pending_count, memory_order_relaxed);
        int pb = atomic_load_explicit(&engine->rings[b].pending_count, memory_order_relaxed);
        return &engine->rings[pa <= pb ? a : b];
    }
    }
}

/**
 * Helper context for I/O submission operations.
 * Returned by submit_begin(), consumed by submit_end() or submit_abort().
 */
typedef struct {
    ring_ctx_t *ring;
    auraio_request_t *req;
    int op_idx;
} submit_ctx_t;

/**
 * Begin an I/O submission operation.
 *
 * Checks shutdown state, acquires ring lock, ensures submit capacity,
 * and allocates a request slot.
 *
 * @param engine  Engine handle
 * @return Context with req set on success (ring locked), or req=NULL on failure
 *         (ring NOT locked, errno set to ESHUTDOWN/EAGAIN/ENOMEM)
 */
static submit_ctx_t submit_begin(auraio_engine_t *engine) {
    submit_ctx_t ctx = { .ring = NULL, .req = NULL, .op_idx = -1 };

    if (atomic_load_explicit(&engine->shutting_down, memory_order_acquire)) {
        errno = ESHUTDOWN;
        return ctx;
    }

    ring_ctx_t *ring = select_ring(engine);
    pthread_mutex_lock(&ring->lock);

    if (!ring_can_submit(ring)) {
        ring_flush(ring);
        /* Release lock before polling: process_completion() re-acquires ring->lock
         * to update counters, so holding it here would deadlock. */
        pthread_mutex_unlock(&ring->lock);
        ring_poll(ring);
        pthread_mutex_lock(&ring->lock);

        if (!ring_can_submit(ring)) {
            pthread_mutex_unlock(&ring->lock);
            errno = EAGAIN;
            return ctx;
        }
    }

    int op_idx;
    auraio_request_t *req = ring_get_request(ring, &op_idx);
    if (!req) {
        pthread_mutex_unlock(&ring->lock);
        errno = ENOMEM;
        return ctx;
    }

    ctx.ring = ring;
    ctx.req = req;
    ctx.op_idx = op_idx;
    return ctx;
}

/**
 * Complete a successful I/O submission.
 * Flushes if batch threshold reached and releases ring lock.
 */
static void submit_end(submit_ctx_t *ctx) {
    if (ring_should_flush(ctx->ring)) {
        ring_flush(ctx->ring);
    }
    pthread_mutex_unlock(&ctx->ring->lock);
}

/**
 * Abort an I/O submission after submit failure.
 * Returns request slot to pool and releases ring lock.
 */
static void submit_abort(submit_ctx_t *ctx) {
    ring_put_request(ctx->ring, ctx->op_idx);
    pthread_mutex_unlock(&ctx->ring->lock);
}

/**
 * Adaptive tick thread function.
 *
 * Runs every 10ms, calling adaptive_tick() on each ring.
 * Uses clock_nanosleep with TIMER_ABSTIME for EINTR-safe drift-free timing.
 */
static void *tick_thread_func(void *arg) {
    auraio_engine_t *engine = arg;
    struct timespec next_tick;

    /* Initialize absolute time for first tick */
    clock_gettime(CLOCK_MONOTONIC, &next_tick);

    while (atomic_load(&engine->tick_running)) {
        /* Calculate next tick time */
        next_tick.tv_nsec += TICK_INTERVAL_MS * 1000000LL;
        while (next_tick.tv_nsec >= 1000000000LL) {
            next_tick.tv_sec++;
            next_tick.tv_nsec -= 1000000000LL;
        }

        /* Sleep until absolute time - immune to EINTR drift.
         * clock_nanosleep returns 0 on success or an error number (not -1).
         * TIMER_ABSTIME with valid timespec only returns EINTR on signal. */
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_tick, NULL) == EINTR) {
            /* Interrupted by signal, retry */
        }

        /* Tick each ring's adaptive controller and compute avg pending */
        int total_pending = 0;
        for (int i = 0; i < engine->ring_count; i++) {
            adaptive_tick(&engine->rings[i].adaptive);
            total_pending +=
                atomic_load_explicit(&engine->rings[i].pending_count, memory_order_relaxed);
        }
        if (engine->ring_count > 0) {
            atomic_store_explicit(&engine->avg_ring_pending, total_pending / engine->ring_count,
                                  memory_order_relaxed);
        }
    }

    return NULL;
}

/* ============================================================================
 * Options Initialization
 * ============================================================================
 */

void auraio_options_init(auraio_options_t *options) {
    if (!options) {
        return;
    }

    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(auraio_options_t);
    options->queue_depth = DEFAULT_QUEUE_DEPTH;
    options->ring_count = 0; /* Auto-detect */
    options->initial_in_flight = 0; /* Auto: queue_depth / 4 */
    options->min_in_flight = 4;
    options->max_p99_latency_ms = 0; /* Auto */
    options->buffer_alignment = BUFFER_ALIGNMENT;
    options->disable_adaptive = false;

    /* Phase 5: Advanced features */
    options->enable_sqpoll = false; /* Requires root/CAP_SYS_NICE */
    options->sqpoll_idle_ms = 1000; /* 1 second default */

    /* Ring selection (also zero-init safe since AURAIO_SELECT_ADAPTIVE = 0) */
    options->ring_select = AURAIO_SELECT_ADAPTIVE;
}

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

auraio_engine_t *auraio_create(void) {
    return auraio_create_with_options(NULL);
}

auraio_engine_t *auraio_create_with_options(const auraio_options_t *options) {
    /* Use defaults if no options provided */
    auraio_options_t default_opts;
    if (!options) {
        auraio_options_init(&default_opts);
        options = &default_opts;
    }

    /* Validate options */
    if (options->queue_depth < 0 || options->queue_depth > 32768) {
        errno = EINVAL;
        return NULL;
    }
    if (options->buffer_alignment > 0 &&
        (options->buffer_alignment & (options->buffer_alignment - 1)) != 0) {
        errno = EINVAL; /* Must be power of 2 */
        return NULL;
    }
    if (options->ring_count < 0 || options->ring_count > 256) {
        errno = EINVAL;
        return NULL;
    }

    auraio_engine_t *engine = calloc(1, sizeof(*engine));
    if (!engine) {
        return NULL;
    }

    /* Initialize event_fd to invalid state */
    engine->event_fd = -1;

    /* Store configuration */
    engine->queue_depth = options->queue_depth > 0 ? options->queue_depth : DEFAULT_QUEUE_DEPTH;
    engine->buffer_alignment =
        options->buffer_alignment > 0 ? options->buffer_alignment : BUFFER_ALIGNMENT;
    engine->adaptive_enabled = !options->disable_adaptive;
    engine->ring_select = options->ring_select;

    /* Initialize atomic variables.
     * Randomize next_ring to avoid ring-0-first bias on initial spills. */
    atomic_init(&engine->next_ring, (int)((uintptr_t)engine >> 6));
    atomic_init(&engine->avg_ring_pending, 0);
    atomic_init(&engine->running, false);
    atomic_init(&engine->stop_requested, false);
    atomic_init(&engine->shutting_down, false);
    atomic_init(&engine->tick_running, false);
    atomic_init(&engine->total_ops, 0);
    atomic_init(&engine->total_bytes, 0);

    /* Initialize registration lock */
    pthread_rwlock_init(&engine->reg_lock, NULL);

    /* Initialize buffer pool */
    if (buffer_pool_init(&engine->buffer_pool, engine->buffer_alignment) != 0) {
        goto cleanup_engine;
    }

    /* Create rings */
    engine->ring_count = options->ring_count > 0 ? options->ring_count : get_cpu_count();
    engine->rings = calloc(engine->ring_count, sizeof(ring_ctx_t));
    if (!engine->rings) {
        goto cleanup_buffer_pool;
    }

    /* Prepare ring options */
    ring_options_t ring_opts = { .enable_sqpoll = options->enable_sqpoll,
                                 .sqpoll_idle_ms =
                                     options->sqpoll_idle_ms > 0 ? options->sqpoll_idle_ms : 1000 };

    /* Initialize each ring */
    for (int i = 0; i < engine->ring_count; i++) {
        if (ring_init(&engine->rings[i], engine->queue_depth, i, &ring_opts) != 0) {
            /* Destroy already-initialized rings */
            for (int j = 0; j < i; j++) {
                ring_destroy(&engine->rings[j]);
            }
            goto cleanup_rings;
        }
        engine->rings[i].ring_idx = i;

        /* Track if any ring has SQPOLL enabled */
        if (engine->rings[i].sqpoll_enabled) {
            engine->sqpoll_enabled = true;
        }

        /* Apply custom adaptive settings if provided */
        if (options->initial_in_flight > 0) {
            atomic_store(&engine->rings[i].adaptive.current_in_flight_limit,
                         options->initial_in_flight);
        }
        if (options->min_in_flight > 0) {
            engine->rings[i].adaptive.min_in_flight = options->min_in_flight;
        }
        if (options->max_p99_latency_ms > 0) {
            engine->rings[i].adaptive.max_p99_ms = options->max_p99_latency_ms;
        }
    }

    /* Create unified eventfd for event loop integration.
     * This single fd is registered with all io_uring rings, so any ring
     * completion will wake up the user's event loop (epoll/select/etc). */
    engine->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (engine->event_fd < 0) {
        goto cleanup_rings_full;
    }

    /* Register eventfd with each ring */
    for (int i = 0; i < engine->ring_count; i++) {
        int ret = io_uring_register_eventfd(&engine->rings[i].ring, engine->event_fd);
        if (ret != 0) {
            errno = -ret;
            goto cleanup_eventfd;
        }
    }

    /* Start tick thread if adaptive is enabled */
    if (engine->adaptive_enabled) {
        atomic_store(&engine->tick_running, true);
        if (pthread_create(&engine->tick_thread, NULL, tick_thread_func, engine) != 0) {
            atomic_store(&engine->tick_running, false);
            goto cleanup_eventfd; /* Don't skip eventfd cleanup */
        }
    }

    return engine;

cleanup_eventfd:
    close(engine->event_fd);
cleanup_rings_full:
    for (int i = 0; i < engine->ring_count; i++) {
        ring_destroy(&engine->rings[i]);
    }
cleanup_rings:
    free(engine->rings);
cleanup_buffer_pool:
    buffer_pool_destroy(&engine->buffer_pool);
cleanup_engine:
    pthread_rwlock_destroy(&engine->reg_lock);
    free(engine);
    return NULL;
}

void auraio_destroy(auraio_engine_t *engine) {
    if (!engine) {
        return;
    }

    /* Signal shutdown - new submissions will be rejected */
    atomic_store_explicit(&engine->shutting_down, true, memory_order_release);

    /* Stop tick thread (atomic_exchange ensures only one thread joins) */
    if (engine->adaptive_enabled && atomic_exchange(&engine->tick_running, false)) {
        pthread_join(engine->tick_thread, NULL);
    }

    /* Stop event loop if running */
    auraio_stop(engine);

    /* Unregister buffers and files before destroying rings */
    if (engine->buffers_registered) {
        auraio_unregister_buffers(engine);
    }
    if (engine->files_registered) {
        auraio_unregister_files(engine);
    }

    /* Close unified eventfd */
    if (engine->event_fd >= 0) {
        close(engine->event_fd);
    }

    /* Destroy all rings (waits for pending ops) */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_destroy(&engine->rings[i]);
    }
    free(engine->rings);

    /* Destroy buffer pool */
    buffer_pool_destroy(&engine->buffer_pool);

    pthread_rwlock_destroy(&engine->reg_lock);
    free(engine);
}

/* ============================================================================
 * Core I/O Operations
 * ============================================================================
 */

auraio_request_t *auraio_read(auraio_engine_t *engine, int fd, auraio_buf_t buf, size_t len,
                              off_t offset, auraio_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || len == 0) {
        errno = EINVAL;
        return NULL;
    }

    /* Validate buffer based on type.
     * Registered buffers require reg_lock to prevent use-after-free if another
     * thread calls auraio_unregister_buffers() between validation and submission. */
    bool hold_reg_lock = (buf.type == AURAIO_BUF_REGISTERED);

    if (buf.type == AURAIO_BUF_UNREGISTERED) {
        if (!buf.u.ptr) {
            errno = EINVAL;
            return NULL;
        }
    } else if (hold_reg_lock) {
        pthread_rwlock_rdlock(&engine->reg_lock);
        if (!engine->buffers_registered) {
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = ENOENT; /* No buffers registered */
            return NULL;
        }
        if (buf.u.fixed.index < 0 || buf.u.fixed.index >= engine->registered_buffer_count) {
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = EINVAL;
            return NULL;
        }
        struct iovec *iov = &engine->registered_buffers[buf.u.fixed.index];
        /* Overflow-safe bounds check: avoid offset + len which can wrap */
        if (buf.u.fixed.offset >= iov->iov_len || len > iov->iov_len - buf.u.fixed.offset) {
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = EOVERFLOW;
            return NULL;
        }
    } else {
        errno = EINVAL; /* Invalid buffer type */
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine);
    if (!ctx.req) {
        if (hold_reg_lock) pthread_rwlock_unlock(&engine->reg_lock);
        return NULL;
    }

    ctx.req->fd = fd;
    ctx.req->len = len;
    ctx.req->offset = offset;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;

    int ret;
    if (buf.type == AURAIO_BUF_UNREGISTERED) {
        ctx.req->buffer = buf.u.ptr;
        ret = ring_submit_read(ctx.ring, ctx.req);
    } else {
        struct iovec *iov = &engine->registered_buffers[buf.u.fixed.index];
        ctx.req->buffer = (char *)iov->iov_base + buf.u.fixed.offset;
        ctx.req->buf_index = buf.u.fixed.index;
        ctx.req->buf_offset = buf.u.fixed.offset;
        ret = ring_submit_read_fixed(ctx.ring, ctx.req);
    }

    if (ret != 0) {
        submit_abort(&ctx);
        if (hold_reg_lock) pthread_rwlock_unlock(&engine->reg_lock);
        return NULL;
    }

    submit_end(&ctx);
    if (hold_reg_lock) pthread_rwlock_unlock(&engine->reg_lock);
    return ctx.req;
}

auraio_request_t *auraio_write(auraio_engine_t *engine, int fd, auraio_buf_t buf, size_t len,
                               off_t offset, auraio_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || len == 0) {
        errno = EINVAL;
        return NULL;
    }

    /* Validate buffer based on type.
     * See auraio_read() for reg_lock rationale. */
    bool hold_reg_lock = (buf.type == AURAIO_BUF_REGISTERED);

    if (buf.type == AURAIO_BUF_UNREGISTERED) {
        if (!buf.u.ptr) {
            errno = EINVAL;
            return NULL;
        }
    } else if (hold_reg_lock) {
        pthread_rwlock_rdlock(&engine->reg_lock);
        if (!engine->buffers_registered) {
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = ENOENT; /* No buffers registered */
            return NULL;
        }
        if (buf.u.fixed.index < 0 || buf.u.fixed.index >= engine->registered_buffer_count) {
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = EINVAL;
            return NULL;
        }
        struct iovec *iov = &engine->registered_buffers[buf.u.fixed.index];
        /* Overflow-safe bounds check: avoid offset + len which can wrap */
        if (buf.u.fixed.offset >= iov->iov_len || len > iov->iov_len - buf.u.fixed.offset) {
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = EOVERFLOW;
            return NULL;
        }
    } else {
        errno = EINVAL; /* Invalid buffer type */
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine);
    if (!ctx.req) {
        if (hold_reg_lock) pthread_rwlock_unlock(&engine->reg_lock);
        return NULL;
    }

    ctx.req->fd = fd;
    ctx.req->len = len;
    ctx.req->offset = offset;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;

    int ret;
    if (buf.type == AURAIO_BUF_UNREGISTERED) {
        ctx.req->buffer = buf.u.ptr;
        ret = ring_submit_write(ctx.ring, ctx.req);
    } else {
        struct iovec *iov = &engine->registered_buffers[buf.u.fixed.index];
        ctx.req->buffer = (char *)iov->iov_base + buf.u.fixed.offset;
        ctx.req->buf_index = buf.u.fixed.index;
        ctx.req->buf_offset = buf.u.fixed.offset;
        ret = ring_submit_write_fixed(ctx.ring, ctx.req);
    }

    if (ret != 0) {
        submit_abort(&ctx);
        if (hold_reg_lock) pthread_rwlock_unlock(&engine->reg_lock);
        return NULL;
    }

    submit_end(&ctx);
    if (hold_reg_lock) pthread_rwlock_unlock(&engine->reg_lock);
    return ctx.req;
}

auraio_request_t *auraio_fsync(auraio_engine_t *engine, int fd, auraio_callback_t callback,
                               void *user_data) {
    return auraio_fsync_ex(engine, fd, AURAIO_FSYNC_DEFAULT, callback, user_data);
}

auraio_request_t *auraio_fsync_ex(auraio_engine_t *engine, int fd, auraio_fsync_flags_t flags,
                                  auraio_callback_t callback, void *user_data) {
    if (!engine || fd < 0) {
        errno = EINVAL;
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine);
    if (!ctx.req) {
        return NULL;
    }

    ctx.req->fd = fd;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;

    int ret;
    if (flags & AURAIO_FSYNC_DATASYNC) {
        ret = ring_submit_fdatasync(ctx.ring, ctx.req);
    } else {
        ret = ring_submit_fsync(ctx.ring, ctx.req);
    }

    if (ret != 0) {
        submit_abort(&ctx);
        return NULL;
    }

    submit_end(&ctx);
    return ctx.req;
}

/* ============================================================================
 * Vectored I/O Operations
 * ============================================================================
 */

auraio_request_t *auraio_readv(auraio_engine_t *engine, int fd, const struct iovec *iov, int iovcnt,
                               off_t offset, auraio_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || !iov || iovcnt <= 0 || iovcnt > IOV_MAX) {
        errno = EINVAL;
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine);
    if (!ctx.req) {
        return NULL;
    }

    ctx.req->fd = fd;
    ctx.req->iov = iov;
    ctx.req->iovcnt = iovcnt;
    ctx.req->offset = offset;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;

    if (ring_submit_readv(ctx.ring, ctx.req) != 0) {
        submit_abort(&ctx);
        return NULL;
    }

    submit_end(&ctx);
    return ctx.req;
}

auraio_request_t *auraio_writev(auraio_engine_t *engine, int fd, const struct iovec *iov,
                                int iovcnt, off_t offset, auraio_callback_t callback,
                                void *user_data) {
    if (!engine || fd < 0 || !iov || iovcnt <= 0 || iovcnt > IOV_MAX) {
        errno = EINVAL;
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine);
    if (!ctx.req) {
        return NULL;
    }

    ctx.req->fd = fd;
    ctx.req->iov = iov;
    ctx.req->iovcnt = iovcnt;
    ctx.req->offset = offset;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;

    if (ring_submit_writev(ctx.ring, ctx.req) != 0) {
        submit_abort(&ctx);
        return NULL;
    }

    submit_end(&ctx);
    return ctx.req;
}

/* ============================================================================
 * Cancellation
 * ============================================================================
 */

int auraio_cancel(auraio_engine_t *engine, auraio_request_t *req) {
    if (!engine || !req) {
        errno = EINVAL;
        return (-1);
    }

    if (atomic_load_explicit(&engine->shutting_down, memory_order_acquire)) {
        errno = ESHUTDOWN;
        return (-1);
    }

    /* Check if request is still pending */
    if (!atomic_load_explicit(&req->pending, memory_order_acquire)) {
        errno = EALREADY; /* Already completed */
        return (-1);
    }

    /* Get the ring that owns this request */
    if (req->ring_idx < 0 || req->ring_idx >= engine->ring_count) {
        errno = EINVAL;
        return (-1);
    }

    ring_ctx_t *ring = &engine->rings[req->ring_idx];
    pthread_mutex_lock(&ring->lock);

    /* Double-check it's still pending while holding lock */
    if (!atomic_load_explicit(&req->pending, memory_order_acquire)) {
        pthread_mutex_unlock(&ring->lock);
        errno = EALREADY;
        return (-1);
    }

    /* Get a request slot for the cancel operation */
    int op_idx;
    auraio_request_t *cancel_req = ring_get_request(ring, &op_idx);
    if (!cancel_req) {
        pthread_mutex_unlock(&ring->lock);
        errno = ENOMEM;
        return (-1);
    }

    /* Submit cancel */
    cancel_req->ring_idx = ring->ring_idx;
    if (ring_submit_cancel(ring, cancel_req, req) != 0) {
        ring_put_request(ring, op_idx);
        pthread_mutex_unlock(&ring->lock);
        return (-1);
    }

    /* Flush immediately to expedite cancellation */
    ring_flush(ring);

    pthread_mutex_unlock(&ring->lock);
    return (0);
}

/* ============================================================================
 * Request Introspection
 * ============================================================================
 */

bool auraio_request_pending(const auraio_request_t *req) {
    if (!req) {
        return false;
    }
    /* Cast away const for atomic access - atomic_load is semantically read-only */
    return atomic_load_explicit((atomic_bool *)&req->pending, memory_order_acquire);
}

int auraio_request_fd(const auraio_request_t *req) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    return req->fd;
}

void *auraio_request_user_data(const auraio_request_t *req) {
    if (!req) {
        errno = EINVAL;
        return NULL;
    }
    return req->user_data;
}

/* ============================================================================
 * Event Processing
 * ============================================================================
 */

int auraio_get_poll_fd(auraio_engine_t *engine) {
    if (!engine || engine->ring_count == 0) {
        errno = EINVAL;
        return (-1);
    }

    /* Return the unified eventfd that is registered with all io_uring rings.
     * When ANY ring completes an operation, this fd becomes readable.
     * This allows proper event loop integration in multi-core setups. */
    return engine->event_fd;
}

int auraio_poll(auraio_engine_t *engine) {
    if (!engine) {
        return (0);
    }

    /* Drain eventfd to clear POLLIN state after completions.
     * Without this, epoll would immediately return again even
     * though we've processed all available CQEs.
     * EAGAIN expected if no data. Other errors (EBADF etc) indicate
     * broken state with no recovery - ignore and continue. */
    uint64_t eventfd_val;
    if (read(engine->event_fd, &eventfd_val, sizeof(eventfd_val)) < 0) {
        /* Intentionally ignored - see comment above */
    }

    int total = 0;
    int skipped = 0;

    /* First pass: try-lock to avoid blocking on contended rings.
     * Note: ring_poll manages its own internal locking via process_completion,
     * so we only hold the lock for ring_flush, then release before ring_poll. */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];

        if (pthread_mutex_trylock(&ring->lock) == 0) {
            /* Got the lock - flush, then release before poll */
            ring_flush(ring);
            pthread_mutex_unlock(&ring->lock);

            int completed = ring_poll(ring);

            if (completed > 0) {
                total += completed;
                atomic_fetch_add(&engine->total_ops, completed);
            }
        } else {
            /* Ring is contended - skip for now */
            skipped++;
        }
    }

    /* Second pass: if we skipped rings and got no completions,
     * do blocking acquire to ensure forward progress */
    if (skipped > 0 && total == 0) {
        for (int i = 0; i < engine->ring_count; i++) {
            ring_ctx_t *ring = &engine->rings[i];

            pthread_mutex_lock(&ring->lock);
            ring_flush(ring);
            pthread_mutex_unlock(&ring->lock);

            int completed = ring_poll(ring);

            if (completed > 0) {
                total += completed;
                atomic_fetch_add(&engine->total_ops, completed);
            }
        }
    }

    return total;
}

int auraio_wait(auraio_engine_t *engine, int timeout_ms) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    /* Drain eventfd to clear POLLIN state after completions.
     * Without this, epoll would immediately return again even
     * though we've processed all available CQEs.
     * EAGAIN expected if no data. Other errors (EBADF etc) indicate
     * broken state with no recovery - ignore and continue. */
    uint64_t eventfd_val;
    if (read(engine->event_fd, &eventfd_val, sizeof(eventfd_val)) < 0) {
        /* Intentionally ignored - see comment above */
    }

    /* Single-pass flush+poll: flush each ring and immediately poll it.
     * Returns as soon as any ring produces completions — remaining rings
     * get their flush on the next auraio_wait() call. This merges two
     * full passes (flush all, poll all) into one early-exit pass. */
    int total = 0;
    int first_pending = -1; /* First ring with pending ops (for blocking) */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        pthread_mutex_lock(&ring->lock);
        ring_flush(ring);
        bool has_pending = (atomic_load_explicit(&ring->pending_count, memory_order_relaxed) > 0);
        pthread_mutex_unlock(&ring->lock);

        int n = ring_poll(ring);
        if (n > 0) {
            total += n;
            atomic_fetch_add(&engine->total_ops, n);
        }
        if (has_pending && first_pending < 0) {
            first_pending = i;
        }
    }
    if (total > 0) {
        return total;
    }

    /* Nothing ready — block on first ring with pending ops.
     * first_pending is captured during the flush pass above. */
    if (first_pending < 0) {
        return (0);
    }

    {
        int completed = ring_wait(&engine->rings[first_pending], timeout_ms);
        if (completed > 0) {
            atomic_fetch_add(&engine->total_ops, completed);

            /* Also poll other rings that may have completed while we blocked */
            for (int j = 0; j < engine->ring_count; j++) {
                if (j == first_pending) continue;
                int more = ring_poll(&engine->rings[j]);
                if (more > 0) {
                    completed += more;
                    atomic_fetch_add(&engine->total_ops, more);
                }
            }
            return completed;
        }
        if (completed < 0) {
            return (-1);
        }
    }

    return (0);
}

void auraio_run(auraio_engine_t *engine) {
    if (!engine) {
        return;
    }

    atomic_store(&engine->running, true);
    atomic_store(&engine->stop_requested, false);

    while (!atomic_load(&engine->stop_requested)) {
        int completed = auraio_wait(engine, 100);

        /* Check if we have any pending work.
         * pending_count is atomic — no lock needed for a simple read. */
        bool has_pending = false;
        for (int i = 0; i < engine->ring_count; i++) {
            if (atomic_load_explicit(&engine->rings[i].pending_count, memory_order_acquire) > 0) {
                has_pending = true;
                break;
            }
        }

        /* If no pending and no completions and stop requested, exit */
        if (!has_pending && completed == 0 && atomic_load(&engine->stop_requested)) {
            break;
        }
    }

    atomic_store(&engine->running, false);
}

void auraio_stop(auraio_engine_t *engine) {
    if (!engine) {
        return;
    }
    atomic_store(&engine->stop_requested, true);
}

int auraio_drain(auraio_engine_t *engine, int timeout_ms) {
    if (!engine) {
        errno = EINVAL;
        return -1;
    }

    int64_t deadline_ns = 0;
    if (timeout_ms > 0) {
        deadline_ns = get_time_ns() + (int64_t)timeout_ms * 1000000LL;
    }

    int total = 0;
    for (;;) {
        /* Check if all rings are drained */
        bool has_pending = false;
        for (int i = 0; i < engine->ring_count; i++) {
            ring_ctx_t *ring = &engine->rings[i];
            pthread_mutex_lock(&ring->lock);
            if (atomic_load_explicit(&ring->pending_count, memory_order_relaxed) > 0)
                has_pending = true;
            pthread_mutex_unlock(&ring->lock);
            if (has_pending) break;
        }
        if (!has_pending) return total;

        /* Calculate remaining timeout for this iteration */
        int wait_ms;
        if (timeout_ms < 0) {
            wait_ms = 100; /* Poll in 100ms intervals when no deadline */
        } else if (timeout_ms == 0) {
            /* Non-blocking: just poll once */
            int n = auraio_poll(engine);
            return n > 0 ? total + n : total;
        } else {
            int64_t remaining_ns = deadline_ns - get_time_ns();
            if (remaining_ns <= 0) {
                errno = ETIMEDOUT;
                return total;
            }
            wait_ms = (int)(remaining_ns / 1000000LL);
            if (wait_ms <= 0) wait_ms = 1;
            if (wait_ms > 100) wait_ms = 100; /* Cap per-iteration wait */
        }

        int n = auraio_wait(engine, wait_ms);
        if (n > 0) total += n;
    }
}

/* ============================================================================
 * Managed Buffers
 * ============================================================================
 */

void *auraio_buffer_alloc(auraio_engine_t *engine, size_t size) {
    if (!engine || size == 0) {
        errno = EINVAL;
        return NULL;
    }

    return buffer_pool_alloc(&engine->buffer_pool, size);
}

void auraio_buffer_free(auraio_engine_t *engine, void *buf, size_t size) {
    if (!engine || !buf) {
        return;
    }

    buffer_pool_free(&engine->buffer_pool, buf, size);
}

/* ============================================================================
 * Registered Buffers (Phase 5)
 * ============================================================================
 */

int auraio_register_buffers(auraio_engine_t *engine, const struct iovec *iovs, int count) {
    if (!engine || !iovs || count <= 0 || (size_t)count > SIZE_MAX / sizeof(struct iovec)) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (engine->buffers_registered) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EBUSY; /* Already registered - must unregister first */
        return (-1);
    }

    /* Store a copy of the iovecs for later use in read/write_fixed */
    engine->registered_buffers = malloc((size_t)count * sizeof(struct iovec));
    if (!engine->registered_buffers) {
        pthread_rwlock_unlock(&engine->reg_lock);
        return (-1);
    }
    memcpy(engine->registered_buffers, iovs, (size_t)count * sizeof(struct iovec));
    engine->registered_buffer_count = count;

    /* Register with all rings - they share the registration */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        pthread_mutex_lock(&ring->lock);

        int ret = io_uring_register_buffers(&ring->ring, iovs, count);
        if (ret < 0) {
            pthread_mutex_unlock(&ring->lock);
            /* Unregister from already-registered rings */
            for (int j = 0; j < i; j++) {
                ring_ctx_t *prev_ring = &engine->rings[j];
                pthread_mutex_lock(&prev_ring->lock);
                io_uring_unregister_buffers(&prev_ring->ring);
                pthread_mutex_unlock(&prev_ring->lock);
            }
            free(engine->registered_buffers);
            engine->registered_buffers = NULL;
            engine->registered_buffer_count = 0;
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = -ret;
            return (-1);
        }

        pthread_mutex_unlock(&ring->lock);
    }

    engine->buffers_registered = true;
    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

int auraio_unregister_buffers(auraio_engine_t *engine) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (!engine->buffers_registered) {
        pthread_rwlock_unlock(&engine->reg_lock);
        return (0); /* Nothing to unregister */
    }

    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        pthread_mutex_lock(&ring->lock);
        io_uring_unregister_buffers(&ring->ring);
        pthread_mutex_unlock(&ring->lock);
    }

    free(engine->registered_buffers);
    engine->registered_buffers = NULL;
    engine->registered_buffer_count = 0;
    engine->buffers_registered = false;

    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

/* ============================================================================
 * Registered Files
 * ============================================================================
 */

int auraio_register_files(auraio_engine_t *engine, const int *fds, int count) {
    if (!engine || !fds || count <= 0 || (size_t)count > SIZE_MAX / sizeof(int)) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (engine->files_registered) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EBUSY; /* Already registered - must unregister first */
        return (-1);
    }

    /* Store a copy of the fds */
    engine->registered_files = malloc((size_t)count * sizeof(int));
    if (!engine->registered_files) {
        pthread_rwlock_unlock(&engine->reg_lock);
        return (-1);
    }
    memcpy(engine->registered_files, fds, (size_t)count * sizeof(int));
    engine->registered_file_count = count;

    /* Register with all rings */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        pthread_mutex_lock(&ring->lock);

        int ret = io_uring_register_files(&ring->ring, fds, count);
        if (ret < 0) {
            pthread_mutex_unlock(&ring->lock);
            /* Unregister from already-registered rings */
            for (int j = 0; j < i; j++) {
                ring_ctx_t *prev_ring = &engine->rings[j];
                pthread_mutex_lock(&prev_ring->lock);
                io_uring_unregister_files(&prev_ring->ring);
                pthread_mutex_unlock(&prev_ring->lock);
            }
            free(engine->registered_files);
            engine->registered_files = NULL;
            engine->registered_file_count = 0;
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = -ret;
            return (-1);
        }

        pthread_mutex_unlock(&ring->lock);
    }

    engine->files_registered = true;
    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

int auraio_update_file(auraio_engine_t *engine, int index, int fd) {
    if (!engine || index < 0) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (!engine->files_registered) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = ENOENT;
        return (-1);
    }

    if (index >= engine->registered_file_count) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EINVAL;
        return (-1);
    }

    /* Update in all rings, rolling back on failure */
    int old_fd = engine->registered_files[index];
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        pthread_mutex_lock(&ring->lock);

        int ret = io_uring_register_files_update(&ring->ring, index, &fd, 1);
        if (ret < 0) {
            pthread_mutex_unlock(&ring->lock);
            /* Roll back already-updated rings to old fd */
            for (int j = 0; j < i; j++) {
                ring_ctx_t *prev_ring = &engine->rings[j];
                pthread_mutex_lock(&prev_ring->lock);
                io_uring_register_files_update(&prev_ring->ring, index, &old_fd, 1);
                pthread_mutex_unlock(&prev_ring->lock);
            }
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = -ret;
            return (-1);
        }

        pthread_mutex_unlock(&ring->lock);
    }

    /* Update our copy */
    engine->registered_files[index] = fd;
    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

int auraio_unregister_files(auraio_engine_t *engine) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (!engine->files_registered) {
        pthread_rwlock_unlock(&engine->reg_lock);
        return (0); /* Nothing to unregister */
    }

    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        pthread_mutex_lock(&ring->lock);
        io_uring_unregister_files(&ring->ring);
        pthread_mutex_unlock(&ring->lock);
    }

    free(engine->registered_files);
    engine->registered_files = NULL;
    engine->registered_file_count = 0;
    engine->files_registered = false;

    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

/* ============================================================================
 * Statistics
 * ============================================================================
 */

const char *auraio_version(void) {
    return AURAIO_VERSION_STRING;
}

int auraio_version_int(void) {
    return AURAIO_VERSION;
}

void auraio_get_stats(const auraio_engine_t *engine, auraio_stats_t *stats) {
    if (!engine || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    /* Aggregate stats from all rings */
    int total_in_flight = 0;
    int total_optimal_inflight = 0;
    int total_batch_size = 0;
    double total_throughput = 0.0;
    double max_p99 = 0.0;

    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];

        /* Lock ring while reading stats to prevent data races with
         * completion handlers and tick thread */
        pthread_mutex_lock(&ring->lock);

        stats->ops_completed += ring->ops_completed;
        stats->bytes_transferred += ring->bytes_completed;
        total_in_flight += atomic_load_explicit(&ring->pending_count, memory_order_relaxed);

        /* Get adaptive controller values */
        adaptive_controller_t *ctrl = &ring->adaptive;
        total_optimal_inflight += atomic_load(&ctrl->current_in_flight_limit);
        total_batch_size += atomic_load(&ctrl->current_batch_threshold);

        /* Use memory_order_acquire to pair with release in tick thread,
         * ensuring consistent reads on ARM/PowerPC with weak memory ordering. */
        double throughput =
            atomic_load_explicit(&ctrl->current_throughput_bps, memory_order_acquire);
        double p99 = atomic_load_explicit(&ctrl->current_p99_ms, memory_order_acquire);
        total_throughput += throughput;
        if (p99 > max_p99) {
            max_p99 = p99;
        }

        pthread_mutex_unlock(&ring->lock);
    }

    stats->current_in_flight = total_in_flight;
    stats->optimal_in_flight = total_optimal_inflight;
    /* Guard against division by zero if no rings are active */
    stats->optimal_batch_size = engine->ring_count > 0 ? total_batch_size / engine->ring_count : 0;
    stats->current_throughput_bps = total_throughput;
    stats->p99_latency_ms = max_p99;
    stats->adaptive_spills = atomic_load_explicit(&engine->adaptive_spills, memory_order_relaxed);
}

/* Verify public and internal histogram constants stay in sync */
_Static_assert(AURAIO_HISTOGRAM_BUCKETS == LATENCY_BUCKET_COUNT,
               "Public AURAIO_HISTOGRAM_BUCKETS must match internal LATENCY_BUCKET_COUNT");
_Static_assert(
    AURAIO_HISTOGRAM_BUCKET_WIDTH_US == LATENCY_BUCKET_WIDTH_US,
    "Public AURAIO_HISTOGRAM_BUCKET_WIDTH_US must match internal LATENCY_BUCKET_WIDTH_US");

/* Verify public phase constants match internal enum */
_Static_assert(AURAIO_PHASE_BASELINE == ADAPTIVE_PHASE_BASELINE,
               "AURAIO_PHASE_BASELINE must match internal enum");
_Static_assert(AURAIO_PHASE_PROBING == ADAPTIVE_PHASE_PROBING,
               "AURAIO_PHASE_PROBING must match internal enum");
_Static_assert(AURAIO_PHASE_STEADY == ADAPTIVE_PHASE_STEADY,
               "AURAIO_PHASE_STEADY must match internal enum");
_Static_assert(AURAIO_PHASE_BACKOFF == ADAPTIVE_PHASE_BACKOFF,
               "AURAIO_PHASE_BACKOFF must match internal enum");
_Static_assert(AURAIO_PHASE_SETTLING == ADAPTIVE_PHASE_SETTLING,
               "AURAIO_PHASE_SETTLING must match internal enum");
_Static_assert(AURAIO_PHASE_CONVERGED == ADAPTIVE_PHASE_CONVERGED,
               "AURAIO_PHASE_CONVERGED must match internal enum");

int auraio_get_ring_count(const auraio_engine_t *engine) {
    if (!engine) return 0;
    return engine->ring_count;
}

int auraio_get_ring_stats(const auraio_engine_t *engine, int ring_idx, auraio_ring_stats_t *stats) {
    if (!engine || !stats) return -1;
    if (ring_idx < 0 || ring_idx >= engine->ring_count) {
        memset(stats, 0, sizeof(*stats));
        return -1;
    }

    ring_ctx_t *ring = &engine->rings[ring_idx];
    pthread_mutex_lock(&ring->lock);

    stats->ops_completed = ring->ops_completed;
    stats->bytes_transferred = ring->bytes_completed;
    stats->pending_count = atomic_load_explicit(&ring->pending_count, memory_order_relaxed);
    stats->queue_depth = ring->max_requests;

    /* Use acquire ordering on all adaptive controller atomics to pair with
     * release in the tick thread.  The tick thread writes these without
     * holding ring->lock, so the mutex alone does not establish
     * happens-before; acquire on the loads does. */
    adaptive_controller_t *ctrl = &ring->adaptive;
    stats->in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_acquire);
    stats->batch_threshold =
        atomic_load_explicit(&ctrl->current_batch_threshold, memory_order_acquire);
    stats->p99_latency_ms = atomic_load_explicit(&ctrl->current_p99_ms, memory_order_acquire);
    stats->throughput_bps =
        atomic_load_explicit(&ctrl->current_throughput_bps, memory_order_acquire);
    stats->aimd_phase = atomic_load_explicit(&ctrl->phase, memory_order_acquire);
    memset(stats->_reserved, 0, sizeof(stats->_reserved));

    pthread_mutex_unlock(&ring->lock);
    return 0;
}

int auraio_get_histogram(const auraio_engine_t *engine, int ring_idx, auraio_histogram_t *hist) {
    if (!engine || !hist) return -1;
    if (ring_idx < 0 || ring_idx >= engine->ring_count) {
        memset(hist, 0, sizeof(*hist));
        return -1;
    }

    ring_ctx_t *ring = &engine->rings[ring_idx];
    pthread_mutex_lock(&ring->lock);

    /* Read from the active histogram.  Individual bucket loads are atomic but
     * the overall snapshot is approximate — see auraio_histogram_t docs. */
    adaptive_histogram_t *active = adaptive_hist_active(&ring->adaptive.hist_pair);
    for (int i = 0; i < AURAIO_HISTOGRAM_BUCKETS; i++) {
        hist->buckets[i] = atomic_load_explicit(&active->buckets[i], memory_order_relaxed);
    }
    hist->overflow = atomic_load_explicit(&active->overflow, memory_order_relaxed);
    hist->total_count = atomic_load_explicit(&active->total_count, memory_order_relaxed);
    hist->bucket_width_us = LATENCY_BUCKET_WIDTH_US;
    hist->max_tracked_us = LATENCY_MAX_US;
    memset(hist->_reserved, 0, sizeof(hist->_reserved));

    pthread_mutex_unlock(&ring->lock);
    return 0;
}

int auraio_get_buffer_stats(const auraio_engine_t *engine, auraio_buffer_stats_t *stats) {
    if (!engine || !stats) return -1;

    memset(stats, 0, sizeof(*stats));

    const buffer_pool_t *pool = &engine->buffer_pool;
    stats->total_allocated_bytes =
        atomic_load_explicit(&pool->total_allocated, memory_order_relaxed);
    stats->total_buffers = atomic_load_explicit(&pool->total_buffers, memory_order_relaxed);
    stats->shard_count = pool->shard_count;
    return 0;
}

const char *auraio_phase_name(int phase) {
    return adaptive_phase_name((adaptive_phase_t)phase);
}
