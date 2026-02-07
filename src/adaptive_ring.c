/**
 * @file adaptive_ring.c
 * @brief io_uring ring management implementation
 *
 * Wraps io_uring with request tracking and adaptive control integration.
 */

#define _GNU_SOURCE
#include "adaptive_ring.h"
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

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

    /* Allocate request tracking */
    ctx->requests = calloc(queue_depth, sizeof(auraio_request_t));
    if (!ctx->requests) {
        goto cleanup_ring;
    }

    ctx->free_request_stack = malloc(queue_depth * sizeof(int));
    if (!ctx->free_request_stack) {
        goto cleanup_requests;
    }

    /* Initialize free stack (all slots available) */
    for (int i = 0; i < queue_depth; i++) {
        ctx->free_request_stack[i] = i;
        ctx->requests[i].op_idx = i;
        atomic_init(&ctx->requests[i].pending, false);
        atomic_init(&ctx->requests[i].cancel_requested, false);
    }
    ctx->free_request_count = queue_depth;

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

void ring_destroy(ring_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    /* Wait for pending operations to complete with a timeout.
     * Benign race: pending_count read without lock. Worst case is one extra
     * ring_wait() iteration, which is acceptable during shutdown.
     * Timeout after 10 seconds to avoid infinite hang on stuck ops
     * (e.g., hung NFS mount, stuck SCSI device). */
    int drain_attempts = 0;
    while (atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) > 0 &&
           drain_attempts < 100) {
        ring_wait(ctx, 100);
        drain_attempts++;
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

auraio_request_t *ring_get_request(ring_ctx_t *ctx, int *op_idx) {
    if (!ctx || ctx->free_request_count == 0) {
        return NULL;
    }

    int idx = ctx->free_request_stack[--ctx->free_request_count];
    auraio_request_t *req = &ctx->requests[idx];

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
    req->cancel_target = NULL;
    atomic_init(&req->pending, false);
    atomic_init(&req->cancel_requested, false);

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
        return;
    }

    ctx->free_request_stack[ctx->free_request_count++] = op_idx;
}

/* ============================================================================
 * Submission Operations
 * ============================================================================ */

int ring_submit_read(ring_ctx_t *ctx, auraio_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_read(sqe, req->fd, req->buffer, req->len, req->offset);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_READ;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

int ring_submit_write(ring_ctx_t *ctx, auraio_request_t *req) {
    if (!ctx || !req || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    io_uring_prep_write(sqe, req->fd, req->buffer, req->len, req->offset);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_WRITE;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

int ring_submit_readv(ring_ctx_t *ctx, auraio_request_t *req) {
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
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_READV;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += iovec_total_len(req->iov, req->iovcnt);

    return (0);
}

int ring_submit_writev(ring_ctx_t *ctx, auraio_request_t *req) {
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
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_WRITEV;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += iovec_total_len(req->iov, req->iovcnt);

    return (0);
}

int ring_submit_fsync(ring_ctx_t *ctx, auraio_request_t *req) {
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
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_FSYNC;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);

    return (0);
}

int ring_submit_fdatasync(ring_ctx_t *ctx, auraio_request_t *req) {
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
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_FDATASYNC;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);

    return (0);
}

int ring_submit_cancel(ring_ctx_t *ctx, auraio_request_t *req, auraio_request_t *target) {
    if (!ctx || !req || !target || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ctx->ring);
    if (!sqe) {
        errno = EBUSY;
        return (-1);
    }

    /* Mark target as cancel-requested */
    atomic_store_explicit(&target->cancel_requested, true, memory_order_release);

    /* io_uring cancel uses user_data to identify the request */
    io_uring_prep_cancel(sqe, target, 0);
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_CANCEL;
    req->cancel_target = target;
    req->submit_time_ns = 0; /* Cancel ops skip latency tracking */
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);

    return (0);
}

int ring_submit_read_fixed(ring_ctx_t *ctx, auraio_request_t *req) {
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
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_READ_FIXED;
    req->submit_time_ns =
        (ctx->sample_counter++ & RING_LATENCY_SAMPLE_MASK) == 0 ? get_time_ns() : 0;
    atomic_store_explicit(&req->pending, true, memory_order_release);
    TSAN_RELEASE(req);

    ctx->queued_sqes++;
    atomic_fetch_add_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    ctx->bytes_submitted += req->len;

    return (0);
}

int ring_submit_write_fixed(ring_ctx_t *ctx, auraio_request_t *req) {
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
    io_uring_sqe_set_data(sqe, req);

    req->op_type = AURAIO_OP_WRITE_FIXED;
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

    /* Record submit for batch optimizer */
    adaptive_record_submit(&ctx->adaptive, ctx->queued_sqes);
    ctx->queued_sqes = 0;

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
static void process_completion(ring_ctx_t *ctx, auraio_request_t *req, ssize_t result) {
    if (!req) {
        return;
    }

    /* Save callback info BEFORE any state changes.
     * The callback may submit new operations that reuse this request slot,
     * so we must capture everything we need before potential reuse. */
    int op_idx = req->op_idx;
    auraio_callback_t callback = req->callback;
    void *user_data = req->user_data;

    /* Mark as no longer pending */
    atomic_store_explicit(&req->pending, false, memory_order_release);

    /* Record completion for adaptive controller.
     * Skip cancel ops and non-sampled ops (submit_time_ns == 0). */
    if (req->submit_time_ns != 0) {
        int64_t now_ns = get_time_ns();
        int64_t latency_ns = now_ns - req->submit_time_ns;
        size_t bytes = (result > 0) ? (size_t)result : 0;
        adaptive_record_completion(&ctx->adaptive, latency_ns, bytes);
    }

    /* Invoke callback WITHOUT holding lock to prevent deadlock.
     * If the callback calls auraio_read/write, it will try to acquire
     * the lock, which would deadlock with non-recursive mutexes.
     * The req pointer remains valid because ring_put_request hasn't
     * been called yet. pending is already false (atomic release above). */
    if (callback) {
        callback((auraio_request_t *)req, result, user_data);
    }

    /* After callback: update counters and return slot in one lock region.
     * pending_count is momentarily inflated by 1 during callback execution,
     * which may cause a spurious EAGAIN if the ring is exactly at capacity.
     * This is acceptable: the caller handles EAGAIN gracefully, and the
     * count is corrected immediately after callback returns. */
    pthread_mutex_lock(&ctx->lock);
    ctx->ops_completed++;
    atomic_fetch_sub_explicit(&ctx->pending_count, 1, memory_order_relaxed);
    if (result > 0) {
        ctx->bytes_completed += result;
    }
    ring_put_request(ctx, op_idx);
    pthread_mutex_unlock(&ctx->lock);
}

/** Max CQEs to extract per lock acquisition in ring_poll().
 *  Keeps lock hold time bounded while amortizing lock overhead. */
#define RING_POLL_BATCH 32

int ring_poll(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return (0);
    }

    int completed = 0;

    /* Batch-extract CQEs under a single lock hold, then process
     * completions outside the lock. This amortizes mutex overhead
     * from O(completions) to O(completions / RING_POLL_BATCH).
     * Callbacks are invoked outside the lock to prevent deadlock
     * (callbacks may re-enter auraio_read/write). */
    struct {
        auraio_request_t *req;
        ssize_t result;
    } batch[RING_POLL_BATCH];

    while (1) {
        int n = 0;

        pthread_mutex_lock(&ctx->cq_lock);
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
        pthread_mutex_unlock(&ctx->cq_lock);

        if (n == 0) {
            break;
        }

        /* Process completions without holding cq_lock */
        for (int i = 0; i < n; i++) {
            process_completion(ctx, batch[i].req, batch[i].result);
        }
        completed += n;
    }

    return completed;
}

int ring_wait(ring_ctx_t *ctx, int timeout_ms) {
    if (!ctx || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    if (atomic_load_explicit(&ctx->pending_count, memory_order_relaxed) == 0) {
        return (0);
    }

    /* Batch buffer shared by all paths. Both the initial peek/wait and
     * any additional available CQEs are extracted in a single lock hold,
     * eliminating the previous separate drain loop. */
    struct {
        auraio_request_t *req;
        ssize_t result;
    } batch[RING_POLL_BATCH];
    int completed = 0;

    if (timeout_ms == 0) {
        /* Non-blocking: batch-extract all available CQEs in one lock hold */
        int n = 0;
        pthread_mutex_lock(&ctx->cq_lock);
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
        pthread_mutex_unlock(&ctx->cq_lock);

        for (int i = 0; i < n; i++) {
            process_completion(ctx, batch[i].req, batch[i].result);
        }
        completed = n;
    } else {
        /* Blocking wait: try non-blocking peek first, then block if needed.
         * In both cases, batch-extract all available CQEs under a single
         * cq_lock hold to merge the initial extraction with the drain. */
        struct io_uring_cqe *cqe;
        int n;

        pthread_mutex_lock(&ctx->cq_lock);
        if (io_uring_peek_cqe(&ctx->ring, &cqe) == 0) {
            /* CQE available immediately - batch extract all available */
            batch[0].req = io_uring_cqe_get_data(cqe);
            batch[0].result = cqe->res;
            io_uring_cqe_seen(&ctx->ring, cqe);
            TSAN_ACQUIRE(batch[0].req);
            n = 1;
            while (n < RING_POLL_BATCH) {
                if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
                    break;
                }
                batch[n].req = io_uring_cqe_get_data(cqe);
                batch[n].result = cqe->res;
                io_uring_cqe_seen(&ctx->ring, cqe);
                n++;
            }
            pthread_mutex_unlock(&ctx->cq_lock);

            for (int i = 0; i < n; i++) {
                process_completion(ctx, batch[i].req, batch[i].result);
            }
            completed = n;
        } else {
            /* Nothing available - release lock and do blocking wait */
            int ret;
            pthread_mutex_unlock(&ctx->cq_lock);

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
                errno = -ret;
                return (-1);
            }

            /* Wait returned - batch extract all available CQEs.
             * Another thread might have consumed the CQE that woke us;
             * n==0 is handled gracefully. */
            n = 0;
            pthread_mutex_lock(&ctx->cq_lock);
            while (n < RING_POLL_BATCH) {
                if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
                    break;
                }
                batch[n].req = io_uring_cqe_get_data(cqe);
                batch[n].result = cqe->res;
                io_uring_cqe_seen(&ctx->ring, cqe);
                n++;
            }
            pthread_mutex_unlock(&ctx->cq_lock);

            for (int i = 0; i < n; i++) {
                process_completion(ctx, batch[i].req, batch[i].result);
            }
            completed = n;
        }
    }

    return completed;
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
