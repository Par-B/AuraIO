// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file aura.c
 * @brief Main async I/O API implementation
 *
 * Ties together ring management, buffer pools, and adaptive control
 * into the public aura_* API.
 */

#define _GNU_SOURCE
#include "../include/aura.h"
#include "adaptive_buffer.h"
#include "adaptive_engine.h"
#include "adaptive_ring.h"
#include "internal.h"
#include "log.h"

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
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

/* Verify AURA_O_* constants match the system's O_* values.
 * These are hardcoded in aura.h for header-only use, but they are
 * architecture-specific on Linux (differ on MIPS, SPARC, Alpha). */
_Static_assert(AURA_O_RDONLY == O_RDONLY, "AURA_O_RDONLY mismatch");
_Static_assert(AURA_O_WRONLY == O_WRONLY, "AURA_O_WRONLY mismatch");
_Static_assert(AURA_O_RDWR == O_RDWR, "AURA_O_RDWR mismatch");
_Static_assert(AURA_O_CREAT == O_CREAT, "AURA_O_CREAT mismatch");
_Static_assert(AURA_O_TRUNC == O_TRUNC, "AURA_O_TRUNC mismatch");
_Static_assert(AURA_O_APPEND == O_APPEND, "AURA_O_APPEND mismatch");
#ifdef O_DIRECT
_Static_assert(AURA_O_DIRECT == O_DIRECT, "AURA_O_DIRECT mismatch");
#endif

/* ============================================================================
 * Configuration Constants
 * ============================================================================
 */

#define DEFAULT_QUEUE_DEPTH 1024 /**< Default ring queue depth */
static size_t get_page_size(void) {
    static _Atomic size_t cached;
    size_t val = atomic_load_explicit(&cached, memory_order_relaxed);
    if (val == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        val = (ps > 0) ? (size_t)ps : 4096;
        atomic_store_explicit(&cached, val, memory_order_relaxed);
    }
    return val;
}
#define BUFFER_ALIGNMENT get_page_size() /**< Page alignment for O_DIRECT */
#define TICK_INTERVAL_MS 10 /**< Adaptive tick interval */
/**
 * Maximum number of io_uring rings (1 ring per CPU core).
 *
 * This is a sanity limit for pathological cases where sysconf(_SC_NPROCESSORS_ONLN)
 * returns an unexpectedly large or garbage value. Can safely be increased to any
 * value if you have more than 1024 cores.
 */
#define AURA_MAX_RINGS 1024

/* ============================================================================
 * Internal Types
 * ============================================================================
 */

/**
 * Engine structure
 *
 * Contains one ring per CPU core plus shared state.
 */
struct aura_engine {
    /* === Cache line 0: submission hot path (every I/O submission) === */
    ring_ctx_t *rings; /**< Array of ring contexts */
    int ring_count; /**< Number of rings */
    int queue_depth; /**< Queue depth per ring */
    _Atomic unsigned int next_ring; /**< Round-robin ring selector (randomized init) */
    _Atomic int avg_ring_pending; /**< Tick-updated average pending across rings */
    atomic_bool shutting_down; /**< Shutdown in progress - reject new submissions */
    aura_ring_select_t ring_select; /**< Ring selection mode */
    _Atomic int fatal_submit_errno; /**< Latched fatal submit error from ring_flush */

    /* === Cache line 1+: registration path === */
    _Alignas(64) _Atomic bool buffers_registered; /**< True if buffers are registered */
    _Atomic bool files_registered; /**< True if files are registered */
    bool buffers_unreg_pending; /**< Deferred buffer unregister requested */
    bool files_unreg_pending; /**< Deferred file unregister requested */
    pthread_rwlock_t reg_lock; /**< Protects registered_buffers/files access */
    struct iovec *registered_buffers; /**< Copy of registered buffer iovecs */
    int *registered_files; /**< Copy of registered file descriptors */
    int registered_buffer_count; /**< Number of registered buffers */
    int registered_file_count; /**< Number of registered files */

    /* === Cold: init-only or rarely accessed === */
    _Alignas(64) atomic_bool running; /**< Event loop active flag */
    atomic_bool stop_requested; /**< Stop signal */
    atomic_bool tick_running; /**< Tick thread active */
    bool adaptive_enabled; /**< Adaptive tuning enabled */
    bool sqpoll_enabled; /**< True if SQPOLL is active on any ring */
    int event_fd; /**< Unified eventfd for all rings */
    bool eventfd_registered; /**< True if eventfd is registered with rings */
    size_t buffer_alignment; /**< Buffer alignment */
    pthread_t tick_thread; /**< Tick thread handle */
    _Atomic uint64_t adaptive_spills; /**< Count of ADAPTIVE ring spills */

    /* Buffer pool (largest member, placed last) */
    buffer_pool_t buffer_pool; /**< Aligned buffer pool */
    buf_size_map_t buf_size_map; /**< ptr → size_class lookup for buffer_free */
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
    if (n > AURA_MAX_RINGS) n = AURA_MAX_RINGS; /* Sanity limit, not arbitrary cap */
    return (int)n;
}

/** How often to refresh the cached CPU index (every N submissions). */
#define SELECT_RING_REFRESH 32

/** Thread-local cached CPU for ring selection. -1 = not yet cached. */
static _Thread_local int cached_cpu = -1;
/** Countdown to next sched_getcpu() refresh. */
static _Thread_local int cpu_refresh_counter = 0;

/** Thread-local link chain state.
 *  When a linked request is submitted, we pin subsequent submissions to the
 *  same ring and suppress flushing until the chain ends. */
static _Thread_local ring_ctx_t *tls_link_ring = NULL;
static _Thread_local int tls_link_depth = 0;

/** Pointer to the ring used by the last submission on this thread.
 *  Used by aura_request_set_linked() to set tls_link_ring without
 *  needing the engine pointer. */
static _Thread_local ring_ctx_t *tls_last_ring = NULL;

/** Thread-local owned ring for THREAD_LOCAL mode.
 *  Once claimed, a thread always submits to and polls from this ring. */
static _Thread_local ring_ctx_t *tls_owned_ring = NULL;


/**
 * Select the CPU-local ring.
 *
 * Caches sched_getcpu() result in TLS and refreshes every SELECT_RING_REFRESH
 * submissions. Falls back to thread-ID-based sticky assignment if CPU
 * detection fails or returns an out-of-range value.
 */
static ring_ctx_t *select_ring_cpu_local(aura_engine_t *engine) {
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
    if (cached_cpu < 0 || cached_cpu >= engine->ring_count) {
        cached_cpu = (int)((uintptr_t)pthread_self() % engine->ring_count);
    }
    return &engine->rings[cached_cpu];
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
    if (__builtin_expect(s == 0, 0)) {
        s = (uint32_t)(uintptr_t)pthread_self() ^ 0x5A5A5A5A;
        if (s == 0) s = 1; /* xorshift32 has 0 as fixed point */
    }
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    tls_rng_state = s;
    return s;
}

/**
 * Select a thread-local ring.
 *
 * On first call, atomically claims a ring index. If this thread is the sole
 * owner, enables single_thread mode (no locks). If shared (more threads than
 * rings), keeps mutexes active.
 */
static ring_ctx_t *select_ring_thread_local(aura_engine_t *engine) {
    /* Validate tls_owned_ring belongs to this engine (handles stale TLS
     * from a previously destroyed engine in the same thread). */
    if (tls_owned_ring &&
        tls_owned_ring >= engine->rings &&
        tls_owned_ring < engine->rings + engine->ring_count) {
        return tls_owned_ring;
    }
    tls_owned_ring = NULL;

    unsigned idx = atomic_fetch_add_explicit(&engine->next_ring, 1, memory_order_relaxed);
    ring_ctx_t *ring = &engine->rings[idx % engine->ring_count];

    int prev = atomic_fetch_add_explicit(&ring->owner_count, 1, memory_order_acq_rel);
    if (prev == 0) {
        /* First (sole) owner — enable the ring and set single_thread */
        if (ring->needs_enable) {
            ring_enable(ring);
        }
        ring->single_thread = true;
    } else {
        /* Shared ring — keep mutexes, disable single_thread if it was set */
        ring->single_thread = false;
    }

    tls_owned_ring = ring;
    return ring;
}

/**
 * Select a ring for the next operation.
 *
 * Dispatches based on the configured ring_select mode:
 * - ADAPTIVE: CPU-local with two-gate spill + power-of-two target selection
 * - CPU_LOCAL: Always use CPU-local ring (original behavior)
 * - ROUND_ROBIN: Atomic round-robin across all rings
 * - THREAD_LOCAL: Thread-local exclusive ring ownership
 */
static ring_ctx_t *select_ring(aura_engine_t *engine) {
    if (engine->ring_count == 1 && !engine->rings[0].needs_enable)
        return &engine->rings[0];

    switch (engine->ring_select) {
    case AURA_SELECT_THREAD_LOCAL:
        return select_ring_thread_local(engine);

    case AURA_SELECT_ROUND_ROBIN: {
        unsigned idx = atomic_fetch_add_explicit(&engine->next_ring, 1, memory_order_relaxed);
        return &engine->rings[idx % engine->ring_count];
    }

    case AURA_SELECT_CPU_LOCAL:
        return select_ring_cpu_local(engine);

    case AURA_SELECT_ADAPTIVE:
    default: {
        ring_ctx_t *local = select_ring_cpu_local(engine);
        int pending = atomic_load_explicit(&local->pending_count, memory_order_relaxed);
        int limit = adaptive_get_inflight_limit(&local->adaptive);

        /* Gate 1: Not congested → stay local */
        if (pending < (limit * ADAPTIVE_SPILL_THRESHOLD_NUM / ADAPTIVE_SPILL_THRESHOLD_DEN))
            return local;

        /* Gate 2: If load is broadly distributed (local within 2x of average),
         * spilling won't help — stay local for cache benefits.
         * If local ring is an outlier (>2x average), proceed to spill.
         * Skip when avg==0 (tick hasn't run yet — no data to judge). */
        int avg = atomic_load_explicit(&engine->avg_ring_pending, memory_order_relaxed);
        if (avg > 0 && pending <= avg * 2) return local;

        /* System broadly loaded — power-of-two random choices.
         * Pick two random rings (skipping local), use the lighter one. */
        atomic_fetch_add_explicit(&engine->adaptive_spills, 1, memory_order_relaxed);
        unsigned rc = (unsigned)engine->ring_count;
        unsigned local_idx = (unsigned)(local - engine->rings);
        unsigned a = xorshift_tls() % (rc - 1);
        if (a >= local_idx) a++;
        unsigned b;
        if (rc - 1 >= 2) {
            /* Pick b != local_idx and b != a */
            b = xorshift_tls() % (rc - 2);
            unsigned skip1 = local_idx < a ? local_idx : a;
            unsigned skip2 = local_idx < a ? a : local_idx;
            if (b >= skip1) b++;
            if (b >= skip2) b++;
        } else {
            b = a; /* Only one non-local ring */
        }
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
    aura_engine_t *engine;
    ring_ctx_t *ring;
    aura_request_t *req;
    int op_idx;
} submit_ctx_t;

/*
 * File resolution guard for RAII-style handling of registered file lookup.
 * Used by I/O functions to manage rwlock acquisition/release around file resolution.
 */
typedef struct {
    aura_engine_t *engine;
    int submit_fd;
    bool uses_registered_file;
    bool locked;
} file_resolve_guard_t;

/* Forward declarations for internal helpers used by aura_destroy() */
static int request_unregister_buffers(aura_engine_t *engine);
static int request_unregister_files(aura_engine_t *engine);

/* Forward declarations for submit context helpers */
static submit_ctx_t submit_begin(aura_engine_t *engine, bool allow_poll);
static void submit_end(submit_ctx_t *ctx);
static void submit_abort(submit_ctx_t *ctx);

static bool flush_error_is_fatal(int err) {
    return err != EAGAIN && err != EBUSY && err != EINTR;
}

static int get_fatal_submit_errno(const aura_engine_t *engine) {
    return atomic_load_explicit(&engine->fatal_submit_errno, memory_order_acquire);
}

static void latch_fatal_submit_errno(aura_engine_t *engine, int err) {
    if (err <= 0) {
        err = EIO;
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&engine->fatal_submit_errno, &expected, err,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        return;
    }

    if (engine->event_fd >= 0) {
        uint64_t one = 1;
        ssize_t rc = write(engine->event_fd, &one, sizeof(one));
        (void)rc;
    }
}

/**
 * Drain eventfd to clear POLLIN state after completions.
 * Without this, poll/epoll would immediately return again even though
 * we've processed all available CQEs.  Must be called in ALL modes
 * because aura_wait() uses poll(eventfd) internally regardless of
 * single_thread setting.
 */
static inline void drain_eventfd(aura_engine_t *engine) {
    if (engine->event_fd >= 0) {
        uint64_t eventfd_val;
        if (read(engine->event_fd, &eventfd_val, sizeof(eventfd_val)) < 0) {
            /* Intentionally ignored — EAGAIN expected if no data */
        }
    }
}

/**
 * Flush a ring and handle fatal errors. Returns true if a fatal error was latched.
 */
static inline bool flush_ring_checked(aura_engine_t *engine, ring_ctx_t *ring) {
    if (ring_flush(ring) < 0) {
        if (flush_error_is_fatal(errno)) {
            latch_fatal_submit_errno(engine, errno);
            return true;
        }
        errno = 0;
    }
    return false;
}

static bool check_fatal_submit_errno(aura_engine_t *engine) {
    int fatal = get_fatal_submit_errno(engine);
    if (fatal != 0) {
        errno = fatal;
    }
    return fatal != 0;
}

static bool resolve_registered_file_locked(const aura_engine_t *engine, int fd, int *fixed_fd) {
    if (!atomic_load_explicit(&engine->files_registered, memory_order_relaxed) ||
        engine->files_unreg_pending) {
        return false;
    }

    for (int i = 0; i < engine->registered_file_count; i++) {
        if (engine->registered_files[i] == fd) {
            *fixed_fd = i;
            return true;
        }
    }

    return false;
}

/*
 * Begin file resolution: check if fd is registered, acquire lock if needed.
 * Returns a guard that must be cleaned up with resolve_file_end().
 */
static file_resolve_guard_t resolve_file_begin(aura_engine_t *engine, int fd) {
    file_resolve_guard_t guard = {
        .engine = engine, .submit_fd = fd, .uses_registered_file = false, .locked = false
    };

    if (atomic_load_explicit(&engine->files_registered, memory_order_acquire)) {
        pthread_rwlock_rdlock(&engine->reg_lock);
        guard.locked = true;
        guard.uses_registered_file = resolve_registered_file_locked(engine, fd, &guard.submit_fd);

        /* If file not registered, release lock immediately */
        if (!guard.uses_registered_file) {
            pthread_rwlock_unlock(&engine->reg_lock);
            guard.locked = false;
        }
    }

    return guard;
}

/*
 * End file resolution: release lock if still held.
 */
static void resolve_file_end(file_resolve_guard_t *guard) {
    if (guard->locked) {
        pthread_rwlock_unlock(&guard->engine->reg_lock);
        guard->locked = false;
    }
}

/*
 * Function pointer types for ring submit operations.
 */
typedef int (*ring_submit_fn_t)(ring_ctx_t *ctx, aura_request_t *req);
typedef int (*ring_submit_fixed_fn_t)(ring_ctx_t *ctx, aura_request_t *req);

/*
 * Helper for unregistered buffer I/O (used by aura_read and aura_write).
 */
static aura_request_t *submit_unregistered_io(aura_engine_t *engine, int fd, void *buffer,
                                              size_t len, off_t offset, aura_callback_t callback,
                                              void *user_data, ring_submit_fn_t submit_fn) {

    file_resolve_guard_t file_guard = resolve_file_begin(engine, fd);

    submit_ctx_t ctx = submit_begin(engine, !file_guard.uses_registered_file);
    if (!ctx.req) {
        resolve_file_end(&file_guard);
        return NULL;
    }

    ctx.req->fd = file_guard.submit_fd;
    ctx.req->original_fd = fd;
    ctx.req->len = len;
    ctx.req->offset = offset;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;
    ctx.req->uses_registered_file = file_guard.uses_registered_file;

    ctx.req->buffer = buffer;
    if (submit_fn(ctx.ring, ctx.req) != 0) {
        submit_abort(&ctx);
        resolve_file_end(&file_guard);
        return NULL;
    }

    submit_end(&ctx);
    resolve_file_end(&file_guard);
    return ctx.req;
}

/*
 * Registered buffer info returned by validation.
 */
typedef struct {
    struct iovec *reg_iov;
} registered_buf_info_t;

/*
 * Validate registered buffer bounds (used by aura_read and aura_write).
 * Returns reg_iov on success, NULL on error (with errno set and lock released).
 */
static registered_buf_info_t validate_registered_buffer(aura_engine_t *engine, int buf_index,
                                                        size_t buf_offset, size_t len) {

    registered_buf_info_t info = { .reg_iov = NULL };

    pthread_rwlock_rdlock(&engine->reg_lock);

    if (!atomic_load_explicit(&engine->buffers_registered, memory_order_relaxed)) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = ENOENT; /* No buffers registered */
        return info;
    }

    if (engine->buffers_unreg_pending) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EBUSY; /* Unregister in progress */
        return info;
    }

    if (buf_index < 0 || buf_index >= engine->registered_buffer_count) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EINVAL;
        return info;
    }

    struct iovec *reg_iov = &engine->registered_buffers[buf_index];

    /* Overflow-safe bounds check: avoid offset + len which can wrap */
    if (buf_offset >= reg_iov->iov_len || len > reg_iov->iov_len - buf_offset) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EOVERFLOW;
        return info;
    }

    /* Success: keep lock held, return iov */
    info.reg_iov = reg_iov;
    return info;
}

/*
 * Submit registered buffer I/O (used by aura_read and aura_write).
 * Assumes engine->reg_lock is already held (from validate_registered_buffer).
 */
static aura_request_t *submit_registered_buffer_io(aura_engine_t *engine, int fd,
                                                   struct iovec *reg_iov, int buf_index,
                                                   size_t buf_offset, size_t len, off_t offset,
                                                   aura_callback_t callback, void *user_data,
                                                   ring_submit_fixed_fn_t submit_fn) {

    submit_ctx_t ctx = submit_begin(engine, false);
    if (!ctx.req) {
        pthread_rwlock_unlock(&engine->reg_lock);
        return NULL;
    }

    int submit_fd = fd;
    bool use_registered_file = resolve_registered_file_locked(engine, fd, &submit_fd);

    ctx.req->fd = submit_fd;
    ctx.req->original_fd = fd;
    ctx.req->len = len;
    ctx.req->offset = offset;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;
    ctx.req->uses_registered_buffer = true;
    ctx.req->uses_registered_file = use_registered_file;

    atomic_fetch_add_explicit(&ctx.ring->fixed_buf_inflight, 1, memory_order_relaxed);

    ctx.req->buffer = (char *)reg_iov->iov_base + buf_offset;
    ctx.req->buf_index = buf_index;
    ctx.req->buf_offset = buf_offset;

    if (submit_fn(ctx.ring, ctx.req) != 0) {
        atomic_fetch_sub_explicit(&ctx.ring->fixed_buf_inflight, 1, memory_order_relaxed);
        submit_abort(&ctx);
        pthread_rwlock_unlock(&engine->reg_lock);
        return NULL;
    }

    submit_end(&ctx);
    pthread_rwlock_unlock(&engine->reg_lock);
    return ctx.req;
}

static uint32_t fixed_buf_inflight_total(const aura_engine_t *engine) {
    uint32_t total = 0;
    for (int i = 0; i < engine->ring_count; i++) {
        total += atomic_load_explicit(&engine->rings[i].fixed_buf_inflight, memory_order_acquire);
    }
    return total;
}

static uint32_t fixed_file_inflight_total(const aura_engine_t *engine) {
    uint32_t total = 0;
    for (int i = 0; i < engine->ring_count; i++) {
        total += atomic_load_explicit(&engine->rings[i].fixed_file_inflight, memory_order_acquire);
    }
    return total;
}

static int finalize_deferred_unregistration(aura_engine_t *engine) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (atomic_load_explicit(&engine->buffers_registered, memory_order_relaxed) &&
        engine->buffers_unreg_pending && fixed_buf_inflight_total(engine) == 0) {
        /* No ring_lock needed: reg_lock(write) prevents new registered-buffer
         * submissions, and inflight count is zero so no completions will touch
         * the registered buffer table.  Avoiding ring_lock here prevents a
         * lock-ordering inversion (reg_lock → ring_lock vs the submission
         * path's ring_lock → reg_lock). */
        for (int i = 0; i < engine->ring_count; i++) {
            io_uring_unregister_buffers(&engine->rings[i].ring);
        }

        free(engine->registered_buffers);
        engine->registered_buffers = NULL;
        engine->registered_buffer_count = 0;
        atomic_store_explicit(&engine->buffers_registered, false, memory_order_release);
        engine->buffers_unreg_pending = false;
    }

    if (atomic_load_explicit(&engine->files_registered, memory_order_relaxed) &&
        engine->files_unreg_pending && fixed_file_inflight_total(engine) == 0) {
        /* Same rationale as above — no ring_lock needed. */
        for (int i = 0; i < engine->ring_count; i++) {
            io_uring_unregister_files(&engine->rings[i].ring);
        }

        free(engine->registered_files);
        engine->registered_files = NULL;
        engine->registered_file_count = 0;
        atomic_store_explicit(&engine->files_registered, false, memory_order_release);
        engine->files_unreg_pending = false;
    }

    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

static int maybe_finalize_deferred_unregistration(aura_engine_t *engine) {
    /* Fast path: if nothing is registered, there's nothing to finalize.
     * Avoids rwlock acquisition on every poll/wait call. */
    if (!atomic_load_explicit(&engine->buffers_registered, memory_order_acquire) &&
        !atomic_load_explicit(&engine->files_registered, memory_order_acquire)) {
        return (0);
    }

    bool need_finalize = false;

    pthread_rwlock_rdlock(&engine->reg_lock);
    need_finalize = engine->buffers_unreg_pending || engine->files_unreg_pending;
    pthread_rwlock_unlock(&engine->reg_lock);

    if (!need_finalize) {
        return (0);
    }

    return finalize_deferred_unregistration(engine);
}

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
static submit_ctx_t submit_begin(aura_engine_t *engine, bool allow_poll) {
    submit_ctx_t ctx = { .engine = engine, .ring = NULL, .req = NULL, .op_idx = -1 };

    if (check_fatal_submit_errno(engine)) {
        return ctx;
    }

    if (atomic_load_explicit(&engine->shutting_down, memory_order_acquire)) {
        errno = ESHUTDOWN;
        return ctx;
    }

    /* If we're in a link chain, pin to the same ring so the linked SQEs
     * share one submission queue. */
    ring_ctx_t *ring = tls_link_ring ? tls_link_ring : select_ring(engine);

    /* Thread-local single-thread fast path: no lock, inline capacity check.
     * select_ring_thread_local() has validated tls_owned_ring ownership. */
    if (ring->single_thread && engine->ring_select == AURA_SELECT_THREAD_LOCAL &&
        !tls_link_ring) {
        if (!ring_can_submit(ring)) {
            ring_flush_fast(ring);
            ring_drain_cqes_fast(ring);
            if (!ring_can_submit(ring)) {
                errno = EAGAIN;
                return ctx;
            }
        }

        int op_idx;
        aura_request_t *req = ring_get_request(ring, &op_idx);
        if (!req) {
            errno = ENOMEM;
            return ctx;
        }

        ctx.ring = ring;
        ctx.req = req;
        ctx.op_idx = op_idx;
        return ctx;
    }

    ring_lock(ring);

    if (!ring_can_submit(ring)) {
        if (ring_flush(ring) < 0) {
            if (flush_error_is_fatal(errno)) {
                latch_fatal_submit_errno(engine, errno);
                ring_unlock(ring);
                return ctx;
            }
            errno = 0;
        }

        if (allow_poll) {
            /* Release lock before polling: process_completion() re-acquires ring->lock
             * to update counters, so holding it here would deadlock. */
            ring_unlock(ring);
            ring_poll(ring);
            ring_lock(ring);
        }

        if (!ring_can_submit(ring)) {
            ring_unlock(ring);
            errno = EAGAIN;
            return ctx;
        }
    }

    int op_idx;
    aura_request_t *req = ring_get_request(ring, &op_idx);
    if (!req) {
        ring_unlock(ring);
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
    if (ctx->req->uses_registered_file) {
        atomic_fetch_add_explicit(&ctx->ring->fixed_file_inflight, 1, memory_order_relaxed);
    }

    /* Thread-local single-thread fast path: no lock to release */
    if (ctx->ring->single_thread && tls_link_ring == NULL) {
        tls_last_ring = ctx->ring;
        if (ring_should_flush(ctx->ring)) {
            ring_flush_fast(ctx->ring);
        }
        return;
    }

    /* Save the ring for aura_request_set_linked() to use. */
    tls_last_ring = ctx->ring;

    if (tls_link_ring != NULL) {
        /* We're inside a link chain. This is the chain tail (last op)
         * since set_linked() was not called on *this* request yet—
         * it was called on the *previous* one. Force flush to submit the
         * entire chain together, then clear TLS state. */
        if (ring_flush(ctx->ring) < 0 && flush_error_is_fatal(errno)) {
            latch_fatal_submit_errno(ctx->engine, errno);
        }
        tls_link_ring = NULL;
        tls_link_depth = 0;
        ring_clear_last_sqe();
        ring_unlock(ctx->ring);
        return;
    }

    if (ring_should_flush(ctx->ring)) {
        if (ring_flush(ctx->ring) < 0 && flush_error_is_fatal(errno)) {
            latch_fatal_submit_errno(ctx->engine, errno);
        }
    }
    ring_unlock(ctx->ring);
}

/**
 * Abort an I/O submission after submit failure.
 * Returns request slot to pool and releases ring lock.
 */
static void submit_abort(submit_ctx_t *ctx) {
    ring_put_request(ctx->ring, ctx->op_idx);
    /* If we're in a link chain, best-effort flush the partial chain
     * and clear TLS state so subsequent submissions aren't pinned. */
    if (tls_link_ring != NULL) {
        (void)ring_flush(ctx->ring);
        tls_link_ring = NULL;
        tls_link_depth = 0;
    }
    ring_unlock(ctx->ring); /* No-op for single_thread rings */
}

/**
 * Adaptive tick thread function.
 *
 * Runs every 10ms, calling adaptive_tick() on each ring.
 * Uses clock_nanosleep with TIMER_ABSTIME for EINTR-safe drift-free timing.
 */
static void *tick_thread_func(void *arg) {
    aura_engine_t *engine = arg;
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
            int pending =
                atomic_load_explicit(&engine->rings[i].pending_count, memory_order_relaxed);
            adaptive_tick(&engine->rings[i].adaptive, pending);
            total_pending += pending;
            if (pending >
                atomic_load_explicit(&engine->rings[i].peak_pending_count, memory_order_relaxed)) {
                atomic_store_explicit(&engine->rings[i].peak_pending_count, pending,
                                      memory_order_relaxed);
            }
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

void aura_options_init(aura_options_t *options) {
    if (!options) {
        return;
    }

    memset(options, 0, sizeof(*options));
    options->struct_size = sizeof(aura_options_t);
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

    /* Ring selection (also zero-init safe since AURA_SELECT_ADAPTIVE = 0) */
    options->ring_select = AURA_SELECT_ADAPTIVE;
}

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

aura_engine_t *aura_create(void) {
    return aura_create_with_options(NULL);
}

/**
 * Stage 1: Validate options and initialize engine core state.
 * Returns NULL on failure with errno set.
 */
static aura_engine_t *validate_and_init_engine_core(const aura_options_t *options) {
    /* Validate struct_size for ABI forward-compatibility.
     * Accept structs from older headers (smaller) — fields beyond
     * the caller's struct_size are treated as zero (defaults).
     * Reject struct_size == 0 (uninitialized) or larger than we know
     * (future header with fields we can't validate). */
    if (options->struct_size == 0 || options->struct_size > sizeof(aura_options_t)) {
        errno = EINVAL;
        return NULL;
    }

    /* Reject non-zero reserved fields for forward-compatibility.
     * Future versions may assign meaning to these; non-zero values from
     * a caller built against an older header would silently misbehave.
     * Only check reserved fields within the caller's struct_size. */
    size_t reserved_offset = offsetof(aura_options_t, _reserved);
    if (options->struct_size > reserved_offset) {
        size_t reserved_bytes = options->struct_size - reserved_offset;
        size_t reserved_count = reserved_bytes / sizeof(options->_reserved[0]);
        for (size_t i = 0; i < reserved_count; i++) {
            if (options->_reserved[i] != 0) {
                errno = EINVAL;
                return NULL;
            }
        }
    }

    /* Validate options.
     * queue_depth == 0 means "use default"; explicit values must be >= 4
     * because the AIMD controller needs at least 4 request slots. */
    if (options->queue_depth < 0 || options->queue_depth > 32768 ||
        (options->queue_depth > 0 && options->queue_depth < 4)) {
        errno = EINVAL;
        return NULL;
    }
    if (options->buffer_alignment > 0 &&
        (options->buffer_alignment & (options->buffer_alignment - 1)) != 0) {
        errno = EINVAL; /* Must be power of 2 */
        return NULL;
    }
    if (options->ring_count < 0 || options->ring_count > AURA_MAX_RINGS) {
        errno = EINVAL;
        return NULL;
    }

    /* Validate adaptive tuning limits */
    int qdepth = options->queue_depth > 0 ? options->queue_depth : DEFAULT_QUEUE_DEPTH;
    if (options->min_in_flight != 0 &&
        (options->min_in_flight < 1 || options->min_in_flight > qdepth)) {
        errno = EINVAL;
        return NULL;
    }
    int effective_min = options->min_in_flight > 0 ? options->min_in_flight : 4;
    if (options->initial_in_flight != 0 &&
        (options->initial_in_flight < effective_min || options->initial_in_flight > qdepth)) {
        errno = EINVAL;
        return NULL;
    }

    aura_engine_t *engine = calloc(1, sizeof(*engine));
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
    atomic_init(&engine->fatal_submit_errno, 0);

    /* Initialize registration lock */
    int rwlock_ret = pthread_rwlock_init(&engine->reg_lock, NULL);
    if (rwlock_ret != 0) {
        errno = rwlock_ret;
        free(engine);
        return NULL;
    }

    return engine;
}

/**
 * Stage 2: Initialize buffer pool and size tracking map.
 * Returns 0 on success, -1 on failure with errno set.
 */
static int init_engine_buffer_pool(aura_engine_t *engine) {
    if (buffer_pool_init(&engine->buffer_pool, engine->buffer_alignment) != 0) {
        return -1;
    }
    if (buf_size_map_init(&engine->buf_size_map) != 0) {
        buffer_pool_destroy(&engine->buffer_pool);
        return -1;
    }
    return 0;
}

/**
 * Stage 3: Initialize io_uring rings.
 * Returns 0 on success, -1 on failure with errno set.
 */
static int init_engine_rings(aura_engine_t *engine, const aura_options_t *options) {
    /* Create rings */
    engine->ring_count = options->ring_count > 0 ? options->ring_count : get_cpu_count();
    engine->rings = calloc(engine->ring_count, sizeof(ring_ctx_t));
    if (!engine->rings) {
        return -1;
    }

    /* Prepare ring options */
    bool thread_local_mode = (options->ring_select == AURA_SELECT_THREAD_LOCAL);
    ring_options_t ring_opts = { .enable_sqpoll = options->enable_sqpoll,
                                 .sqpoll_idle_ms =
                                     options->sqpoll_idle_ms > 0 ? options->sqpoll_idle_ms : 1000,
                                 .enable_single_issuer = thread_local_mode,
                                 .start_disabled = thread_local_mode };

    /* Initialize each ring */
    for (int i = 0; i < engine->ring_count; i++) {
        if (ring_init(&engine->rings[i], engine->queue_depth, i, &ring_opts) != 0) {
            /* Destroy already-initialized rings */
            for (int j = 0; j < i; j++) {
                ring_destroy(&engine->rings[j]);
            }
            free(engine->rings);
            engine->rings = NULL;
            return -1;
        }
        engine->rings[i].ring_idx = i;
        engine->rings[i].single_thread = options->single_thread;

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

    return 0;
}

/**
 * Stage 4: Create unified eventfd and register with all rings.
 * Returns 0 on success, -1 on failure with errno set.
 */
static int register_eventfd_with_rings(aura_engine_t *engine) {
    /* Create unified eventfd for event loop integration.
     * Always create the fd (for aura_get_poll_fd() compatibility),
     * but only register with rings if not in THREAD_LOCAL mode. */
    engine->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (engine->event_fd < 0) {
        return -1;
    }

    /* In THREAD_LOCAL mode, skip registration — threads poll their own rings
     * directly without needing eventfd wakeups. Registration is deferred
     * until aura_get_poll_fd() is called (lazy registration). */
    if (engine->ring_select == AURA_SELECT_THREAD_LOCAL) {
        engine->eventfd_registered = false;
        return 0;
    }

    /* Register eventfd with each ring */
    for (int i = 0; i < engine->ring_count; i++) {
        int ret = io_uring_register_eventfd(&engine->rings[i].ring, engine->event_fd);
        if (ret != 0) {
            errno = -ret;
            for (int j = 0; j < i; j++) {
                io_uring_unregister_eventfd(&engine->rings[j].ring);
            }
            close(engine->event_fd);
            engine->event_fd = -1;
            return -1;
        }
    }

    engine->eventfd_registered = true;
    return 0;
}

/**
 * Stage 5: Start adaptive tick thread.
 * Returns 0 on success, -1 on failure with errno set.
 */
static int start_tick_thread(aura_engine_t *engine) {
    if (!engine->adaptive_enabled) {
        return 0; /* Not an error - adaptive is disabled */
    }

    atomic_store(&engine->tick_running, true);
    int ret = pthread_create(&engine->tick_thread, NULL, tick_thread_func, engine);
    if (ret != 0) {
        atomic_store(&engine->tick_running, false);
        errno = ret;
        return -1;
    }

    return 0;
}

aura_engine_t *aura_create_with_options(const aura_options_t *options) {
    /* Use defaults if no options provided */
    aura_options_t default_opts;
    if (!options) {
        aura_options_init(&default_opts);
        options = &default_opts;
    }

    /* Stage 1: Validate options and initialize engine core */
    aura_engine_t *engine = validate_and_init_engine_core(options);
    if (!engine) {
        return NULL;
    }

    /* Stage 2: Initialize buffer pool */
    if (init_engine_buffer_pool(engine) != 0) {
        goto cleanup_engine;
    }

    /* Stage 3: Initialize rings */
    if (init_engine_rings(engine, options) != 0) {
        goto cleanup_buffer_pool;
    }

    /* Stage 4: Register eventfd */
    if (register_eventfd_with_rings(engine) != 0) {
        goto cleanup_rings;
    }

    /* Stage 5: Start tick thread */
    if (start_tick_thread(engine) != 0) {
        goto cleanup_eventfd;
    }

    return engine;

cleanup_eventfd:
    if (engine->event_fd >= 0) {
        if (engine->eventfd_registered) {
            for (int i = 0; i < engine->ring_count; i++) {
                io_uring_unregister_eventfd(&engine->rings[i].ring);
            }
        }
        close(engine->event_fd);
    }
cleanup_rings:
    for (int i = 0; i < engine->ring_count; i++) {
        ring_destroy(&engine->rings[i]);
    }
    free(engine->rings);
cleanup_buffer_pool:
    buf_size_map_destroy(&engine->buf_size_map);
    buffer_pool_destroy(&engine->buffer_pool);
cleanup_engine:
    pthread_rwlock_destroy(&engine->reg_lock);
    free(engine);
    return NULL;
}

/**
 * Phase 1: Signal shutdown and stop background threads.
 *
 * Sets shutting_down flag with memory_order_release to ensure visibility,
 * then stops the adaptive tick thread and event loop. Uses atomic_exchange
 * on tick_running to ensure only one thread attempts pthread_join.
 */
static void destroy_phase_1_shutdown(aura_engine_t *engine) {
    /* Signal shutdown - new submissions will be rejected */
    atomic_store_explicit(&engine->shutting_down, true, memory_order_release);

    /* Stop tick thread (atomic_exchange ensures only one thread joins) */
    if (engine->adaptive_enabled && atomic_exchange(&engine->tick_running, false)) {
        pthread_join(engine->tick_thread, NULL);
    }

    /* Stop event loop if running */
    aura_stop(engine);
}

/**
 * Phase 3: Unregister buffers and files.
 *
 * Safe to call after phase 2 completes - all I/O is drained. Uses deferred
 * unregistration (request_unregister) to avoid aura_wait loop which can
 * livelock post-shutdown when no new I/O is submitted.
 */
static void destroy_phase_3_unregister(aura_engine_t *engine) {
    /* Unregister buffers and files (safe now that I/O is drained).
     * Call request_unregister (deferred) to avoid the aura_wait loop
     * which can livelock post-shutdown (no new I/O).
     * Read unreg_pending under reg_lock to avoid data race with the
     * writers that set these flags under the same lock. */
    pthread_rwlock_rdlock(&engine->reg_lock);
    bool need_unreg_bufs =
        atomic_load_explicit(&engine->buffers_registered, memory_order_relaxed) &&
        !engine->buffers_unreg_pending;
    bool need_unreg_files = atomic_load_explicit(&engine->files_registered, memory_order_relaxed) &&
                            !engine->files_unreg_pending;
    pthread_rwlock_unlock(&engine->reg_lock);

    if (need_unreg_bufs) {
        request_unregister_buffers(engine);
    }
    if (need_unreg_files) {
        request_unregister_files(engine);
    }
    finalize_deferred_unregistration(engine);
}

/**
 * Phase 4: Cleanup all resources and free memory.
 *
 * Destroys engine components in reverse initialization order:
 * eventfd → rings → buffer pool → rwlock → engine struct.
 */
static void destroy_phase_4_cleanup_resources(aura_engine_t *engine) {
    /* Unregister eventfd from all rings BEFORE closing it.
     * Closing first could cause kernel writes to a recycled fd number
     * if the OS reuses it before unregistration completes. */
    if (engine->event_fd >= 0) {
        if (engine->eventfd_registered) {
            for (int i = 0; i < engine->ring_count; i++) {
                io_uring_unregister_eventfd(&engine->rings[i].ring);
            }
        }
        close(engine->event_fd);
    }

    /* Destroy all rings */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_destroy(&engine->rings[i]);
    }
    free(engine->rings);

    /* Destroy buffer pool and size map */
    buf_size_map_destroy(&engine->buf_size_map);
    buffer_pool_destroy(&engine->buffer_pool);

    pthread_rwlock_destroy(&engine->reg_lock);
    free(engine);
}

void aura_destroy(aura_engine_t *engine) {
    if (!engine) {
        return;
    }

    destroy_phase_1_shutdown(engine);
    aura_drain(engine, -1); /* Phase 2: drain I/O before unregistering */
    destroy_phase_3_unregister(engine);
    destroy_phase_4_cleanup_resources(engine);
}

/* ============================================================================
 * Core I/O Operations
 * ============================================================================
 */

/**
 * Generic I/O submission helper - consolidates aura_read/aura_write logic.
 * Static inline ensures devirtualization for zero overhead.
 */
static inline aura_request_t *submit_io_generic(aura_engine_t *engine, int fd, aura_buf_t buf,
                                                size_t len, off_t offset, aura_callback_t callback,
                                                void *user_data, ring_submit_fn_t submit_unreg,
                                                ring_submit_fixed_fn_t submit_reg) {
    if (!engine || fd < 0 || len == 0) {
        errno = EINVAL;
        return NULL;
    }

    if (buf.type == AURA_BUF_UNREGISTERED) {
        if (!buf.u.ptr) {
            errno = EINVAL;
            return NULL;
        }
        return submit_unregistered_io(engine, fd, buf.u.ptr, len, offset, callback, user_data,
                                      submit_unreg);
    } else if (buf.type == AURA_BUF_REGISTERED) {
        registered_buf_info_t buf_info =
            validate_registered_buffer(engine, buf.u.fixed.index, buf.u.fixed.offset, len);
        if (!buf_info.reg_iov) {
            return NULL;
        }
        return submit_registered_buffer_io(engine, fd, buf_info.reg_iov, buf.u.fixed.index,
                                           buf.u.fixed.offset, len, offset, callback, user_data,
                                           submit_reg);
    } else {
        errno = EINVAL; /* Invalid buffer type */
        return NULL;
    }
}

aura_request_t *aura_read(aura_engine_t *engine, int fd, aura_buf_t buf, size_t len, off_t offset,
                          aura_callback_t callback, void *user_data) {
    return submit_io_generic(engine, fd, buf, len, offset, callback, user_data, ring_submit_read,
                             ring_submit_read_fixed);
}

aura_request_t *aura_write(aura_engine_t *engine, int fd, aura_buf_t buf, size_t len, off_t offset,
                           aura_callback_t callback, void *user_data) {
    return submit_io_generic(engine, fd, buf, len, offset, callback, user_data, ring_submit_write,
                             ring_submit_write_fixed);
}

aura_request_t *aura_fsync(aura_engine_t *engine, int fd, unsigned int flags,
                           aura_callback_t callback, void *user_data) {
    if (!engine || fd < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (flags & ~(unsigned)AURA_FSYNC_DATASYNC) {
        errno = EINVAL;
        return NULL;
    }

    file_resolve_guard_t file_guard = resolve_file_begin(engine, fd);

    submit_ctx_t ctx = submit_begin(engine, !file_guard.uses_registered_file);
    if (!ctx.req) {
        resolve_file_end(&file_guard);
        return NULL;
    }

    ctx.req->fd = file_guard.submit_fd;
    ctx.req->original_fd = fd;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;
    ctx.req->uses_registered_file = file_guard.uses_registered_file;

    int ret;
    if (flags & AURA_FSYNC_DATASYNC) {
        ret = ring_submit_fdatasync(ctx.ring, ctx.req);
    } else {
        ret = ring_submit_fsync(ctx.ring, ctx.req);
    }

    if (ret != 0) {
        submit_abort(&ctx);
        resolve_file_end(&file_guard);
        return NULL;
    }

    submit_end(&ctx);
    resolve_file_end(&file_guard);
    return ctx.req;
}

/* ============================================================================
 * Lifecycle Metadata Operations
 * ============================================================================ */

aura_request_t *aura_openat(aura_engine_t *engine, int dirfd, const char *pathname, int flags,
                            mode_t mode, aura_callback_t callback, void *user_data) {
    if (!engine || !pathname || (dirfd < 0 && dirfd != AT_FDCWD)) {
        errno = EINVAL;
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine, true);
    if (!ctx.req) return NULL;

    ctx.req->fd = dirfd;
    ctx.req->original_fd = dirfd;
    ctx.req->meta.open.pathname = pathname;
    ctx.req->meta.open.flags = flags;
    ctx.req->meta.open.mode = mode;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;
    ctx.req->uses_registered_file = false;

    if (ring_submit_openat(ctx.ring, ctx.req) != 0) {
        submit_abort(&ctx);
        return NULL;
    }

    submit_end(&ctx);
    return ctx.req;
}

aura_request_t *aura_close(aura_engine_t *engine, int fd, aura_callback_t callback,
                           void *user_data) {
    if (!engine || fd < 0) {
        errno = EINVAL;
        return NULL;
    }

    /* Close always uses the raw fd — IOSQE_FIXED_FILE on close means
     * "unregister slot" which is not what callers expect. */
    submit_ctx_t ctx = submit_begin(engine, true);
    if (!ctx.req) return NULL;

    ctx.req->fd = fd;
    ctx.req->original_fd = fd;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;
    ctx.req->uses_registered_file = false;

    if (ring_submit_close(ctx.ring, ctx.req) != 0) {
        submit_abort(&ctx);
        return NULL;
    }

    submit_end(&ctx);
    return ctx.req;
}

aura_request_t *aura_statx(aura_engine_t *engine, int dirfd, const char *pathname, int flags,
                           unsigned int mask, struct statx *statxbuf, aura_callback_t callback,
                           void *user_data) {
    if (!engine || !pathname || !statxbuf || (dirfd < 0 && dirfd != AT_FDCWD)) {
        errno = EINVAL;
        return NULL;
    }

    submit_ctx_t ctx = submit_begin(engine, true);
    if (!ctx.req) return NULL;

    ctx.req->fd = dirfd;
    ctx.req->original_fd = dirfd;
    ctx.req->meta.statx.pathname = pathname;
    ctx.req->meta.statx.flags = flags;
    ctx.req->meta.statx.mask = mask;
    ctx.req->meta.statx.buf = statxbuf;
    ctx.req->callback = callback;
    ctx.req->user_data = user_data;
    ctx.req->ring_idx = ctx.ring->ring_idx;
    ctx.req->uses_registered_file = false;

    if (ring_submit_statx(ctx.ring, ctx.req) != 0) {
        submit_abort(&ctx);
        return NULL;
    }

    submit_end(&ctx);
    return ctx.req;
}

/**
 * Macro: file-resolve + submit scaffold.
 *
 * Eliminates the repeated resolve_file_begin → submit_begin → set common fields →
 * ring_submit → submit_end/abort → resolve_file_end pattern.
 *
 * @param engine_     Engine handle
 * @param fd_         File descriptor
 * @param callback_   Completion callback
 * @param user_data_  User context
 * @param submit_fn_  Ring submit function (e.g., ring_submit_fallocate)
 * @param setup_code  Statement(s) to set op-specific fields on ctx.req
 */
#define SUBMIT_FILE_RESOLVED_OP(engine_, fd_, callback_, user_data_, submit_fn_, setup_code) \
    do {                                                                                     \
        file_resolve_guard_t file_guard_ = resolve_file_begin(engine_, fd_);                 \
        submit_ctx_t ctx = submit_begin(engine_, !file_guard_.uses_registered_file);         \
        if (!ctx.req) {                                                                      \
            resolve_file_end(&file_guard_);                                                  \
            return NULL;                                                                     \
        }                                                                                    \
        ctx.req->fd = file_guard_.submit_fd;                                                 \
        ctx.req->original_fd = fd_;                                                          \
        ctx.req->callback = callback_;                                                       \
        ctx.req->user_data = user_data_;                                                     \
        ctx.req->ring_idx = ctx.ring->ring_idx;                                              \
        ctx.req->uses_registered_file = file_guard_.uses_registered_file;                    \
        setup_code;                                                                          \
        if (submit_fn_(ctx.ring, ctx.req) != 0) {                                            \
            submit_abort(&ctx);                                                              \
            resolve_file_end(&file_guard_);                                                  \
            return NULL;                                                                     \
        }                                                                                    \
        submit_end(&ctx);                                                                    \
        resolve_file_end(&file_guard_);                                                      \
        return ctx.req;                                                                      \
    } while (0)

aura_request_t *aura_fallocate(aura_engine_t *engine, int fd, int mode, off_t offset, off_t len,
                               aura_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || offset < 0 || len <= 0) {
        errno = EINVAL;
        return NULL;
    }
    SUBMIT_FILE_RESOLVED_OP(engine, fd, callback, user_data, ring_submit_fallocate, {
        ctx.req->offset = offset;
        ctx.req->len = (size_t)len;
        ctx.req->meta.fallocate.mode = mode;
    });
}

aura_request_t *aura_ftruncate(aura_engine_t *engine, int fd, off_t length,
                               aura_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || length < 0) {
        errno = EINVAL;
        return NULL;
    }
    SUBMIT_FILE_RESOLVED_OP(engine, fd, callback, user_data, ring_submit_ftruncate,
                            { ctx.req->len = (size_t)length; });
}

aura_request_t *aura_sync_file_range(aura_engine_t *engine, int fd, off_t offset, off_t nbytes,
                                     unsigned int flags, aura_callback_t callback,
                                     void *user_data) {
    if (!engine || fd < 0 || offset < 0 || nbytes < 0) {
        errno = EINVAL;
        return NULL;
    }
    SUBMIT_FILE_RESOLVED_OP(engine, fd, callback, user_data, ring_submit_sync_file_range, {
        ctx.req->offset = offset;
        ctx.req->len = (size_t)nbytes;
        ctx.req->meta.sync_range.flags = flags;
    });
}

/* ============================================================================
 * Vectored I/O Operations
 * ============================================================================
 */

aura_request_t *aura_readv(aura_engine_t *engine, int fd, const struct iovec *iov, int iovcnt,
                           off_t offset, aura_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || !iov || iovcnt <= 0 || iovcnt > IOV_MAX) {
        errno = EINVAL;
        return NULL;
    }
    SUBMIT_FILE_RESOLVED_OP(engine, fd, callback, user_data, ring_submit_readv, {
        ctx.req->iov = iov;
        ctx.req->iovcnt = iovcnt;
        ctx.req->offset = offset;
    });
}

aura_request_t *aura_writev(aura_engine_t *engine, int fd, const struct iovec *iov, int iovcnt,
                            off_t offset, aura_callback_t callback, void *user_data) {
    if (!engine || fd < 0 || !iov || iovcnt <= 0 || iovcnt > IOV_MAX) {
        errno = EINVAL;
        return NULL;
    }
    SUBMIT_FILE_RESOLVED_OP(engine, fd, callback, user_data, ring_submit_writev, {
        ctx.req->iov = iov;
        ctx.req->iovcnt = iovcnt;
        ctx.req->offset = offset;
    });
}

/* ============================================================================
 * Cancellation
 * ============================================================================
 */

int aura_cancel(aura_engine_t *engine, aura_request_t *req) {
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
    ring_lock(ring);

    /* Double-check it's still pending while holding lock */
    if (!atomic_load_explicit(&req->pending, memory_order_acquire)) {
        ring_unlock(ring);
        errno = EALREADY;
        return (-1);
    }

    /* Get a request slot for the cancel operation */
    int op_idx;
    aura_request_t *cancel_req = ring_get_request(ring, &op_idx);
    if (!cancel_req) {
        ring_unlock(ring);
        errno = ENOMEM;
        return (-1);
    }

    /* Submit cancel — clear callback/user_data so process_completion()
     * does not invoke a stale callback from this slot's previous use. */
    cancel_req->ring_idx = ring->ring_idx;
    cancel_req->fd = -1;
    cancel_req->original_fd = -1;
    cancel_req->callback = NULL;
    cancel_req->user_data = NULL;
    if (ring_submit_cancel(ring, cancel_req, req) != 0) {
        ring_put_request(ring, op_idx);
        ring_unlock(ring);
        if (errno == 0) errno = EIO;
        return (-1);
    }

    /* Flush immediately to expedite cancellation */
    if (ring_flush(ring) < 0) {
        if (!flush_error_is_fatal(errno)) {
            errno = 0;
            ring_unlock(ring);
            return (0);
        }
        latch_fatal_submit_errno(engine, errno);
        ring_unlock(ring);
        errno = get_fatal_submit_errno(engine);
        return (-1);
    }

    ring_unlock(ring);
    return (0);
}

/* ============================================================================
 * Request Introspection
 * ============================================================================
 */

bool aura_request_pending(const aura_request_t *req) {
    if (!req) {
        return false;
    }
    return atomic_load_explicit(&req->pending, memory_order_acquire);
}

int aura_request_fd(const aura_request_t *req) {
    if (!req) {
        errno = EINVAL;
        return -1;
    }
    /* When registered files are in use, req->fd contains the fixed-file
     * index. Return the original file descriptor instead. */
    if (req->uses_registered_file) {
        return req->original_fd;
    }
    return req->fd;
}

void *aura_request_user_data(const aura_request_t *req) {
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

int aura_get_poll_fd(const aura_engine_t *engine) {
    if (!engine || engine->ring_count == 0) {
        errno = EINVAL;
        return (-1);
    }

    /* Lazy registration: if eventfd exists but is not registered with rings
     * (THREAD_LOCAL mode), register now so the caller's event loop works. */
    if (engine->event_fd >= 0 && !engine->eventfd_registered) {
        aura_engine_t *mutable_engine = (aura_engine_t *)engine;
        for (int i = 0; i < engine->ring_count; i++) {
            int ret = io_uring_register_eventfd(&mutable_engine->rings[i].ring, engine->event_fd);
            if (ret != 0) {
                /* Best-effort: unregister any that succeeded */
                for (int j = 0; j < i; j++) {
                    io_uring_unregister_eventfd(&mutable_engine->rings[j].ring);
                }
                errno = -ret;
                return (-1);
            }
        }
        mutable_engine->eventfd_registered = true;
    }

    /* Return the unified eventfd that is registered with all io_uring rings.
     * When ANY ring completes an operation, this fd becomes readable.
     * This allows proper event loop integration in multi-core setups. */
    return engine->event_fd;
}

int aura_flush(aura_engine_t *engine) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        ring_lock(ring);
        if (ring_flush(ring) < 0 && flush_error_is_fatal(errno)) {
            latch_fatal_submit_errno(engine, errno);
        }
        ring_unlock(ring);
    }
    return 0;
}

int aura_poll(aura_engine_t *engine) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    /* Thread-local fast path: skip eventfd drain, skip two-pass loop.
     * select_ring_thread_local validates ownership; if tls_owned_ring is stale
     * it gets reassigned. After the first submit, tls_owned_ring is guaranteed
     * valid for this engine. If no submit happened yet, we fall through. */
    if (engine->ring_select == AURA_SELECT_THREAD_LOCAL && tls_owned_ring &&
        tls_owned_ring >= engine->rings &&
        tls_owned_ring < engine->rings + engine->ring_count) {
        ring_ctx_t *ring = tls_owned_ring;
        if (ring->single_thread) {
            ring_flush_fast(ring);
        } else {
            ring_lock(ring);
            flush_ring_checked(engine, ring);
            ring_unlock(ring);
        }
        int n = ring_poll(ring);
        return n > 0 ? n : 0;
    }

    bool fatal_latched = (get_fatal_submit_errno(engine) != 0);

    if (maybe_finalize_deferred_unregistration(engine) != 0) {
        return (-1);
    }

    if (engine->eventfd_registered) drain_eventfd(engine);

    int total = 0;
    int skipped = 0;

    /* First pass: try-lock to avoid blocking on contended rings.
     * Note: ring_poll manages its own internal locking via process_completion,
     * so we only hold the lock for ring_flush, then release before ring_poll. */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];

        if (ring_trylock(ring)) {
            if (flush_ring_checked(engine, ring)) fatal_latched = true;
            ring_unlock(ring);

            int completed = ring_poll(ring);
            if (completed > 0) total += completed;
        } else {
            skipped++;
        }
    }

    /* Second pass: if we skipped rings and got no completions,
     * do blocking acquire to ensure forward progress */
    if (skipped > 0 && total == 0) {
        for (int i = 0; i < engine->ring_count; i++) {
            ring_ctx_t *ring = &engine->rings[i];

            ring_lock(ring);
            if (flush_ring_checked(engine, ring)) fatal_latched = true;
            ring_unlock(ring);

            int completed = ring_poll(ring);
            if (completed > 0) total += completed;
        }
    }

    if (maybe_finalize_deferred_unregistration(engine) != 0 && total == 0) {
        return (-1);
    }

    if (fatal_latched && total == 0) {
        errno = get_fatal_submit_errno(engine);
        return (-1);
    }

    return total;
}

int aura_wait(aura_engine_t *engine, int timeout_ms) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    /* Thread-local fast path: flush, poll, and if needed block on own ring.
     * Validate ownership with bounds check to handle stale TLS. */
    if (engine->ring_select == AURA_SELECT_THREAD_LOCAL && tls_owned_ring &&
        tls_owned_ring >= engine->rings &&
        tls_owned_ring < engine->rings + engine->ring_count) {
        ring_ctx_t *ring = tls_owned_ring;
        if (ring->single_thread) {
            ring_flush_fast(ring);
        } else {
            ring_lock(ring);
            flush_ring_checked(engine, ring);
            ring_unlock(ring);
        }
        int n = ring_poll(ring);
        if (n > 0) return n;
        return ring_wait(ring, timeout_ms);
    }

    bool fatal_latched = (get_fatal_submit_errno(engine) != 0);

    if (maybe_finalize_deferred_unregistration(engine) != 0) {
        return (-1);
    }

    if (engine->eventfd_registered) drain_eventfd(engine);

    /* Single-pass flush+poll: flush each ring and immediately poll it.
     * Returns as soon as any ring produces completions — remaining rings
     * get their flush on the next aura_wait() call. This merges two
     * full passes (flush all, poll all) into one early-exit pass. */
    int total = 0;
    int first_pending = -1; /* First ring with pending ops (for blocking) */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];
        ring_lock(ring);
        if (flush_ring_checked(engine, ring)) fatal_latched = true;
        bool has_pending = (atomic_load_explicit(&ring->pending_count, memory_order_relaxed) > 0);
        ring_unlock(ring);

        int n = ring_poll(ring);
        if (n > 0) {
            total += n;
        }
        if (has_pending && first_pending < 0) {
            first_pending = i;
        }
    }
    if (total > 0) {
        if (maybe_finalize_deferred_unregistration(engine) != 0) {
            return (-1);
        }
        return total;
    }

    if (fatal_latched) {
        errno = get_fatal_submit_errno(engine);
        return (-1);
    }

    /* Nothing ready — block on first ring with pending ops.
     * first_pending is captured during the flush pass above. */
    if (first_pending < 0) {
        return (0);
    }

    /* Block on eventfd — any ring completion wakes us, avoiding starvation */
    if (engine->event_fd >= 0) {
        struct pollfd pfd = { .fd = engine->event_fd, .events = POLLIN };
        int poll_timeout = (timeout_ms < 0) ? -1 : timeout_ms;
        int pret = poll(&pfd, 1, poll_timeout);
        if (pret > 0) {
            drain_eventfd(engine);
            int completed = 0;
            for (int j = 0; j < engine->ring_count; j++) {
                ring_ctx_t *r = &engine->rings[j];
                ring_lock(r);
                (void)flush_ring_checked(engine, r);
                ring_unlock(r);
                int n = ring_poll(r);
                if (n > 0) completed += n;
            }
            if (maybe_finalize_deferred_unregistration(engine) != 0) {
                return (-1);
            }
            if (completed > 0) {
                return completed;
            }
            /* Woke from poll but no completions — fall through to timeout. */
        }
        if (pret < 0 && errno != EINTR) {
            return (-1);
        }
        /* pret == 0: timeout, pret < 0 with EINTR: signal interrupted.
         * Do a final non-blocking poll to catch completions that arrived
         * between the interrupt/timeout and now. */
        {
            int completed = 0;
            for (int j = 0; j < engine->ring_count; j++) {
                int n = ring_poll(&engine->rings[j]);
                if (n > 0) completed += n;
            }
            if (completed > 0) {
                if (maybe_finalize_deferred_unregistration(engine) != 0) {
                    return (-1);
                }
                return completed;
            }
        }
    } else {
        /* Fallback: no eventfd, block on first pending ring.
         * NOTE: With timeout_ms == -1 and multiple rings, this can block
         * indefinitely on one ring while completions arrive on others.
         * Deferred: eventfd() failure on modern Linux (2.6.22+) is near-
         * impossible unless the fd limit is hit, and the timeout path
         * handles it adequately. Not worth adding complexity here. */
        int completed = ring_wait(&engine->rings[first_pending], timeout_ms);
        if (completed > 0) {
            for (int j = 0; j < engine->ring_count; j++) {
                if (j == first_pending) continue;
                int more = ring_poll(&engine->rings[j]);
                if (more > 0) completed += more;
            }
            if (maybe_finalize_deferred_unregistration(engine) != 0) {
                return (-1);
            }
            return completed;
        }
        if (completed < 0) {
            return (-1);
        }
    }

    /* Operations were pending but none completed before the timeout.
     * Return -1 with ETIMEDOUT so callers can distinguish from the
     * "nothing pending" case (which returns 0). */
    if (maybe_finalize_deferred_unregistration(engine) != 0) {
        return (-1);
    }
    errno = ETIMEDOUT;
    return (-1);
}

void aura_run(aura_engine_t *engine) {
    if (!engine) {
        return;
    }

    /* Clear stop_requested BEFORE setting running, so a concurrent
     * aura_stop() that sees running=true will store stop_requested=true
     * after our clear.  The reverse order had a race: aura_stop() could
     * set stop_requested between our running=true and the clear. */
    atomic_store(&engine->stop_requested, false);
    atomic_store_explicit(&engine->running, true, memory_order_release);

    int drain_iterations = 0;
    for (;;) {
        int completed = aura_wait(engine, 100);
        if (completed < 0 && errno != ETIMEDOUT) {
            break;
        }

        if (!atomic_load(&engine->stop_requested)) {
            drain_iterations = 0;
            continue;
        }

        /* Stop requested — drain in-flight I/O but don't wait forever
         * for new submissions from other threads. */
        bool has_pending = false;
        for (int i = 0; i < engine->ring_count; i++) {
            if (atomic_load_explicit(&engine->rings[i].pending_count, memory_order_acquire) > 0) {
                has_pending = true;
                break;
            }
        }

        if (!has_pending && completed <= 0) {
            break;
        }
        if (++drain_iterations >= 100) {
            aura_log(AURA_LOG_WARN, "aura_run drain timed out with in-flight I/O still pending");
            break;
        }
    }

    atomic_store(&engine->running, false);
}

void aura_stop(aura_engine_t *engine) {
    if (!engine) {
        return;
    }
    atomic_store(&engine->stop_requested, true);
}

int aura_drain(aura_engine_t *engine, int timeout_ms) {
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
        if (maybe_finalize_deferred_unregistration(engine) != 0) {
            return (-1);
        }
        /* Check if all rings are drained */
        bool has_pending = false;
        for (int i = 0; i < engine->ring_count; i++) {
            ring_ctx_t *ring = &engine->rings[i];
            /* pending_count is atomic; no lock needed for an advisory read. */
            if (atomic_load_explicit(&ring->pending_count, memory_order_acquire) > 0) {
                has_pending = true;
                break;
            }
        }
        if (!has_pending) {
            if (maybe_finalize_deferred_unregistration(engine) != 0) {
                return (-1);
            }
            return total;
        }

        /* Calculate remaining timeout for this iteration */
        int wait_ms;
        if (timeout_ms < 0) {
            wait_ms = 100; /* Poll in 100ms intervals when no deadline */
        } else if (timeout_ms == 0) {
            /* Non-blocking: just poll once */
            int n = aura_poll(engine);
            return n >= 0 ? total + n : (total > 0 ? total : n);
        } else {
            int64_t remaining_ns = deadline_ns - get_time_ns();
            if (remaining_ns <= 0) {
                errno = ETIMEDOUT;
                return (-1);
            }
            wait_ms = (int)(remaining_ns / 1000000LL);
            if (wait_ms <= 0) wait_ms = 1;
            if (wait_ms > 100) wait_ms = 100; /* Cap per-iteration wait */
        }

        int n = aura_wait(engine, wait_ms);
        if (n > 0) total += n;
        if (n < 0 && errno != ETIMEDOUT) return (-1);
        /* ETIMEDOUT from aura_wait is expected — we loop with capped
         * per-iteration timeouts and check the overall deadline above. */
    }
}

/* ============================================================================
 * Managed Buffers
 * ============================================================================
 */

void *aura_buffer_alloc(aura_engine_t *engine, size_t size) {
    if (!engine || size == 0) {
        errno = EINVAL;
        return NULL;
    }

    void *buf = buffer_pool_alloc(&engine->buffer_pool, size);
    if (buf) {
        if (buf_size_map_insert(&engine->buf_size_map, buf, size_to_class(size)) != 0) {
            buffer_pool_free(&engine->buffer_pool, buf, size);
            return NULL;
        }
    }
    return buf;
}

void aura_buffer_free(aura_engine_t *engine, void *buf) {
    if (!engine || !buf) {
        return;
    }

    buffer_pool_free_tracked(&engine->buffer_pool, &engine->buf_size_map, buf);
}

/* ============================================================================
 * Registered Buffers (Phase 5)
 * ============================================================================
 */

int aura_register_buffers(aura_engine_t *engine, const struct iovec *iovs, unsigned int count) {
    if (!engine || !iovs || count == 0 || count > (unsigned int)INT_MAX) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (atomic_load_explicit(&engine->buffers_registered, memory_order_relaxed) ||
        engine->buffers_unreg_pending) {
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

    /* Register with all rings — they share the registration.
     * No ring_lock needed: reg_lock(write) prevents new submissions that use
     * registered buffers, and io_uring_register_buffers is a kernel syscall
     * that doesn't touch the SQ/CQ.  Avoiding ring_lock here prevents a
     * lock-ordering inversion (reg_lock → ring_lock vs the submission path's
     * ring_lock → reg_lock(read)). */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];

        int ret = io_uring_register_buffers(&ring->ring, iovs, count);
        if (ret < 0) {
            /* Unregister from already-registered rings */
            for (int j = 0; j < i; j++) {
                io_uring_unregister_buffers(&engine->rings[j].ring);
            }
            free(engine->registered_buffers);
            engine->registered_buffers = NULL;
            engine->registered_buffer_count = 0;
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = -ret;
            return (-1);
        }
    }

    atomic_store_explicit(&engine->buffers_registered, true, memory_order_release);
    engine->buffers_unreg_pending = false;
    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

/* ============================================================================
 * Unified Registration Lifecycle
 * ============================================================================
 */

/**
 * Generic deferred unregister: set the pending flag and finalize if possible.
 * @param registered      Pointer to the _Atomic bool (buffers_registered or files_registered)
 * @param unreg_pending   Pointer to the bool (buffers_unreg_pending or files_unreg_pending)
 */
static int request_unregister_generic(aura_engine_t *engine, _Atomic bool *registered,
                                      bool *unreg_pending) {
    pthread_rwlock_wrlock(&engine->reg_lock);

    if (!atomic_load_explicit(registered, memory_order_relaxed) && !*unreg_pending) {
        pthread_rwlock_unlock(&engine->reg_lock);
        return (0);
    }

    *unreg_pending = true;
    pthread_rwlock_unlock(&engine->reg_lock);

    return finalize_deferred_unregistration(engine);
}

static int request_unregister_buffers(aura_engine_t *engine) {
    return request_unregister_generic(engine, &engine->buffers_registered,
                                      &engine->buffers_unreg_pending);
}

static int request_unregister_files(aura_engine_t *engine) {
    return request_unregister_generic(engine, &engine->files_registered,
                                      &engine->files_unreg_pending);
}

int aura_request_unregister(aura_engine_t *engine, aura_reg_type_t type) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    switch (type) {
    case AURA_REG_BUFFERS:
        return request_unregister_buffers(engine);
    case AURA_REG_FILES:
        return request_unregister_files(engine);
    default:
        errno = EINVAL;
        return (-1);
    }
}

int aura_unregister(aura_engine_t *engine, aura_reg_type_t type) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }

    if (ring_in_callback_context()) {
        /* Callback-safe path: request deferred unregister and return. */
        return aura_request_unregister(engine, type);
    }

    if (aura_request_unregister(engine, type) != 0) {
        return (-1);
    }

    /* Wait for the deferred unregister to complete.
     * Timeout after 10 seconds to avoid hanging on stuck I/O. */
    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);
    int64_t deadline_ms = (int64_t)ts_start.tv_sec * 1000 + ts_start.tv_nsec / 1000000 + 10000;
    for (;;) {
        bool done;
        pthread_rwlock_rdlock(&engine->reg_lock);
        switch (type) {
        case AURA_REG_BUFFERS:
            done = !atomic_load_explicit(&engine->buffers_registered, memory_order_relaxed) &&
                   !engine->buffers_unreg_pending;
            break;
        case AURA_REG_FILES:
            done = !atomic_load_explicit(&engine->files_registered, memory_order_relaxed) &&
                   !engine->files_unreg_pending;
            break;
        default:
            done = true;
            break;
        }
        pthread_rwlock_unlock(&engine->reg_lock);
        if (done) {
            return (0);
        }

        struct timespec ts_now;
        clock_gettime(CLOCK_MONOTONIC, &ts_now);
        int64_t now_ms = (int64_t)ts_now.tv_sec * 1000 + ts_now.tv_nsec / 1000000;
        if (now_ms >= deadline_ms) {
            errno = ETIMEDOUT;
            return (-1);
        }

        int n = aura_wait(engine, 100);
        if (n < 0 && errno != ETIMEDOUT) {
            return (-1);
        }
        if (n <= 0) {
            /* No completions — sleep briefly to avoid busy-polling */
            struct timespec ts_sleep = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1ms */
            nanosleep(&ts_sleep, NULL);
        }
    }
}

/* ============================================================================
 * Registered Files
 * ============================================================================
 */

int aura_register_files(aura_engine_t *engine, const int *fds, unsigned int count) {
    if (!engine || !fds || count == 0 || count > (unsigned int)INT_MAX) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (atomic_load_explicit(&engine->files_registered, memory_order_relaxed) ||
        engine->files_unreg_pending) {
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

    /* Register with all rings — no ring_lock needed (same rationale as
     * aura_register_buffers: reg_lock(write) blocks submissions, and the
     * kernel syscall doesn't touch SQ/CQ). */
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];

        int ret = io_uring_register_files(&ring->ring, fds, count);
        if (ret < 0) {
            /* Unregister from already-registered rings */
            for (int j = 0; j < i; j++) {
                io_uring_unregister_files(&engine->rings[j].ring);
            }
            free(engine->registered_files);
            engine->registered_files = NULL;
            engine->registered_file_count = 0;
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = -ret;
            return (-1);
        }
    }

    atomic_store_explicit(&engine->files_registered, true, memory_order_release);
    engine->files_unreg_pending = false;
    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

int aura_update_file(aura_engine_t *engine, int index, int fd) {
    if (!engine || index < 0) {
        errno = EINVAL;
        return (-1);
    }

    pthread_rwlock_wrlock(&engine->reg_lock);

    if (!atomic_load_explicit(&engine->files_registered, memory_order_relaxed) ||
        engine->files_unreg_pending) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = engine->files_unreg_pending ? EBUSY : ENOENT;
        return (-1);
    }

    if (index >= engine->registered_file_count) {
        pthread_rwlock_unlock(&engine->reg_lock);
        errno = EINVAL;
        return (-1);
    }

    /* Update in all rings, rolling back on failure.  No ring_lock needed
     * (same rationale as aura_register_buffers/files).
     * If rollback itself fails, the file table is inconsistent across rings
     * and further I/O submissions are unsafe — latch a fatal error. */
    int old_fd = engine->registered_files[index];
    for (int i = 0; i < engine->ring_count; i++) {
        ring_ctx_t *ring = &engine->rings[i];

        int ret = io_uring_register_files_update(&ring->ring, index, &fd, 1);
        if (ret < 0) {
            /* Roll back already-updated rings to old fd */
            bool rollback_failed = false;
            for (int j = 0; j < i; j++) {
                int rb = io_uring_register_files_update(&engine->rings[j].ring, index, &old_fd, 1);
                if (rb < 0) {
                    rollback_failed = true;
                }
            }
            if (rollback_failed) {
                aura_log(AURA_LOG_ERR,
                         "file registration rollback failed — file table inconsistent");
                latch_fatal_submit_errno(engine, EIO);
            }
            pthread_rwlock_unlock(&engine->reg_lock);
            errno = -ret;
            return (-1);
        }
    }

    /* Update our copy */
    engine->registered_files[index] = fd;
    pthread_rwlock_unlock(&engine->reg_lock);
    return (0);
}

/* ============================================================================
 * Statistics
 * ============================================================================
 */

const char *aura_version(void) {
    return AURA_VERSION_STRING;
}

int aura_version_int(void) {
    return AURA_VERSION;
}

int aura_get_stats(const aura_engine_t *engine, aura_stats_t *stats, size_t stats_size) {
    if (!engine || !stats || stats_size == 0) {
        errno = EINVAL;
        return (-1);
    }

    /* Zero the caller's buffer (may be smaller than our struct) */
    aura_stats_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    /* Aggregate stats from all rings */
    int total_in_flight = 0;
    int total_peak_in_flight = 0;
    int total_optimal_inflight = 0;
    int total_batch_size = 0;
    double total_throughput = 0.0;
    double max_p99 = 0.0;

    for (int i = 0; i < engine->ring_count; i++) {
        /* Cast away const: locking a mutex is logically const */
        ring_ctx_t *ring = (ring_ctx_t *)&engine->rings[i];

        /* Lock ring while reading stats to prevent data races with
         * completion handlers and tick thread */
        ring_lock(ring);

        tmp.ops_completed += ring->ops_completed;
        tmp.bytes_transferred += ring->bytes_completed;
        total_in_flight += atomic_load_explicit(&ring->pending_count, memory_order_relaxed);
        total_peak_in_flight +=
            atomic_load_explicit(&ring->peak_pending_count, memory_order_relaxed);

        /* Get adaptive controller values */
        adaptive_controller_t *ctrl = &ring->adaptive;
        total_optimal_inflight += atomic_load(&ctrl->current_in_flight_limit);
        total_batch_size += atomic_load(&ctrl->current_batch_threshold);

        /* Use memory_order_acquire to pair with release in tick thread,
         * ensuring consistent reads on ARM/PowerPC with weak memory ordering. */
        double throughput = atomic_load_double(&ctrl->current_throughput_bps, memory_order_acquire);
        double p99 = atomic_load_double(&ctrl->current_p99_ms, memory_order_acquire);
        total_throughput += throughput;
        if (p99 > max_p99) {
            max_p99 = p99;
        }

        ring_unlock(ring);
    }

    tmp.current_in_flight = total_in_flight;
    tmp.peak_in_flight = total_peak_in_flight;
    tmp.optimal_in_flight = total_optimal_inflight;
    /* Per-ring average, consistent with optimal_in_flight being a total.
     * Guard against division by zero if no rings are active. */
    tmp.optimal_batch_size = engine->ring_count > 0 ? total_batch_size / engine->ring_count : 0;
    tmp.current_throughput_bps = total_throughput;
    tmp.p99_latency_ms = max_p99;
    tmp.adaptive_spills = atomic_load_explicit(&engine->adaptive_spills, memory_order_relaxed);

    /* Copy only as many bytes as the caller's struct can hold */
    size_t copy_size = stats_size < sizeof(tmp) ? stats_size : sizeof(tmp);
    memcpy(stats, &tmp, copy_size);
    return (0);
}

/* Verify public and internal histogram constants stay in sync */
_Static_assert(AURA_HISTOGRAM_BUCKETS == LATENCY_BUCKET_COUNT,
               "Public AURA_HISTOGRAM_BUCKETS must match internal LATENCY_BUCKET_COUNT");
_Static_assert(AURA_HISTOGRAM_TIER_COUNT == LATENCY_TIER_COUNT,
               "Public AURA_HISTOGRAM_TIER_COUNT must match internal LATENCY_TIER_COUNT");

/* Verify public phase constants match internal enum */
_Static_assert(AURA_PHASE_BASELINE == ADAPTIVE_PHASE_BASELINE,
               "AURA_PHASE_BASELINE must match internal enum");
_Static_assert(AURA_PHASE_PROBING == ADAPTIVE_PHASE_PROBING,
               "AURA_PHASE_PROBING must match internal enum");
_Static_assert(AURA_PHASE_STEADY == ADAPTIVE_PHASE_STEADY,
               "AURA_PHASE_STEADY must match internal enum");
_Static_assert(AURA_PHASE_BACKOFF == ADAPTIVE_PHASE_BACKOFF,
               "AURA_PHASE_BACKOFF must match internal enum");
_Static_assert(AURA_PHASE_SETTLING == ADAPTIVE_PHASE_SETTLING,
               "AURA_PHASE_SETTLING must match internal enum");
_Static_assert(AURA_PHASE_CONVERGED == ADAPTIVE_PHASE_CONVERGED,
               "AURA_PHASE_CONVERGED must match internal enum");

int aura_get_ring_count(const aura_engine_t *engine) {
    if (!engine) return 0;
    return engine->ring_count;
}

int aura_get_ring_stats(const aura_engine_t *engine, int ring_idx, aura_ring_stats_t *stats,
                        size_t stats_size) {
    if (!engine || !stats || stats_size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (ring_idx < 0 || ring_idx >= engine->ring_count) {
        memset(stats, 0, stats_size < sizeof(*stats) ? stats_size : sizeof(*stats));
        errno = EINVAL;
        return -1;
    }

    aura_ring_stats_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    /* Cast away const: locking a mutex is logically const */
    ring_ctx_t *ring = (ring_ctx_t *)&engine->rings[ring_idx];
    ring_lock(ring);

    tmp.ops_completed = ring->ops_completed;
    tmp.bytes_transferred = ring->bytes_completed;
    tmp.pending_count = atomic_load_explicit(&ring->pending_count, memory_order_relaxed);
    tmp.peak_in_flight = atomic_load_explicit(&ring->peak_pending_count, memory_order_relaxed);
    tmp.queue_depth = ring->max_requests;

    adaptive_controller_t *ctrl = &ring->adaptive;
    tmp.in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_acquire);
    tmp.batch_threshold =
        atomic_load_explicit(&ctrl->current_batch_threshold, memory_order_acquire);
    tmp.p99_latency_ms = atomic_load_double(&ctrl->current_p99_ms, memory_order_acquire);
    tmp.throughput_bps = atomic_load_double(&ctrl->current_throughput_bps, memory_order_acquire);
    tmp.aimd_phase = atomic_load_explicit(&ctrl->phase, memory_order_acquire);

    ring_unlock(ring);

    size_t copy_size = stats_size < sizeof(tmp) ? stats_size : sizeof(tmp);
    memcpy(stats, &tmp, copy_size);
    return 0;
}

int aura_get_histogram(const aura_engine_t *engine, int ring_idx, aura_histogram_t *hist,
                       size_t hist_size) {
    if (!engine || !hist || hist_size == 0) {
        errno = EINVAL;
        return -1;
    }
    if (ring_idx < 0 || ring_idx >= engine->ring_count) {
        memset(hist, 0, hist_size < sizeof(*hist) ? hist_size : sizeof(*hist));
        errno = EINVAL;
        return -1;
    }

    aura_histogram_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    /* Cast away const: locking a mutex is logically const */
    ring_ctx_t *ring = (ring_ctx_t *)&engine->rings[ring_idx];
    ring_lock(ring);

    /* Read from the active histogram.  Individual bucket loads are atomic but
     * the overall snapshot is approximate — see aura_histogram_t docs. */
    adaptive_histogram_t *active = adaptive_hist_active(&ring->adaptive.hist_pair);
    for (int i = 0; i < AURA_HISTOGRAM_BUCKETS; i++) {
        tmp.buckets[i] = atomic_load_explicit(&active->buckets[i], memory_order_relaxed);
    }
    tmp.overflow = atomic_load_explicit(&active->overflow, memory_order_relaxed);
    uint64_t total = atomic_load_explicit(&active->total_count, memory_order_relaxed);
    tmp.total_count = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
    tmp.max_tracked_us = LATENCY_TIERED_MAX_US;
    tmp.tier_count = LATENCY_TIER_COUNT;
    for (int t = 0; t < LATENCY_TIER_COUNT; t++) {
        tmp.tier_start_us[t] = LATENCY_TIERS[t].start_us;
        tmp.tier_width_us[t] = LATENCY_TIERS[t].width_us;
        tmp.tier_base_bucket[t] = LATENCY_TIERS[t].base_bucket;
    }

    ring_unlock(ring);

    /* Copy only as many bytes as the caller's struct can hold */
    size_t copy_size = hist_size < sizeof(tmp) ? hist_size : sizeof(tmp);
    memcpy(hist, &tmp, copy_size);
    return 0;
}

int aura_get_buffer_stats(const aura_engine_t *engine, aura_buffer_stats_t *stats,
                          size_t stats_size) {
    if (!engine || !stats || stats_size == 0) {
        errno = EINVAL;
        return -1;
    }

    aura_buffer_stats_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    const buffer_pool_t *pool = &engine->buffer_pool;
    tmp.total_allocated_bytes = atomic_load_explicit(&pool->total_allocated, memory_order_relaxed);
    tmp.total_buffers = atomic_load_explicit(&pool->total_buffers, memory_order_relaxed);
    tmp.shard_count = pool->shard_count;

    size_t copy_size = stats_size < sizeof(tmp) ? stats_size : sizeof(tmp);
    memcpy(stats, &tmp, copy_size);
    return 0;
}

int aura_request_op_type(const aura_request_t *req) {
    if (!req) return -1;
    return (int)req->op_type;
}

void aura_request_set_linked(aura_request_t *req) {
    if (!req) return;
    req->linked = true;
    /* Retroactively set IOSQE_IO_LINK on the SQE that was already prepped
     * during submission. The SQE is still in the SQ ring because
     * ring_should_flush() batch threshold is never met by a single SQE,
     * and linked ops are always submitted back-to-back. */
    struct io_uring_sqe *sqe = ring_get_last_sqe();
    if (sqe) {
        sqe->flags |= IOSQE_IO_LINK;
    }
    /* Pin subsequent submissions to the same ring so linked SQEs share
     * one submission queue. tls_link_ring is checked in submit_begin(). */
    tls_link_ring = tls_last_ring;
    tls_link_depth++;
}

bool aura_request_is_linked(const aura_request_t *req) {
    if (!req) return false;
    return req->linked;
}

const char *aura_phase_name(int phase) {
    return adaptive_phase_name((adaptive_phase_t)phase);
}

/* ============================================================================
 * Diagnostics
 * ============================================================================
 */

int aura_get_fatal_error(const aura_engine_t *engine) {
    if (!engine) {
        errno = EINVAL;
        return (-1);
    }
    return get_fatal_submit_errno(engine);
}

bool aura_in_callback_context(void) {
    return ring_in_callback_context();
}

double aura_histogram_percentile(const aura_histogram_t *hist, double percentile) {
    if (!hist || percentile < 0.0 || percentile > 100.0) {
        return -1.0;
    }
    if (hist->total_count == 0) {
        return -1.0;
    }

    /* Scan from high to low (matching the internal adaptive_hist_p99 approach)
     * to correctly account for the overflow bucket. The target is the number
     * of samples in the top (100 - percentile)% of the distribution. */
    uint32_t total = hist->total_count;
    uint64_t target = (uint64_t)(((100.0 - percentile) / 100.0) * total);
    if (target == 0 && percentile < 100.0) {
        target = 1;
    }

    /* Check overflow bucket first */
    uint64_t count = hist->overflow;
    if (count >= target) {
        return (double)hist->max_tracked_us / 1000.0;
    }

    /* Scan buckets from high to low */
    for (int i = AURA_HISTOGRAM_BUCKETS - 1; i >= 0; i--) {
        count += hist->buckets[i];
        if (count >= target) {
            /* Return bucket midpoint in ms using tier metadata for reverse mapping */
            double mid_us = 0.0;
            for (int t = hist->tier_count - 1; t >= 0; t--) {
                if (i >= hist->tier_base_bucket[t]) {
                    int offset = i - hist->tier_base_bucket[t];
                    mid_us = hist->tier_start_us[t] + (offset + 0.5) * hist->tier_width_us[t];
                    break;
                }
            }
            return mid_us / 1000.0;
        }
    }

    /* Bucket sum didn't reach target — likely a snapshot inconsistency */
    return -1.0;
}

int aura_histogram_bucket_upper_bound_us(const aura_histogram_t *hist, int bucket) {
    if (!hist || bucket < 0 || bucket >= AURA_HISTOGRAM_BUCKETS) {
        return 0;
    }
    for (int t = hist->tier_count - 1; t >= 0; t--) {
        if (bucket >= hist->tier_base_bucket[t]) {
            int offset = bucket - hist->tier_base_bucket[t];
            return hist->tier_start_us[t] + (offset + 1) * hist->tier_width_us[t];
        }
    }
    return 0;
}
