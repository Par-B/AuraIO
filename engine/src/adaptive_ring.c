// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file adaptive_ring.c
 * @brief io_uring ring management implementation
 *
 * Wraps io_uring with request tracking and adaptive control integration.
 */

#define _GNU_SOURCE
#include "adaptive_ring.h"
#include "internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>

/* Feature detection: check if io_uring_prep_ftruncate exists in liburing
 * This was added in liburing 2.7 (May 2024).
 * Use version detection instead of opcode detection (opcodes are enums, not macros).
 */
#if defined(__has_include)
#    if __has_include(<liburing/io_uring_version.h>)
#        include <liburing/io_uring_version.h>
#        if (IO_URING_VERSION_MAJOR > 2) || \
            (IO_URING_VERSION_MAJOR == 2 && IO_URING_VERSION_MINOR >= 7)
#            define HAVE_FTRUNCATE_SUPPORT 1
#        endif
#    endif
#endif

/* If liburing < 2.7, provide a stub that always fails with ENOSYS.
 * The stub is only compiled when HAVE_FTRUNCATE_SUPPORT is not defined,
 * so there's no conflict with the real function from newer liburing versions.
 */
#ifndef HAVE_FTRUNCATE_SUPPORT
static inline void io_uring_prep_ftruncate(struct io_uring_sqe *sqe, int fd, loff_t len) {
    (void)sqe;
    (void)fd;
    (void)len;
    /* This will never execute - we return ENOSYS in ring_submit_ftruncate() */
}
#endif

/* Per-thread callback context depth.
 * >0 means current thread is inside AuraIO completion callback(s). */
static _Thread_local int callback_context_depth = 0;

/** Per-thread pointer to the last submitted SQE.
 *  Set by ring_submit_* macros so that aura_request_set_linked() can
 *  retroactively apply IOSQE_IO_LINK on the already-prepped SQE. */
static _Thread_local struct io_uring_sqe *tls_last_sqe = NULL;

struct io_uring_sqe *ring_get_last_sqe(void) {
    return tls_last_sqe;
}

void ring_clear_last_sqe(void) {
    tls_last_sqe = NULL;
}

/* Conditional locking helpers are in adaptive_ring.h (inline). */

/**
 * Increment queued_sqes by 1. Uses relaxed ordering since this is always
 * called under ring->lock; the atomic type exists only for the lock-free
 * read in ring_should_flush().
 */
static inline void queued_sqes_inc(ring_ctx_t *ctx) {
    atomic_fetch_add_explicit(&ctx->queued_sqes, 1, memory_order_relaxed);
}

/** Returns true for ops whose CQE result represents a byte count. */
static inline bool op_is_data_transfer(aura_op_type_t op) {
    return op == AURA_OP_READ || op == AURA_OP_WRITE || op == AURA_OP_READV ||
           op == AURA_OP_WRITEV || op == AURA_OP_READ_FIXED || op == AURA_OP_WRITE_FIXED;
}

/* ============================================================================
 * Ring Lifecycle
 * ============================================================================ */

int ring_init(ring_ctx_t *ctx, int queue_depth, int cpu_id, const ring_options_t *options) {
    if (!ctx || queue_depth < 1) {
        errno = EINVAL;
        return (-1);
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->cpu_id = cpu_id;
    ctx->max_requests = queue_depth;

    /* Initialize per-ring locks */
    int ret = pthread_mutex_init(&ctx->lock, NULL);
    if (ret != 0) {
        errno = ret;
        return (-1);
    }
    ret = pthread_mutex_init(&ctx->cq_lock, NULL);
    if (ret != 0) {
        pthread_mutex_destroy(&ctx->lock);
        errno = ret;
        return (-1);
    }

    /* NOTE: CPU pinning removed - ring_init() is called from the main thread.
     * The cpu_id is stored for reference but affinity is not changed.
     * Users who need CPU pinning should do it in their worker threads. */

    /* Initialize io_uring with performance flags.
     *
     * IORING_SETUP_COOP_TASKRUN (kernel 5.19+): completions are delivered
     * cooperatively rather than via task_work interrupts. Reduces latency
     * variance and context switches. Safe for multi-threaded usage.
     *
     * Note: IORING_SETUP_SINGLE_ISSUER is only used in THREAD_LOCAL mode
     * where each thread exclusively owns its ring. In other modes, any
     * thread can submit to any ring via select_ring() + per-ring mutex. */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

#ifdef IORING_SETUP_COOP_TASKRUN
    params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif

    bool try_sqpoll = options && options->enable_sqpoll;
    bool try_single_issuer = options && options->enable_single_issuer;
    bool try_disabled = options && options->start_disabled;

/* Helper: apply SQPOLL params if requested */
#define APPLY_SQPOLL_PARAMS(p)                                                                    \
    do {                                                                                          \
        if (try_sqpoll) {                                                                         \
            (p)->flags |= IORING_SETUP_SQPOLL;                                                    \
            (p)->sq_thread_idle = (options->sqpoll_idle_ms > 0) ? options->sqpoll_idle_ms : 1000; \
        }                                                                                         \
    } while (0)

/* Helper: apply SINGLE_ISSUER + R_DISABLED if requested */
#define APPLY_SINGLE_ISSUER_PARAMS(p, si, rd)              \
    do {                                                   \
        if (si) {                                          \
            (p)->flags |= IORING_SETUP_SINGLE_ISSUER;      \
            if (rd) (p)->flags |= IORING_SETUP_R_DISABLED; \
        }                                                  \
    } while (0)

    APPLY_SQPOLL_PARAMS(&params);
    APPLY_SINGLE_ISSUER_PARAMS(&params, try_single_issuer, try_disabled);

    ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);

    /* If init failed, retry without optional flags.
     * COOP_TASKRUN/SINGLE_ISSUER may not be supported on older kernels,
     * and SQPOLL requires root/CAP_SYS_NICE. */
    bool got_single_issuer = false;
    if (ret < 0) {
        /* Retry without SINGLE_ISSUER/R_DISABLED but keep COOP_TASKRUN */
        memset(&params, 0, sizeof(params));
#ifdef IORING_SETUP_COOP_TASKRUN
        params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif
        APPLY_SQPOLL_PARAMS(&params);
        ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
        ctx->sqpoll_enabled = try_sqpoll && (ret >= 0);

        /* If still failed, retry without COOP_TASKRUN */
        if (ret < 0) {
            memset(&params, 0, sizeof(params));
            APPLY_SQPOLL_PARAMS(&params);
            ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
            ctx->sqpoll_enabled = try_sqpoll && (ret >= 0);
        }

        /* If SQPOLL also failed, try bare minimum */
        if (ret < 0 && try_sqpoll) {
            memset(&params, 0, sizeof(params));
            ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
            ctx->sqpoll_enabled = false;
        }
    } else {
        ctx->sqpoll_enabled = try_sqpoll;
        got_single_issuer = try_single_issuer;
    }
#undef APPLY_SQPOLL_PARAMS
#undef APPLY_SINGLE_ISSUER_PARAMS

    ctx->single_issuer_enabled = got_single_issuer;
    ctx->needs_enable = got_single_issuer && try_disabled;

    if (ret < 0) {
        errno = -ret;
        goto cleanup_mutex;
    }
    ctx->ring_initialized = true;

    /* Allocate request tracking.
     * Cache-line aligned so requests[0] starts on a 64-byte boundary.
     * Combined with the 128-byte struct size this guarantees each request
     * occupies exactly 2 cache lines with no neighbor sharing. */
    ctx->requests = aligned_alloc(64, (size_t)queue_depth * sizeof(aura_request_t));
    if (!ctx->requests) {
        goto cleanup_ring;
    }
    memset(ctx->requests, 0, (size_t)queue_depth * sizeof(aura_request_t));

    ctx->free_request_stack = malloc((size_t)queue_depth * sizeof(int));
    if (!ctx->free_request_stack) {
        goto cleanup_requests;
    }

    /* Initialize free stack (all slots available) */
    for (int i = 0; i < queue_depth; i++) {
        ctx->free_request_stack[i] = i;
        ctx->requests[i].op_idx = i;
        ctx->requests[i].uses_registered_buffer = false;
        ctx->requests[i].uses_registered_file = false;
        ctx->requests[i].in_pool = true;
        atomic_init(&ctx->requests[i].pending, false);
    }
    ctx->free_request_count = queue_depth;
    atomic_init(&ctx->fixed_buf_inflight, 0);
    atomic_init(&ctx->fixed_file_inflight, 0);

    /* Set CQE drain batch size based on queue depth */
    int batch = queue_depth >> 2;
    if (batch < RING_MIN_POLL_BATCH) batch = RING_MIN_POLL_BATCH;
    if (batch > RING_MAX_POLL_BATCH) batch = RING_MAX_POLL_BATCH;
    ctx->poll_batch_size = batch;

    /* Initialize adaptive controller */
    int initial_inflight = queue_depth / 4;
    if (initial_inflight < 4) initial_inflight = 4;
    if (initial_inflight > queue_depth) initial_inflight = queue_depth;

    if (adaptive_init(&ctx->adaptive, queue_depth, initial_inflight, 4) != 0) {
        goto cleanup_free_stack;
    }

    return (0);

cleanup_free_stack:
    free(ctx->free_request_stack);
cleanup_requests:
    free(ctx->requests);
cleanup_ring:
    io_uring_queue_exit(&ctx->ring);
    ctx->ring_initialized = false;
cleanup_mutex:
    pthread_mutex_destroy(&ctx->cq_lock);
    pthread_mutex_destroy(&ctx->lock);
    return (-1);
}

bool ring_in_callback_context(void) {
    return callback_context_depth > 0;
}

int ring_enable(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }
    if (!ctx->needs_enable) {
        return (0); /* Already enabled or not created with R_DISABLED */
    }
    int ret = io_uring_enable_rings(&ctx->ring);
    if (ret < 0) {
        errno = -ret;
        return (-1);
    }
    ctx->needs_enable = false;
    return (0);
}

void ring_destroy(ring_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    /* Enable disabled rings before destroying — io_uring_submit fails on
     * disabled rings, and we need to flush/drain any pending SQEs. */
    if (ctx->needs_enable) {
        (void)ring_enable(ctx);
    }

    /* Reject new submissions before draining. */
    atomic_store_explicit(&ctx->shutting_down, true, memory_order_release);

    /* Flush any batched-but-not-submitted SQEs before draining.
     * Without this, ring_wait() would block waiting for CQEs that
     * correspond to SQEs never submitted to the kernel. */
    bool st = ring_lock(ctx);
    (void)ring_flush(ctx);
    ring_unlock(ctx, st);

    /* Wait for pending operations to complete with a timeout.
     * Benign race: pending_count read without lock. Worst case is one extra
     * ring_wait() iteration, which is acceptable during shutdown.
     * Timeout after 10 seconds to avoid infinite hang on stuck ops
     * (e.g., hung NFS mount, stuck SCSI device). */
    /** Maximum drain attempts before giving up (100 * 100ms = 10s timeout). */
#define RING_DESTROY_MAX_DRAIN_ATTEMPTS 100
    int drain_attempts = 0;
    while (atomic_load_explicit(&ctx->pending_count, memory_order_acquire) > 0 &&
           drain_attempts < RING_DESTROY_MAX_DRAIN_ATTEMPTS) {
        /* Retry flush in case an earlier submit failed transiently. */
        st = ring_lock(ctx);
        (void)ring_flush(ctx);
        ring_unlock(ctx, st);
        ring_wait(ctx, 100);
        drain_attempts++;
    }

    if (atomic_load_explicit(&ctx->pending_count, memory_order_acquire) > 0) {
        aura_log(AURA_LOG_WARN, "ring_destroy timed out with %d ops still pending",
                 (int)atomic_load_explicit(&ctx->pending_count, memory_order_acquire));
    }

    adaptive_destroy(&ctx->adaptive);

    /* Close the ring BEFORE freeing requests to prevent the kernel from
     * delivering CQEs to freed request memory if the drain timed out. */
    if (ctx->ring_initialized) {
        ctx->ring_initialized = false;
        io_uring_queue_exit(&ctx->ring);
    }

    free(ctx->free_request_stack);
    ctx->free_request_stack = NULL;

    free(ctx->requests);
    ctx->requests = NULL;

    /* Destroy per-ring locks */
    pthread_mutex_destroy(&ctx->cq_lock);
    pthread_mutex_destroy(&ctx->lock);
}

/* ============================================================================
 * Request Management
 * ============================================================================ */

aura_request_t *ring_get_request(ring_ctx_t *ctx, int *op_idx) {
    if (!ctx || ctx->free_request_count == 0) {
        return NULL;
    }

    int idx = ctx->free_request_stack[--ctx->free_request_count];
    aura_request_t *req = &ctx->requests[idx];

    /* Zero only the variant fields that differ between operation types.
     * Fields always set by callers (fd, callback, user_data, ring_idx,
     * submit_time_ns, op_type) are omitted — they'll be overwritten.
     * op_idx is preserved (assigned at ring init, never changes). */
    req->offset = 0;
    req->buffer = NULL;
    req->len = 0;
    req->buf_index = 0;
    req->iovcnt = 0;
    req->buf_offset = 0;
    req->iov = NULL;
    req->uses_registered_buffer = false;
    req->uses_registered_file = false;
    req->original_fd = -1;
    req->in_pool = false;
    req->linked = false;
    atomic_store_explicit(&req->pending, false, memory_order_relaxed);

    if (op_idx) {
        *op_idx = idx;
    }

    return req;
}

void ring_put_request(ring_ctx_t *ctx, int op_idx) {
    if (!ctx || op_idx < 0 || op_idx >= ctx->max_requests) {
        return;
    }

    /* Guard against double-free: check per-slot in_pool flag.
     * This catches all double-frees, not just the full-stack case. */
    if (ctx->requests[op_idx].in_pool) {
        aura_log(AURA_LOG_ERR, "BUG: ring_put_request double-free detected (op_idx=%d, ring=%p)",
                 op_idx, (void *)ctx);
        return;
    }

    /* Guard against returning an index that is still marked pending.
     * If pending is true, this slot was already returned or was never
     * properly completed — returning it again would alias requests. */
    if (atomic_load_explicit(&ctx->requests[op_idx].pending, memory_order_relaxed)) {
        aura_log(AURA_LOG_ERR,
                 "BUG: ring_put_request called on pending request (op_idx=%d, ring=%p)", op_idx,
                 (void *)ctx);
        return;
    }

    ctx->requests[op_idx].in_pool = true;
    ctx->free_request_stack[ctx->free_request_count++] = op_idx;
}

/* ============================================================================
 * Submission Operations
 * ============================================================================ */

static inline void sqe_apply_fixed_file(struct io_uring_sqe *sqe, const aura_request_t *req) {
    if (req->uses_registered_file) {
        sqe->flags |= IOSQE_FIXED_FILE;
    }
}

static inline void sqe_apply_link(struct io_uring_sqe *sqe, const aura_request_t *req) {
    if (req->linked) {
        sqe->flags |= IOSQE_IO_LINK;
    }
}

/* ============================================================================
 * Shared Submission Helpers
 * ============================================================================ */

/** Common preamble: validate ctx/req and get SQE. */
static struct io_uring_sqe *submit_get_sqe(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return NULL;
    }
    if (atomic_load_explicit(&ctx->shutting_down, memory_order_acquire)) {
        errno = ESHUTDOWN;
        return NULL;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return NULL;
    }
    return sqe;
}

/** Common postamble for data-transfer submissions (latency-sampled). */
static inline void data_finish(ring_ctx_t *ctx, aura_request_t *req, aura_op_type_t op) {
    req->op_type = op;
    int mask = adaptive_is_passthrough(&ctx->adaptive) ? PASSTHROUGH_SAMPLE_MASK
                                                       : RING_LATENCY_SAMPLE_MASK;
    unsigned int sc = atomic_load_explicit(&ctx->sample_counter, memory_order_relaxed);
    atomic_store_explicit(&ctx->sample_counter, sc + 1, memory_order_relaxed);
    req->submit_time_ns = (sc & mask) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);
    bool st = atomic_load_explicit(&ctx->single_thread, memory_order_relaxed);
    ATOMIC_ADD_ST(ctx->queued_sqes, 1, st, memory_order_relaxed);
    ATOMIC_ADD_ST(ctx->pending_count, 1, st, memory_order_release);
}

/** Common postamble for metadata submissions (skip AIMD sampling). */
static inline void meta_finish(ring_ctx_t *ctx, aura_request_t *req, aura_op_type_t op) {
    req->op_type = op;
    req->submit_time_ns = 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);
    bool st = atomic_load_explicit(&ctx->single_thread, memory_order_relaxed);
    ATOMIC_ADD_ST(ctx->queued_sqes, 1, st, memory_order_relaxed);
    ATOMIC_ADD_ST(ctx->pending_count, 1, st, memory_order_release);
}

/**
 * Macro for data-transfer submissions: validate, get SQE, prep, finish.
 * Handles fixed-file flag, latency sampling, and pending bookkeeping.
 */
#define RING_SUBMIT_DATA_OP(ctx, req, op_type, prep_call)    \
    do {                                                     \
        struct io_uring_sqe *sqe = submit_get_sqe(ctx, req); \
        if (!sqe) return (-1);                               \
        prep_call;                                           \
        sqe_apply_fixed_file(sqe, req);                      \
        sqe_apply_link(sqe, req);                            \
        io_uring_sqe_set_data(sqe, req);                     \
        tls_last_sqe = sqe;                                  \
        data_finish(ctx, req, op_type);                      \
        return (0);                                          \
    } while (0)

int ring_submit_read(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_DATA_OP(ctx, req, AURA_OP_READ,
                        io_uring_prep_read(sqe, req->fd, req->buffer, req->len, req->offset));
}

int ring_submit_write(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_DATA_OP(ctx, req, AURA_OP_WRITE,
                        io_uring_prep_write(sqe, req->fd, req->buffer, req->len, req->offset));
}

int ring_submit_readv(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || !req->iov || req->iovcnt <= 0 || req->iovcnt > IOV_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_DATA_OP(ctx, req, AURA_OP_READV,
                        io_uring_prep_readv(sqe, req->fd, req->iov, req->iovcnt, req->offset));
}

int ring_submit_writev(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || !req->iov || req->iovcnt <= 0 || req->iovcnt > IOV_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_DATA_OP(ctx, req, AURA_OP_WRITEV,
                        io_uring_prep_writev(sqe, req->fd, req->iov, req->iovcnt, req->offset));
}

int ring_submit_cancel(ring_ctx_t *ctx, aura_request_t *req, aura_request_t *target) {
    if (!target) {
        errno = EINVAL;
        return (-1);
    }
    /* Caller MUST hold ring_lock to prevent the target slot from being
     * recycled between this pending check and the cancel SQE submission. */
    if (!atomic_load_explicit(&target->pending, memory_order_acquire)) {
        errno = EALREADY;
        return (-1);
    }
    struct io_uring_sqe *sqe = submit_get_sqe(ctx, req);
    if (!sqe) return (-1);
    io_uring_prep_cancel(sqe, target, 0);
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_CANCEL);
    tls_last_sqe = NULL; /* Prevent stale SQE from being linked after cancel */
    return (0);
}

int ring_submit_read_fixed(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_DATA_OP(
        ctx, req, AURA_OP_READ_FIXED,
        io_uring_prep_read_fixed(sqe, req->fd, req->buffer, req->len, req->offset, req->buf_index));
}

int ring_submit_write_fixed(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_DATA_OP(ctx, req, AURA_OP_WRITE_FIXED,
                        io_uring_prep_write_fixed(sqe, req->fd, req->buffer, req->len, req->offset,
                                                  req->buf_index));
}

/* ============================================================================
 * Lifecycle Metadata Operations
 *
 * These skip AIMD latency sampling (submit_time_ns = 0).
 * ============================================================================ */

/**
 * Macro to eliminate boilerplate in metadata operations.
 * Usage: RING_SUBMIT_META_OP(ctx, req, OP_TYPE, io_uring_prep_call);
 *
 * Handles: validation, SQE acquisition, set_data, finish, and return.
 */
#define RING_SUBMIT_META_OP(ctx, req, op_type, prep_call)    \
    do {                                                     \
        struct io_uring_sqe *sqe = submit_get_sqe(ctx, req); \
        if (!sqe) return (-1);                               \
        prep_call;                                           \
        sqe_apply_link(sqe, req);                            \
        io_uring_sqe_set_data(sqe, req);                     \
        tls_last_sqe = sqe;                                  \
        meta_finish(ctx, req, op_type);                      \
        return (0);                                          \
    } while (0)

/**
 * Variant for operations that require fixed file handling.
 */
#define RING_SUBMIT_META_OP_FIXED(ctx, req, op_type, prep_call) \
    do {                                                        \
        struct io_uring_sqe *sqe = submit_get_sqe(ctx, req);    \
        if (!sqe) return (-1);                                  \
        prep_call;                                              \
        sqe_apply_fixed_file(sqe, req);                         \
        sqe_apply_link(sqe, req);                               \
        io_uring_sqe_set_data(sqe, req);                        \
        tls_last_sqe = sqe;                                     \
        meta_finish(ctx, req, op_type);                         \
        return (0);                                             \
    } while (0)

int ring_submit_fsync(ring_ctx_t *ctx, aura_request_t *req) {
    RING_SUBMIT_META_OP_FIXED(ctx, req, AURA_OP_FSYNC, io_uring_prep_fsync(sqe, req->fd, 0));
}

int ring_submit_fdatasync(ring_ctx_t *ctx, aura_request_t *req) {
    RING_SUBMIT_META_OP_FIXED(ctx, req, AURA_OP_FDATASYNC,
                              io_uring_prep_fsync(sqe, req->fd, IORING_FSYNC_DATASYNC));
}

int ring_submit_openat(ring_ctx_t *ctx, aura_request_t *req) {
    RING_SUBMIT_META_OP(ctx, req, AURA_OP_OPENAT,
                        io_uring_prep_openat(sqe, req->fd, req->meta.open.pathname,
                                             req->meta.open.flags, req->meta.open.mode));
}

int ring_submit_close(ring_ctx_t *ctx, aura_request_t *req) {
    /* No sqe_apply_fixed_file: close always uses the raw fd.
     * IOSQE_FIXED_FILE on close means "unregister slot", not "close fd". */
    RING_SUBMIT_META_OP(ctx, req, AURA_OP_CLOSE, io_uring_prep_close(sqe, req->fd));
}

int ring_submit_statx(ring_ctx_t *ctx, aura_request_t *req) {
    RING_SUBMIT_META_OP(ctx, req, AURA_OP_STATX,
                        io_uring_prep_statx(sqe, req->fd, req->meta.statx.pathname,
                                            req->meta.statx.flags, req->meta.statx.mask,
                                            req->meta.statx.buf));
}

int ring_submit_fallocate(ring_ctx_t *ctx, aura_request_t *req) {
    if (req && req->len > (size_t)INT64_MAX) {
        errno = EOVERFLOW;
        return (-1);
    }
    RING_SUBMIT_META_OP_FIXED(ctx, req, AURA_OP_FALLOCATE,
                              io_uring_prep_fallocate(sqe, req->fd, req->meta.fallocate.mode,
                                                      req->offset, (off_t)req->len));
}

int ring_submit_ftruncate(ring_ctx_t *ctx, aura_request_t *req) {
    if (req && req->len > (size_t)INT64_MAX) {
        errno = EOVERFLOW;
        return (-1);
    }
#ifndef HAVE_FTRUNCATE_SUPPORT
    /* liburing < 2.7 doesn't support ftruncate - return ENOSYS
     * The test suite will detect this and skip the test */
    (void)ctx;
    (void)req;
    errno = ENOSYS;
    return (-1);
#else
    RING_SUBMIT_META_OP_FIXED(ctx, req, AURA_OP_FTRUNCATE,
                              io_uring_prep_ftruncate(sqe, req->fd, (loff_t)req->len));
#endif
}

int ring_submit_sync_file_range(ring_ctx_t *ctx, aura_request_t *req) {
    if (!req || req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }
    RING_SUBMIT_META_OP_FIXED(ctx, req, AURA_OP_SYNC_FILE_RANGE,
                              io_uring_prep_sync_file_range(sqe, req->fd, (unsigned)req->len,
                                                            req->offset,
                                                            req->meta.sync_range.flags));
}

/* ============================================================================
 * Flush and Completion Processing
 * ============================================================================ */

int ring_flush(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return (0);
    }
    int queued = atomic_load_explicit(&ctx->queued_sqes, memory_order_relaxed);
    if (queued == 0) {
        return (0);
    }
    tls_last_sqe = NULL; /* Prevent stale SQE access after flush */

    int submitted = io_uring_submit(&ctx->ring);
    if (submitted < 0) {
        /* Submit failed. SQEs remain in the kernel SQ for retry on the next
         * flush. queued_sqes is intentionally NOT reset: if some SQEs were
         * somehow consumed despite the error, the count stays conservatively
         * high (causing benign no-op retries) rather than dropping to zero
         * and losing track of pending SQEs.
         * Persistent failures will be caught by ring_destroy's timeout. */
        aura_log(AURA_LOG_WARN, "io_uring_submit failed: %s (queued=%d)", strerror(-submitted),
                 queued);
        errno = -submitted;
        return (-1);
    }

    /* Record submit for batch optimizer.
     * Use submitted (not queued_sqes) in case of partial submit. */
    adaptive_record_submit(&ctx->adaptive, submitted);
    if (submitted < queued) {
        aura_log(AURA_LOG_WARN, "partial submit: %d of %d SQEs submitted", submitted, queued);
        /* Un-submitted SQEs remain in the kernel SQ ring and will be
         * submitted on the next io_uring_submit call, eventually producing
         * CQEs.  Do NOT correct pending_count here — doing so would cause
         * a double-decrement when those CQEs arrive via ring_retire_batch. */
    }
    /* Subtract the submitted count atomically.  Use a CAS loop to clamp:
     * io_uring_submit may flush SQEs left over from a prior partial submit,
     * returning more than our queued_sqes count.  Without clamping, the
     * counter could go negative and break ring_should_flush. */
    if (submitted > 0) {
        int old = atomic_load_explicit(&ctx->queued_sqes, memory_order_relaxed);
        for (;;) {
            int sub = submitted < old ? submitted : old;
            int desired = old - sub;
            if (atomic_compare_exchange_weak_explicit(&ctx->queued_sqes, &old, desired,
                                                      memory_order_relaxed, memory_order_relaxed))
                break;
        }
    }

    return submitted;
}

/**
 * Process a single completion with pre-extracted CQE data.
 *
 * Called after the CQE has been consumed (cqe_seen called) with the
 * request pointer and result extracted under cq_lock protection.
 *
 * @param ctx    Ring context
 * @param req    Request from io_uring_cqe_get_data (may be NULL)
 * @param result Result from cqe->res
 */
/**
 * Batch entry for deferred counter updates and slot retirement.
 * Populated during callback phase, consumed during batched lock phase.
 */
typedef struct {
    int op_idx;
    ssize_t result;
    aura_op_type_t op_type;
} retire_entry_t;

/**
 * Process a single completion's callback phase (no lock held).
 *
 * Does adaptive recording, sets pending=false, invokes user callback.
 * Returns the retirement info needed for deferred counter updates.
 * Must be called before ring_retire_batch().
 *
 * When single_thread is true, uses plain load+store instead of atomic RMW
 * (atomic_exchange, atomic_fetch_sub) to avoid lock-prefixed instructions.
 */
static retire_entry_t process_completion(ring_ctx_t *ctx, aura_request_t *req, ssize_t result) {
    retire_entry_t retire = { .op_idx = -1, .result = result, .op_type = AURA_OP_CANCEL };

    if (!req) {
        return retire;
    }

    bool st = atomic_load_explicit(&ctx->single_thread, memory_order_relaxed);

    /* Guard against double-completion: if pending is already false, this CQE
     * is a duplicate (e.g., io_uring can deliver both a cancel CQE and the
     * original op's CQE).  Skip processing to avoid double-decrementing
     * inflight counters and double-freeing the request slot. */
    if (st) {
        /* Single-thread: plain load+store avoids lock-prefixed xchg */
        if (!atomic_load_explicit(&req->pending, memory_order_relaxed)) return retire;
        atomic_store_explicit(&req->pending, false, memory_order_relaxed);
    } else {
        if (!atomic_exchange_explicit(&req->pending, false, memory_order_acquire)) {
            return retire;
        }
    }

    /* Save callback info and retirement data BEFORE any state changes.
     * The callback may submit new operations that reuse this request slot,
     * so we must capture everything we need before potential reuse. */
    retire.op_idx = req->op_idx;
    retire.op_type = req->op_type;
    bool uses_registered_buffer = req->uses_registered_buffer;
    bool uses_registered_file = req->uses_registered_file;
    aura_callback_t callback = req->callback;
    void *user_data = req->user_data;

    /* Record completion for adaptive controller BEFORE callback.
     * Skip cancel ops and non-sampled ops (submit_time_ns == 0).
     * req fields are safe to read here: slot is still allocated
     * (ring_put_request hasn't been called yet). */
    if (req->submit_time_ns != 0) {
        int64_t now_ns = get_time_ns();
        int64_t latency_ns = now_ns - req->submit_time_ns;
        size_t bytes = (result > 0) ? (size_t)result : 0;
        if (latency_ns > 0) adaptive_record_completion(&ctx->adaptive, latency_ns, bytes);
    }

    /* Decrement registered buffer/file inflight counters BEFORE the callback.
     * If the callback calls aura_request_unregister(), it checks inflight == 0
     * to decide whether to finalize unregistration.  Decrementing after the
     * callback would leave the count stale during the check, potentially
     * causing the final unregister to never fire.
     * Single-thread: plain load+store avoids lock-prefixed instructions. */
    if (uses_registered_buffer) {
        ATOMIC_SUB_ST(ctx->fixed_buf_inflight, 1, st, memory_order_relaxed);
    }
    if (uses_registered_file) {
        ATOMIC_SUB_ST(ctx->fixed_file_inflight, 1, st, memory_order_relaxed);
    }

    /* pending was already cleared by atomic_exchange in the double-completion
     * guard above.  No separate store needed. */

    /* Invoke callback WITHOUT holding lock to prevent deadlock.
     * If the callback calls aura_read/write, it will try to acquire
     * the lock, which would deadlock with non-recursive mutexes.
     * The req pointer remains valid because ring_put_request hasn't
     * been called yet. */
    if (callback) {
        int saved_depth = callback_context_depth;
        callback_context_depth = saved_depth + 1;
        callback((aura_request_t *)req, result, user_data);
        callback_context_depth = saved_depth;
    }

    return retire;
}

/**
 * Batch-retire completed requests under a single ring->lock hold.
 *
 * Updates ops_completed, bytes_completed, pending_count, and returns
 * all request slots to the free pool. This amortizes mutex overhead
 * from O(completions) to O(1) per poll/wait cycle.
 *
 * pending_count is inflated during the callback phase (between
 * process_completion and ring_retire_batch). This may cause spurious
 * EAGAIN from ring_can_submit, which callers handle gracefully.
 */
static void ring_retire_batch(ring_ctx_t *ctx, const retire_entry_t *entries, int count) {
    if (count <= 0) {
        return;
    }

    int pending_delta = 0;

    bool st = ring_lock(ctx);
    for (int i = 0; i < count; i++) {
        if (entries[i].op_idx < 0) {
            continue; /* NULL/unknown req — no matching submit, don't adjust pending */
        }
        /* Don't count cancel operations in ops_completed — they are
         * internal bookkeeping, not user I/O operations. */
        if (entries[i].op_type != AURA_OP_CANCEL) {
            atomic_fetch_add_explicit(&ctx->ops_completed, 1, memory_order_relaxed);
            /* Only accumulate bytes for actual data transfer ops.
             * Non-transfer ops like openat return fd numbers, not byte counts. */
            if (entries[i].result > 0 && op_is_data_transfer(entries[i].op_type)) {
                atomic_fetch_add_explicit(&ctx->bytes_completed, entries[i].result,
                                          memory_order_relaxed);
            }
        }
        ring_put_request(ctx, entries[i].op_idx);
        pending_delta++;
    }
    /* Single atomic decrement for the entire batch instead of one per CQE.
     * Use release ordering to pair with acquire loads in ring_destroy/ring_wait. */
    atomic_fetch_sub_explicit(&ctx->pending_count, pending_delta, memory_order_release);
    ring_unlock(ctx, st);
}

/**
 * Drain all available CQEs in batches.
 *
 * Shared by ring_poll, ring_wait (non-blocking), and ring_wait (after blocking).
 * Extracts CQEs under cq_lock, processes callbacks without locks, then retires
 * the batch under ring->lock.
 *
 * Batch size is per-ring, derived from queue depth at init time
 * (queue_depth/4, clamped to [16, 128]). This keeps lock hold time bounded
 * while amortizing lock overhead proportionally to ring capacity.
 *
 * @param ctx Ring context
 * @return Number of completions processed
 */
static int ring_drain_cqes(ring_ctx_t *ctx) {
    const int batch_size = ctx->poll_batch_size;
    struct {
        aura_request_t *req;
        ssize_t result;
    } batch[RING_MAX_POLL_BATCH];
    retire_entry_t retire[RING_MAX_POLL_BATCH];
    int completed = 0;

    while (1) {
        int n = 0;
        bool cq_st = ring_cq_lock(ctx);
        while (n < batch_size) {
            struct io_uring_cqe *cqe;
            if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
                break;
            }
            batch[n].req = io_uring_cqe_get_data(cqe);
            batch[n].result = cqe->res;
            io_uring_cqe_seen(&ctx->ring, cqe);
            if (batch[n].req) TSAN_ACQUIRE(batch[n].req);
            n++;
        }
        ring_cq_unlock(ctx, cq_st);

        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            retire[i] = process_completion(ctx, batch[i].req, batch[i].result);
        }
        ring_retire_batch(ctx, retire, n);
        completed += n;
    }

    return completed;
}

/**
 * Fast CQE drain for single-thread rings.
 *
 * No cq_lock acquisition, no batch staging for retirement.
 * Processes completions inline and retires directly under ring->lock
 * (which is a no-op for single_thread rings).
 */
int ring_drain_cqes_fast(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) return 0;
    int completed = 0;
    struct io_uring_cqe *cqe;

    while (io_uring_peek_cqe(&ctx->ring, &cqe) == 0) {
        aura_request_t *req = io_uring_cqe_get_data(cqe);
        ssize_t result = cqe->res;
        io_uring_cqe_seen(&ctx->ring, cqe);

        if (req) TSAN_ACQUIRE(req);

        retire_entry_t retire = process_completion(ctx, req, result);

        /* Inline retirement — single_thread: plain stores via ATOMIC_*_ST. */
        if (retire.op_idx >= 0) {
            if (retire.op_type != AURA_OP_CANCEL) {
                ATOMIC_ADD_ST(ctx->ops_completed, 1, true, memory_order_relaxed);
                if (retire.result > 0 && op_is_data_transfer(retire.op_type)) {
                    ATOMIC_ADD_ST(ctx->bytes_completed, (uint64_t)retire.result, true,
                                  memory_order_relaxed);
                }
            }
            ring_put_request(ctx, retire.op_idx);
            ATOMIC_SUB_ST(ctx->pending_count, 1, true, memory_order_release);
        }
        completed++;
    }

    return completed;
}

int ring_poll(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return (0);
    }

    if (atomic_load_explicit(&ctx->single_thread, memory_order_acquire))
        return ring_drain_cqes_fast(ctx);

    return ring_drain_cqes(ctx);
}

int ring_wait(ring_ctx_t *ctx, int timeout_ms) {
    if (!ctx || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    if (atomic_load_explicit(&ctx->pending_count, memory_order_acquire) == 0) {
        return (0);
    }

    if (timeout_ms == 0) {
        if (atomic_load_explicit(&ctx->single_thread, memory_order_acquire))
            return ring_drain_cqes_fast(ctx);
        return ring_drain_cqes(ctx);
    }

    /* Blocking wait: try non-blocking peek first, then block if needed. */
    struct io_uring_cqe *cqe;

    bool cq_st = ring_cq_lock(ctx);
    if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
        /* Nothing available - release lock and do blocking wait.
         * We intentionally call io_uring_wait_cqe_timeout without
         * holding cq_lock.  This is safe because:
         * - The wait only reads CQ head/tail (never writes CQ head)
         * - CQ head is only written by io_uring_cqe_seen, always under cq_lock
         * - CQ tail is only written by the kernel with store-release semantics
         * - io_uring_enter (the blocking syscall) is thread-safe
         * This is a stable liburing API contract (peek+wait never advances
         * CQ head — only cqe_seen does), not an implementation detail.
         * This allows other threads to poll/peek CQEs concurrently.
         * After the wait returns, we fall through to ring_drain_cqes
         * which acquires cq_lock, handling the case where another
         * thread consumed the waking CQE first. */
        int ret;
        ring_cq_unlock(ctx, cq_st);

        if (timeout_ms < 0) {
            /* Use 1-second timeout chunks instead of infinite wait to avoid
             * hanging if another thread consumes the waking CQE between our
             * cq_lock release and this call.  Retry while pending_count > 0
             * to honor the "wait forever" contract of timeout_ms == -1. */
            struct __kernel_timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            for (;;) {
                ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &ts);
                if (ret != -ETIME && ret != -EAGAIN) break;
                /* Timeout expired — retry if there are still pending ops */
                if (atomic_load_explicit(&ctx->pending_count, memory_order_acquire) == 0) break;
                if (atomic_load_explicit(&ctx->shutting_down, memory_order_acquire)) break;
            }
        } else {
            struct __kernel_timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000LL;
            ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &ts);
        }

        if (ret < 0) {
            if (ret == -ETIME || ret == -EAGAIN) {
                return ring_drain_cqes(ctx);
            }
            if (ret == -EINTR) {
                /* Signal interrupted the wait — not an error.
                 * Fall through to drain any CQEs that arrived. */
            } else {
                errno = -ret;
                return (-1);
            }
        }
    } else {
        ring_cq_unlock(ctx, cq_st);
        return ring_drain_cqes(ctx);
    }

    int drained = ring_drain_cqes(ctx);

    /* The blocking wait succeeded (ret >= 0) but drain found nothing —
     * another thread consumed the CQE between our unlock and drain.
     * Return 0: no completions were actually processed by this call. */
    if (drained == 0) return 0;

    return drained;
}

/* ============================================================================
 * Adaptive Control Integration
 * ============================================================================ */

bool ring_can_submit(ring_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }

    if (adaptive_is_passthrough(&ctx->adaptive)) return true;

    int limit = adaptive_get_inflight_limit(&ctx->adaptive);
    return atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) < limit;
}

bool ring_should_flush(ring_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }

    /* Advisory check — reads queued_sqes without lock. Safe because a stale
     * value only affects flush timing, not correctness. */
    int threshold = adaptive_get_batch_threshold(&ctx->adaptive);
    return atomic_load_explicit(&ctx->queued_sqes, memory_order_relaxed) >= threshold;
}

int ring_get_fd(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return -1;
    }
    return ctx->ring.ring_fd;
}
