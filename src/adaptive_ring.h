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
#include <sys/uio.h>
#include <liburing.h>

#include "adaptive_engine.h"

/* Forward declarations */
typedef struct auraio_request auraio_request_t;
typedef void (*auraio_callback_t)(auraio_request_t *req, ssize_t result, void *user_data);

/**
 * Request operation type
 */
typedef enum {
    AURAIO_OP_READ,
    AURAIO_OP_WRITE,
    AURAIO_OP_READV,
    AURAIO_OP_WRITEV,
    AURAIO_OP_FSYNC,
    AURAIO_OP_FDATASYNC,
    AURAIO_OP_CANCEL,
    AURAIO_OP_READ_FIXED,   /**< Read using registered buffer */
    AURAIO_OP_WRITE_FIXED   /**< Write using registered buffer */
} auraio_op_type_t;

/**
 * Request context
 *
 * Tracks an in-flight I/O operation.
 */
struct auraio_request {
    /* Operation info */
    auraio_op_type_t op_type;       /**< Operation type */
    int fd;                         /**< File descriptor */
    off_t offset;                   /**< File offset */

    /* Buffer for simple read/write */
    void *buffer;                   /**< I/O buffer */
    size_t len;                     /**< I/O size */

    /* Fixed buffer + vectored I/O (packed to eliminate holes) */
    int buf_index;                  /**< Registered buffer index */
    int iovcnt;                     /**< Number of iovecs */
    size_t buf_offset;              /**< Offset within registered buffer */
    const struct iovec *iov;        /**< iovec array for readv/writev */

    /* Callback */
    auraio_callback_t callback;     /**< Completion callback */
    void *user_data;                /**< User data for callback */

    /* Internal tracking */
    int64_t submit_time_ns;         /**< Submission timestamp */
    int ring_idx;                   /**< Which ring owns this request */
    int op_idx;                     /**< Index in ring's request array */
    auraio_request_t *cancel_target; /**< Request to cancel (for AURAIO_OP_CANCEL) */

    /* State flags (trailing bools avoid mid-struct hole) */
    _Atomic bool pending;           /**< True if still in-flight */
    _Atomic bool cancel_requested;  /**< True if cancellation requested */
};

/**
 * Ring initialization options
 */
typedef struct {
    bool enable_sqpoll;             /**< Enable SQPOLL mode */
    int sqpoll_idle_ms;             /**< SQPOLL idle timeout (ms) */
} ring_options_t;

/**
 * Ring context
 *
 * One io_uring ring, typically pinned to a single CPU core.
 */
typedef struct {
    struct io_uring ring;           /**< io_uring instance */
    bool ring_initialized;          /**< Ring setup succeeded */
    bool sqpoll_enabled;            /**< SQPOLL mode active */
    int cpu_id;                     /**< CPU this ring is pinned to (-1 if not pinned) */
    int ring_idx;                   /**< Index of this ring in engine's array */

    /* Per-ring locks for thread-safe access */
    pthread_mutex_t lock;           /**< Protects ring submission and request pool */
    pthread_mutex_t cq_lock;        /**< Protects completion queue access */

    /* Request tracking */
    auraio_request_t *requests;     /**< Request array */
    int *free_request_stack;        /**< Free request indices */
    int free_request_count;         /**< Number of free request slots */
    int max_requests;               /**< Queue depth */
    _Atomic int pending_count;      /**< Number of in-flight ops (atomic for lock-free reads) */

    /* Adaptive controller */
    adaptive_controller_t adaptive; /**< AIMD controller */

    /* Statistics */
    int64_t bytes_submitted;        /**< Total bytes requested at submission */
    int64_t bytes_completed;        /**< Total bytes actually transferred (from CQE results) */
    int64_t ops_completed;          /**< Total ops completed */

    /* Batching state */
    int queued_sqes;                /**< SQEs queued but not submitted */

    /* Latency sampling: only timestamp every Nth submission to reduce
     * clock_gettime overhead. Non-sampled ops get submit_time_ns=0 and
     * are skipped in process_completion's adaptive recording. */
    int sample_counter;             /**< Submission counter for sampling */
} ring_ctx_t;

/** Sample 1 in N submissions for latency measurement.
 *  Must be power of 2. With N=8 and 10K IOPS, gives ~1250 samples/sec
 *  which is sufficient for P99 calculation (needs ~100 per 100ms window). */
#define RING_LATENCY_SAMPLE_RATE  8
#define RING_LATENCY_SAMPLE_MASK  (RING_LATENCY_SAMPLE_RATE - 1)

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
auraio_request_t *ring_get_request(ring_ctx_t *ctx, int *op_idx);

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
int ring_submit_read(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit a write operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_write(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit a vectored read operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_readv(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit a vectored write operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_writev(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit an fsync operation
 *
 * @param ctx Ring context
 * @param req Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_fsync(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit an fdatasync operation
 *
 * @param ctx   Ring context
 * @param req   Request to submit
 * @return 0 on success, -1 on error
 */
int ring_submit_fdatasync(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit a cancel operation
 *
 * @param ctx    Ring context
 * @param req    The cancel request itself
 * @param target The request to cancel
 * @return 0 on success, -1 on error
 */
int ring_submit_cancel(ring_ctx_t *ctx, auraio_request_t *req, auraio_request_t *target);

/**
 * Submit a read using a registered buffer
 *
 * @param ctx Ring context
 * @param req Request to submit (buf_index, buf_offset, len, offset must be set)
 * @return 0 on success, -1 on error
 */
int ring_submit_read_fixed(ring_ctx_t *ctx, auraio_request_t *req);

/**
 * Submit a write using a registered buffer
 *
 * @param ctx Ring context
 * @param req Request to submit (buf_index, buf_offset, len, offset must be set)
 * @return 0 on success, -1 on error
 */
int ring_submit_write_fixed(ring_ctx_t *ctx, auraio_request_t *req);

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

#endif /* ADAPTIVE_RING_H */
