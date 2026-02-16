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

/* Per-thread callback context depth.
 * >0 means current thread is inside AuraIO completion callback(s). */
static _Thread_local int callback_context_depth = 0;

/* Conditional locking helpers are in adaptive_ring.h (inline). */

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
    if (pthread_mutex_init(&ctx->lock, NULL) != 0) {
        return (-1);
    }
    if (pthread_mutex_init(&ctx->cq_lock, NULL) != 0) {
        pthread_mutex_destroy(&ctx->lock);
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
     * Note: IORING_SETUP_SINGLE_ISSUER is NOT used because AuraIO allows
     * any thread to submit to any ring (via select_ring() + per-ring mutex).
     * SINGLE_ISSUER requires a single task per ring's entire lifetime. */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

#ifdef IORING_SETUP_COOP_TASKRUN
    params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif

    bool try_sqpoll = options && options->enable_sqpoll;
    if (try_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        if (options->sqpoll_idle_ms > 0) {
            params.sq_thread_idle = options->sqpoll_idle_ms;
        } else {
            params.sq_thread_idle = 1000; /* Default 1 second */
        }
    }

    int ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);

    /* If init failed, retry without optional flags.
     * COOP_TASKRUN/SINGLE_ISSUER may not be supported on older kernels,
     * and SQPOLL requires root/CAP_SYS_NICE. */
    if (ret < 0) {
        memset(&params, 0, sizeof(params));
        if (try_sqpoll) {
            params.flags |= IORING_SETUP_SQPOLL;
            if (options->sqpoll_idle_ms > 0) {
                params.sq_thread_idle = options->sqpoll_idle_ms;
            } else {
                params.sq_thread_idle = 1000;
            }
        }
        ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
        ctx->sqpoll_enabled = try_sqpoll && (ret >= 0);

        /* If SQPOLL also failed, try bare minimum */
        if (ret < 0 && try_sqpoll) {
            memset(&params, 0, sizeof(params));
            ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
            ctx->sqpoll_enabled = false;
        }
    } else {
        ctx->sqpoll_enabled = try_sqpoll;
    }

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

    ctx->free_request_stack = malloc(queue_depth * sizeof(int));
    if (!ctx->free_request_stack) {
        goto cleanup_requests;
    }

    /* Initialize free stack (all slots available) */
    for (int i = 0; i < queue_depth; i++) {
        ctx->free_request_stack[i] = i;
        ctx->requests[i].op_idx = i;
        ctx->requests[i].uses_registered_buffer = false;
        ctx->requests[i].uses_registered_file = false;
        atomic_init(&ctx->requests[i].pending, false);
    }
    ctx->free_request_count = queue_depth;
    atomic_init(&ctx->fixed_buf_inflight, 0);

    /* Initialize adaptive controller */
    int initial_inflight = queue_depth / 4;
    if (initial_inflight < 4) initial_inflight = 4;
    if (initial_inflight > queue_depth) initial_inflight = queue_depth;

    if (adaptive_init(&ctx->adaptive, queue_depth, initial_inflight) != 0) {
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

void ring_destroy(ring_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    /* Flush any batched-but-not-submitted SQEs before draining.
     * Without this, ring_wait() would block waiting for CQEs that
     * correspond to SQEs never submitted to the kernel. */
    ring_lock(ctx);
    (void)ring_flush(ctx);
    ring_unlock(ctx);

    /* Wait for pending operations to complete with a timeout.
     * Benign race: pending_count read without lock. Worst case is one extra
     * ring_wait() iteration, which is acceptable during shutdown.
     * Timeout after 10 seconds to avoid infinite hang on stuck ops
     * (e.g., hung NFS mount, stuck SCSI device). */
    int drain_attempts = 0;
    while (atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) > 0 &&
           drain_attempts < 100) {
        /* Retry flush in case an earlier submit failed transiently. */
        ring_lock(ctx);
        (void)ring_flush(ctx);
        ring_unlock(ctx);
        ring_wait(ctx, 100);
        drain_attempts++;
    }

    if (atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) > 0) {
        aura_log(AURA_LOG_WARN, "ring_destroy timed out with %d ops still pending",
                 (int)atomic_load_explicit(&ctx->pending_count, memory_order_relaxed));
    }

    adaptive_destroy(&ctx->adaptive);

    free(ctx->free_request_stack);
    ctx->free_request_stack = NULL;

    free(ctx->requests);
    ctx->requests = NULL;

    if (ctx->ring_initialized) {
        io_uring_queue_exit(&ctx->ring);
        ctx->ring_initialized = false;
    }

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

    /* Guard against double-free: if stack is already full, something is wrong */
    if (ctx->free_request_count >= ctx->max_requests) {
        aura_log(AURA_LOG_ERR, "BUG: ring_put_request double-free detected (op_idx=%d, ring=%p)",
                 op_idx, (void *)ctx);
        return;
    }

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

int ring_submit_read(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }
    if (req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_read(sqe, req->fd, req->buffer, req->len, req->offset);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_READ;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

int ring_submit_write(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }
    if (req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_write(sqe, req->fd, req->buffer, req->len, req->offset);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_WRITE;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

int ring_submit_readv(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !req->iov || req->iovcnt <= 0 || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_readv(sqe, req->fd, req->iov, req->iovcnt, req->offset);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_READV;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += iovec_total_len(req->iov, req->iovcnt);

    return (0);
}

int ring_submit_writev(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !req->iov || req->iovcnt <= 0 || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_writev(sqe, req->fd, req->iov, req->iovcnt, req->offset);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_WRITEV;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += iovec_total_len(req->iov, req->iovcnt);

    return (0);
}

int ring_submit_fsync(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_fsync(sqe, req->fd, 0);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_FSYNC;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);

    return (0);
}

int ring_submit_fdatasync(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    /* IORING_FSYNC_DATASYNC flag makes it behave like fdatasync */
    io_uring_prep_fsync(sqe, req->fd, IORING_FSYNC_DATASYNC);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_FDATASYNC;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);

    return (0);
}

int ring_submit_cancel(ring_ctx_t *ctx, aura_request_t *req, aura_request_t *target) {
    if (!ctx || !req || !target || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    /* io_uring cancel uses user_data to identify the request */
    io_uring_prep_cancel(sqe, target, 0);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_CANCEL;
    req->submit_time_ns = 0; /* Cancel ops skip latency tracking */
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);

    return (0);
}

int ring_submit_read_fixed(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    /* io_uring_prep_read_fixed takes buffer address, but for registered buffers
     * we need to compute it from the registered iovec. The caller must provide
     * the actual buffer address in req->buffer computed from buf_index + buf_offset. */
    io_uring_prep_read_fixed(sqe, req->fd, req->buffer, req->len, req->offset, req->buf_index);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_READ_FIXED;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

int ring_submit_write_fixed(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_write_fixed(sqe, req->fd, req->buffer, req->len, req->offset, req->buf_index);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURA_OP_WRITE_FIXED;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

/* ============================================================================
 * Lifecycle Metadata Operations
 *
 * These skip AIMD latency sampling (submit_time_ns = 0) and don't update
 * bytes_submitted — metadata ops are not throughput-sensitive.
 * ============================================================================ */

/** Common preamble for metadata submission: validate + get SQE. */
static struct io_uring_sqe *meta_get_sqe(ring_ctx_t *ctx, aura_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return NULL;
    }
    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return NULL;
    }
    return sqe;
}

/** Common postamble for metadata submission. */
static void meta_finish(ring_ctx_t *ctx, aura_request_t *req, aura_op_type_t op) {
    req->op_type = op;
    req->submit_time_ns = 0; /* Skip AIMD sampling */
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);
    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
}

int ring_submit_openat(ring_ctx_t *ctx, aura_request_t *req) {
    struct io_uring_sqe *sqe = meta_get_sqe(ctx, req);
    if (!sqe) return (-1);

    io_uring_prep_openat(sqe, req->fd, req->meta.open.pathname, req->meta.open.flags,
                         req->meta.open.mode);
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_OPENAT);
    return (0);
}

int ring_submit_close(ring_ctx_t *ctx, aura_request_t *req) {
    struct io_uring_sqe *sqe = meta_get_sqe(ctx, req);
    if (!sqe) return (-1);

    io_uring_prep_close(sqe, req->fd);
    /* No sqe_apply_fixed_file: close always uses the raw fd.
     * IOSQE_FIXED_FILE on close means "unregister slot", not "close fd". */
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_CLOSE);
    return (0);
}

int ring_submit_statx(ring_ctx_t *ctx, aura_request_t *req) {
    struct io_uring_sqe *sqe = meta_get_sqe(ctx, req);
    if (!sqe) return (-1);

    io_uring_prep_statx(sqe, req->fd, req->meta.statx.pathname, req->meta.statx.flags,
                        req->meta.statx.mask, req->meta.statx.buf);
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_STATX);
    return (0);
}

int ring_submit_fallocate(ring_ctx_t *ctx, aura_request_t *req) {
    struct io_uring_sqe *sqe = meta_get_sqe(ctx, req);
    if (!sqe) return (-1);

    io_uring_prep_fallocate(sqe, req->fd, req->meta.fallocate.mode, req->offset, (off_t)req->len);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_FALLOCATE);
    return (0);
}

int ring_submit_ftruncate(ring_ctx_t *ctx, aura_request_t *req) {
    struct io_uring_sqe *sqe = meta_get_sqe(ctx, req);
    if (!sqe) return (-1);

    io_uring_prep_ftruncate(sqe, req->fd, (loff_t)req->len);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_FTRUNCATE);
    return (0);
}

int ring_submit_sync_file_range(ring_ctx_t *ctx, aura_request_t *req) {
    if (req->len > UINT_MAX) {
        errno = EINVAL;
        return (-1);
    }
    struct io_uring_sqe *sqe = meta_get_sqe(ctx, req);
    if (!sqe) return (-1);

    io_uring_prep_sync_file_range(sqe, req->fd, (unsigned)req->len, req->offset,
                                  req->meta.sync_range.flags);
    sqe_apply_fixed_file(sqe, req);
    io_uring_sqe_set_data(sqe, req);
    meta_finish(ctx, req, AURA_OP_SYNC_FILE_RANGE);
    return (0);
}

/* ============================================================================
 * Flush and Completion Processing
 * ============================================================================ */

int ring_flush(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized || ctx->queued_sqes == 0) {
        return (0);
    }

    int submitted = io_uring_submit(&ctx->ring);
    if (submitted < 0) {
        errno = -submitted;
        return (-1);
    }

    /* Record submit for batch optimizer.
     * Use submitted (not queued_sqes) in case of partial submit. */
    adaptive_record_submit(&ctx->adaptive, submitted);
    ctx->queued_sqes -= submitted;

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
 */
static retire_entry_t process_completion(ring_ctx_t *ctx, aura_request_t *req, ssize_t result) {
    retire_entry_t retire = { .op_idx = -1, .result = result, .op_type = AURA_OP_CANCEL };

    if (!req) {
        return retire;
    }

    /* Save callback info and retirement data BEFORE any state changes.
     * The callback may submit new operations that reuse this request slot,
     * so we must capture everything we need before potential reuse. */
    retire.op_idx = req->op_idx;
    retire.op_type = req->op_type;
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
        adaptive_record_completion(&ctx->adaptive, latency_ns, bytes);
    }

    /* Mark as no longer pending just before callback invocation.
     * This ensures aura_request_pending() returns false only when
     * the callback is about to fire (not earlier). */
    atomic_store_explicit(&req->pending, false, memory_order_release);

    /* Invoke callback WITHOUT holding lock to prevent deadlock.
     * If the callback calls aura_read/write, it will try to acquire
     * the lock, which would deadlock with non-recursive mutexes.
     * The req pointer remains valid because ring_put_request hasn't
     * been called yet. */
    if (callback) {
        callback_context_depth++;
        callback((aura_request_t *)req, result, user_data);
        callback_context_depth--;
    }

    if (req->uses_registered_buffer) {
        atomic_fetch_sub_explicit(&ctx->fixed_buf_inflight, 1, memory_order_relaxed);
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

    ring_lock(ctx);
    for (int i = 0; i < count; i++) {
        if (entries[i].op_idx < 0) {
            continue; /* NULL req, skipped */
        }
        /* Don't count cancel operations in ops_completed — they are
         * internal bookkeeping, not user I/O operations. */
        if (entries[i].op_type != AURA_OP_CANCEL) {
            ctx->ops_completed++;
            if (entries[i].result > 0) {
                ctx->bytes_completed += entries[i].result;
            }
        }
        ring_put_request(ctx, entries[i].op_idx);
        pending_delta++;
    }
    /* Single atomic decrement for the entire batch instead of one per CQE. */
    atomic_fetch_sub_explicit(&ctx->pending_count, pending_delta, memory_order_relaxed);
    ring_unlock(ctx);
}

/** Max CQEs to extract per lock acquisition in ring_poll().
 *  Keeps lock hold time bounded while amortizing lock overhead. */
#define RING_POLL_BATCH 32

/**
 * Drain all available CQEs in batches.
 *
 * Shared by ring_poll, ring_wait (non-blocking), and ring_wait (after blocking).
 * Extracts CQEs under cq_lock, processes callbacks without locks, then retires
 * the batch under ring->lock.
 *
 * @param ctx Ring context
 * @return Number of completions processed
 */
static int ring_drain_cqes(ring_ctx_t *ctx) {
    struct {
        aura_request_t *req;
        ssize_t result;
    } batch[RING_POLL_BATCH];
    retire_entry_t retire[RING_POLL_BATCH];
    int completed = 0;

    while (1) {
        int n = 0;
        ring_cq_lock(ctx);
        while (n < RING_POLL_BATCH) {
            struct io_uring_cqe *cqe;
            if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
                break;
            }
            batch[n].req = io_uring_cqe_get_data(cqe);
            batch[n].result = cqe->res;
            io_uring_cqe_seen(&ctx->ring, cqe);
            TSAN_ACQUIRE(batch[n].req);
            n++;
        }
        ring_cq_unlock(ctx);

        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            retire[i] = process_completion(ctx, batch[i].req, batch[i].result);
        }
        ring_retire_batch(ctx, retire, n);
        completed += n;
    }

    return completed;
}

int ring_poll(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return (0);
    }

    return ring_drain_cqes(ctx);
}

int ring_wait(ring_ctx_t *ctx, int timeout_ms) {
    if (!ctx || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    if (atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) == 0) {
        return (0);
    }

    if (timeout_ms == 0) {
        return ring_drain_cqes(ctx);
    }

    /* Blocking wait: try non-blocking peek first, then block if needed. */
    struct io_uring_cqe *cqe;

    ring_cq_lock(ctx);
    if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
        /* Nothing available - release lock and do blocking wait.
         * We intentionally call io_uring_wait_cqe without holding
         * cq_lock.  This allows other threads to poll/peek CQEs
         * concurrently.  After the wait returns, we re-acquire
         * cq_lock and batch-peek, which handles the case where
         * another thread consumed the waking CQE first. */
        int ret;
        ring_cq_unlock(ctx);

        if (timeout_ms < 0) {
            ret = io_uring_wait_cqe(&ctx->ring, &cqe);
        } else {
            struct __kernel_timespec ts;
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000LL;
            ret = io_uring_wait_cqe_timeout(&ctx->ring, &cqe, &ts);
        }

        if (ret < 0) {
            if (ret == -ETIME || ret == -EAGAIN) {
                return (0);
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
        ring_cq_unlock(ctx);
    }

    return ring_drain_cqes(ctx);
}

/* ============================================================================
 * Adaptive Control Integration
 * ============================================================================ */

bool ring_can_submit(ring_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }

    int limit = adaptive_get_inflight_limit(&ctx->adaptive);
    return atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) < limit;
}

bool ring_should_flush(ring_ctx_t *ctx) {
    if (!ctx) {
        return false;
    }

    int threshold = adaptive_get_batch_threshold(&ctx->adaptive);
    return ctx->queued_sqes >= threshold;
}

int ring_get_fd(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return -1;
    }
    return ctx->ring.ring_fd;
}
