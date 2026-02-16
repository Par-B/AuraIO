/**
 * @file adaptive_ring.h
 * @brief io_uring ring management
 *
 * Internal header - not part of public API.
 */

#ifndef ADAPTIVE_RING_H
#define ADAPTIVE_RING_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <liburing.h>

#include "adaptive_engine.h"
#include "aura.h" /* aura_op_type_t, aura_request_t, aura_callback_t */

/**
 * Request context
 *
 * Tracks an in-flight I/O operation.
 */
struct aura_request {
    /* Operation info */
    aura_op_type_t op_type; /**< Operation type */
    int fd;                 /**< File descriptor */
    off_t offset;           /**< File offset */

    /* Buffer for simple read/write */
    void *buffer; /**< I/O buffer */
    size_t len;   /**< I/O size */

    /* Fixed buffer + vectored I/O (packed to eliminate holes) */
    int buf_index;           /**< Registered buffer index */
    int iovcnt;              /**< Number of iovecs */
    size_t buf_offset;       /**< Offset within registered buffer */
    const struct iovec *iov; /**< iovec array for readv/writev */

    /* Callback */
    aura_callback_t callback; /**< Completion callback */
    void *user_data;          /**< User data for callback */

    /* Internal tracking */
    int64_t submit_time_ns;      /**< Submission timestamp */
    int ring_idx;                /**< Which ring owns this request */
    int op_idx;                  /**< Index in ring's request array */
    bool uses_registered_buffer; /**< True if op depends on registered buffer lifetime */
    bool uses_registered_file;   /**< True if fd is a registered-file index */

    /* State flags (trailing bools avoid mid-struct hole) */
    _Atomic bool pending; /**< True if still in-flight */

    /* Original file descriptor (for aura_request_fd() API contract).
     * When uses_registered_file is true, fd holds the fixed-file index
     * and original_fd holds the actual file descriptor. */
    int original_fd;

    /* Metadata operation parameters (union to avoid bloating the struct).
     * close uses fd; ftruncate uses len; others use op-specific fields. */
    union {
        struct {
            const char *pathname;
            int flags;
            mode_t mode;
        } open;
        struct {
            const char *pathname;
            struct statx *buf;
            int flags;
            unsigned mask;
        } statx;
        struct {
            int mode;
        } fallocate;
        struct {
            unsigned flags;
        } sync_range;
    } meta;

    /* Pad to 128 bytes (2 full cache lines) so that adjacent requests in
     * the request array never share a cache line. */
    char _cacheline_pad[128 - 124];
};

/**
 * Ring initialization options
 */
typedef struct {
    bool enable_sqpoll; /**< Enable SQPOLL mode */
    int sqpoll_idle_ms; /**< SQPOLL idle timeout (ms) */
} ring_options_t;

/**
 * Ring context
 *
 * One io_uring ring, typically pinned to a single CPU core.
 */
typedef struct {
    struct io_uring ring;  /**< io_uring instance */
    bool ring_initialized; /**< Ring setup succeeded */
    bool sqpoll_enabled;   /**< SQPOLL mode active */
    int cpu_id;            /**< CPU this ring is pinned to (-1 if not pinned) */
    int ring_idx;          /**< Index of this ring in engine's array */

    /* Per-ring locks for thread-safe access */
    pthread_mutex_t lock;    /**< Protects ring submission and request pool */
    pthread_mutex_t cq_lock; /**< Protects completion queue access */

    /* Request tracking */
    aura_request_t *requests;  /**< Request array */
    int *free_request_stack;   /**< Free request indices */
    int free_request_count;    /**< Number of free request slots */
    int max_requests;          /**< Queue depth */
    _Atomic int pending_count; /**< Number of in-flight ops (atomic for lock-free reads) */
    _Atomic int
        peak_pending_count; /**< High-water mark of pending_count (updated by tick thread) */

    /* Adaptive controller */
    adaptive_controller_t adaptive; /**< AIMD controller */

    /* Submit-path counters (written during ring_submit_*).
     * Separated from completion-path counters to avoid false sharing
     * between the submit and completion code paths. */
    _Alignas(64) _Atomic int queued_sqes; /**< SQEs queued but not submitted */
    int sample_counter;                   /**< Submission counter for sampling */
    int64_t bytes_submitted;              /**< Total bytes requested at submission */

    /* Completion-path counters (written during process_completion) */
    _Alignas(64) int64_t
        bytes_completed;                 /**< Total bytes actually transferred (from CQE results) */
    int64_t ops_completed;               /**< Total ops completed */
    _Atomic uint32_t fixed_buf_inflight; /**< Registered-buffer ops currently in-flight */

    /* Single-thread fast path: skip mutexes when caller guarantees
     * single-thread access. Set during init, const after. */
    bool single_thread;
} ring_ctx_t;

/**
 * Check whether current thread is executing an AuraIO completion callback.
 *
 * Used to avoid blocking operations from inside callbacks.
 *
 * @return true if currently in callback context, false otherwise
 */
bool ring_in_callback_context(void);

/** Sample 1 in N submissions for latency measurement.
 *  Must be power of 2. With N=8 and 10K IOPS, gives ~1250 samples/sec
 *  which is sufficient for P99 calculation (needs ~100 per 100ms window). */
#define RING_LATENCY_SAMPLE_RATE 8
#define RING_LATENCY_SAMPLE_MASK (RING_LATENCY_SAMPLE_RATE - 1)

/**
 * Initialize ring context
 *
 * Creates io_uring ring and allocates request tracking structures.
 *
 * @param ctx         Ring context to initialize
 * @param queue_depth Number of entries in the ring
 * @param cpu_id      CPU to pin to (-1 to disable pinning)
 * @param options     Ring options (may be NULL for defaults)
 * @return 0 on success, -1 on error
 */
int ring_init(ring_ctx_t *ctx, int queue_depth, int cpu_id, const ring_options_t *options);

/**
 * Destroy ring context
 *
 * Waits for pending ops and frees resources.
 *
 * @param ctx Ring context to destroy
 */
void ring_destroy(ring_ctx_t *ctx);

/**
 * Get a free request slot
 *
 * @param ctx    Ring context
 * @param op_idx Output: index of allocated slot
 * @return Request pointer, or NULL if none available
 */
aura_request_t *ring_get_request(ring_ctx_t *ctx, int *op_idx);

/**
 * Return a request slot
 *
 * @param ctx    Ring context
 * @param op_idx Index of slot to return
 */
void ring_put_request(ring_ctx_t *ctx, int op_idx);

/**
 * Submit a read operation
 *
 * Queues the operation in the submission ring.
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_read(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit a write operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_write(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit a vectored read operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_readv(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit a vectored write operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_writev(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit an fsync operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_fsync(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit an fdatasync operation
 *
 * @param ctx   Ring context
 * @param req   Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_fdatasync(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit a cancel operation
 *
 * @param ctx    Ring context
 * @param req    The cancel request itself
 * @param target The request to cancel
 * @return 0 on success, -1 on error
 */
int ring_submit_cancel(ring_ctx_t *ctx, aura_request_t *req, aura_request_t *target);

/**
 * Submit a read using a registered buffer
 *
 * @param ctx Ring context
 * @param req Request to submit (buf_index, buf_offset, len, offset must be set)
 * @return 0 on success, -1 on error
 */
int ring_submit_read_fixed(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Submit a write using a registered buffer
 *
 * @param ctx Ring context
 * @param req Request to submit (buf_index, buf_offset, len, offset must be set)
 * @return 0 on success, -1 on error
 */
int ring_submit_write_fixed(ring_ctx_t *ctx, aura_request_t *req);

/* Lifecycle metadata operations (skip AIMD sampling) */
int ring_submit_openat(ring_ctx_t *ctx, aura_request_t *req);
int ring_submit_close(ring_ctx_t *ctx, aura_request_t *req);
int ring_submit_statx(ring_ctx_t *ctx, aura_request_t *req);
int ring_submit_fallocate(ring_ctx_t *ctx, aura_request_t *req);
int ring_submit_ftruncate(ring_ctx_t *ctx, aura_request_t *req);
int ring_submit_sync_file_range(ring_ctx_t *ctx, aura_request_t *req);

/**
 * Flush queued submissions to kernel
 *
 * Submits all queued SQEs.
 *
 * @param ctx Ring context
 * @return Number of SQEs submitted, or -1 on error
 */
int ring_flush(ring_ctx_t *ctx);

/**
 * Process completions (non-blocking)
 *
 * Harvests CQEs and invokes callbacks.
 *
 * @param ctx Ring context
 * @return Number of completions processed
 */
int ring_poll(ring_ctx_t *ctx);

/**
 * Wait for completions
 *
 * Blocks until at least one completion or timeout.
 *
 * @param ctx        Ring context
 * @param timeout_ms Maximum wait time (-1 = forever)
 * @return Number of completions processed, or -1 on error
 */
int ring_wait(ring_ctx_t *ctx, int timeout_ms);

/**
 * Check if ring should accept more submissions
 *
 * Based on adaptive controller's current in-flight limit.
 *
 * @param ctx Ring context
 * @return true if can submit, false if at limit
 */
bool ring_can_submit(ring_ctx_t *ctx);

/**
 * Check if ring should flush submissions
 *
 * Based on adaptive controller's batch threshold.
 *
 * @param ctx Ring context
 * @return true if should flush, false otherwise
 */
bool ring_should_flush(ring_ctx_t *ctx);

/**
 * Get the io_uring fd for polling
 *
 * @param ctx Ring context
 * @return Ring fd, or -1 if not initialized
 */
int ring_get_fd(ring_ctx_t *ctx);

/* ============================================================================
 * Conditional Locking (inline for hot-path use in adaptive_ring.c + aura.c)
 *
 * When single_thread is set, all mutex ops are skipped. The flag is const
 * after init, so branch prediction eliminates overhead after warmup.
 * ============================================================================ */

static inline void ring_lock(ring_ctx_t *ctx) {
    if (!ctx->single_thread) pthread_mutex_lock(&ctx->lock);
}
static inline void ring_unlock(ring_ctx_t *ctx) {
    if (!ctx->single_thread) pthread_mutex_unlock(&ctx->lock);
}
static inline void ring_cq_lock(ring_ctx_t *ctx) {
    if (!ctx->single_thread) pthread_mutex_lock(&ctx->cq_lock);
}
static inline void ring_cq_unlock(ring_ctx_t *ctx) {
    if (!ctx->single_thread) pthread_mutex_unlock(&ctx->cq_lock);
}

#endif /* ADAPTIVE_RING_H */
