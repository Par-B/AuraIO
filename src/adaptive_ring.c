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

    /* Initialize io_uring with optional SQPOLL */
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));

    bool try_sqpoll = options && options->enable_sqpoll;
    if (try_sqpoll) {
        params.flags |= IORING_SETUP_SQPOLL;
        if (options->sqpoll_idle_ms > 0) {
            params.sq_thread_idle = options->sqpoll_idle_ms;
        } else {
            params.sq_thread_idle = 1000;  /* Default 1 second */
        }
    }

    int ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);

    /* If SQPOLL failed (usually permission), retry without it */
    if (ret < 0 && try_sqpoll) {
        memset(&params, 0, sizeof(params));
        ret = io_uring_queue_init_params(queue_depth, &ctx->ring, &params);
        ctx->sqpoll_enabled = false;
    } else if (ret >= 0 && try_sqpoll) {
        ctx->sqpoll_enabled = true;
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

    /* Wait for pending operations to complete.
     * Benign race: pending_count read without lock. Worst case is one extra
     * ring_wait() iteration, which is acceptable during shutdown. */
    while (ctx->pending_count > 0) {
        ring_wait(ctx, 100);
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

    /* Clear previous state but preserve op_idx.
     * memset(0) is valid for _Atomic bool = false per C11. */
    int saved_idx = req->op_idx;
    memset(req, 0, sizeof(*req));
    req->op_idx = saved_idx;

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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;
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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;
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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;
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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;
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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;

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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;

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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;

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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;
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
    req->submit_time_ns = get_time_ns();
    atomic_store_explicit(&req->pending, true, memory_order_release);

    ctx->queued_sqes++;
    ctx->pending_count++;
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

    int64_t now_ns = get_time_ns();
    int64_t latency_ns = now_ns - req->submit_time_ns;

    /* Save callback info BEFORE any state changes.
     * The callback may submit new operations that reuse this request slot,
     * so we must capture everything we need before potential reuse. */
    int op_idx = req->op_idx;
    auraio_callback_t callback = req->callback;
    void *user_data = req->user_data;

    /* Mark as no longer pending */
    atomic_store_explicit(&req->pending, false, memory_order_release);

    /* Record completion for adaptive controller (skip cancel ops) */
    if (req->op_type != AURAIO_OP_CANCEL) {
        size_t bytes = (result > 0) ? (size_t)result : 0;
        adaptive_record_completion(&ctx->adaptive, latency_ns, bytes);
    }

    /* Update counters under lock */
    pthread_mutex_lock(&ctx->lock);
    ctx->ops_completed++;
    ctx->pending_count--;
    pthread_mutex_unlock(&ctx->lock);

    /* Invoke callback WITHOUT holding lock to prevent deadlock.
     * If the callback calls auraio_read/write, it will try to acquire
     * the lock, which would deadlock with non-recursive mutexes. */
    if (callback) {
        callback((auraio_request_t *)req, result, user_data);
    }

    /* Return request slot AFTER callback completes under lock.
     * This ensures the req pointer passed to callback remains valid
     * throughout callback execution, preventing use-after-free if
     * another thread acquires and reuses the slot. */
    pthread_mutex_lock(&ctx->lock);
    ring_put_request(ctx, op_idx);
    pthread_mutex_unlock(&ctx->lock);
}

int ring_poll(ring_ctx_t *ctx) {
    if (!ctx || !ctx->ring_initialized) {
        return (0);
    }

    int completed = 0;

    /* Process all available completions without blocking.
     * CQ access is serialized via cq_lock to prevent races when multiple
     * threads call poll/wait on the same ring. */
    while (1) {
        struct io_uring_cqe *cqe;
        auraio_request_t *req;
        ssize_t result;

        /* Lock CQ, peek, extract data, mark seen, unlock */
        pthread_mutex_lock(&ctx->cq_lock);
        if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
            pthread_mutex_unlock(&ctx->cq_lock);
            break;
        }
        req = io_uring_cqe_get_data(cqe);
        result = cqe->res;
        io_uring_cqe_seen(&ctx->ring, cqe);
        pthread_mutex_unlock(&ctx->cq_lock);

        /* Process completion without holding cq_lock */
        process_completion(ctx, req, result);
        completed++;
    }

    return completed;
}

int ring_wait(ring_ctx_t *ctx, int timeout_ms) {
    if (!ctx || !ctx->ring_initialized) {
        errno = EINVAL;
        return (-1);
    }

    if (ctx->pending_count == 0) {
        return (0);
    }

    int completed = 0;
    struct io_uring_cqe *cqe;
    auraio_request_t *req;
    ssize_t result;

    /* For blocking waits, we first try a non-blocking peek under lock.
     * If nothing available, we release the lock, do a blocking wait,
     * then re-acquire and peek again. This ensures CQ access is serialized
     * while still allowing the blocking wait to proceed. */

    if (timeout_ms == 0) {
        /* Non-blocking: just peek under lock */
        pthread_mutex_lock(&ctx->cq_lock);
        if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
            pthread_mutex_unlock(&ctx->cq_lock);
            return (0);
        }
        req = io_uring_cqe_get_data(cqe);
        result = cqe->res;
        io_uring_cqe_seen(&ctx->ring, cqe);
        pthread_mutex_unlock(&ctx->cq_lock);

        process_completion(ctx, req, result);
        completed = 1;
    } else {
        /* Blocking wait: try peek first, then wait if needed */
        pthread_mutex_lock(&ctx->cq_lock);
        int ret = io_uring_peek_cqe(&ctx->ring, &cqe);

        if (ret == 0) {
            /* Got one immediately */
            req = io_uring_cqe_get_data(cqe);
            result = cqe->res;
            io_uring_cqe_seen(&ctx->ring, cqe);
            pthread_mutex_unlock(&ctx->cq_lock);
            process_completion(ctx, req, result);
            completed = 1;
        } else {
            /* Nothing available - release lock and wait */
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
                    return (0);  /* Timeout - not an error */
                }
                errno = -ret;
                return (-1);
            }

            /* Wait returned - re-acquire lock and peek to get CQE properly.
             * Another thread might have consumed it, which is fine. */
            pthread_mutex_lock(&ctx->cq_lock);
            if (io_uring_peek_cqe(&ctx->ring, &cqe) == 0) {
                req = io_uring_cqe_get_data(cqe);
                result = cqe->res;
                io_uring_cqe_seen(&ctx->ring, cqe);
                pthread_mutex_unlock(&ctx->cq_lock);
                process_completion(ctx, req, result);
                completed = 1;
            } else {
                /* Another thread got it - that's fine */
                pthread_mutex_unlock(&ctx->cq_lock);
            }
        }
    }

    /* Process any additional available completions */
    while (1) {
        pthread_mutex_lock(&ctx->cq_lock);
        if (io_uring_peek_cqe(&ctx->ring, &cqe) != 0) {
            pthread_mutex_unlock(&ctx->cq_lock);
            break;
        }
        req = io_uring_cqe_get_data(cqe);
        result = cqe->res;
        io_uring_cqe_seen(&ctx->ring, cqe);
        pthread_mutex_unlock(&ctx->cq_lock);

        process_completion(ctx, req, result);
        completed++;
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
    return ctx->pending_count < limit;
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
