/**
 * @file auraio.h
 * @brief Self-tuning async I/O library built on io_uring
 *
 * A high-performance async I/O library that automatically tunes itself
 * for optimal throughput and latency using AIMD congestion control.
 *
 * Basic usage:
 * @code
 *   auraio_engine_t *engine = auraio_create();
 *   void *buf = auraio_buffer_alloc(engine, size);
 *   auraio_request_t *req = auraio_read(engine, fd, auraio_buf(buf), size, 0,
 * callback, user_data); auraio_wait(engine, -1); auraio_buffer_free(engine,
 * buf, size); auraio_destroy(engine);
 * @endcode
 *
 * Event loop integration:
 * @code
 *   int poll_fd = auraio_get_poll_fd(engine);
 *   // Add poll_fd to your epoll/kqueue/select
 *   // When readable, call auraio_poll(engine)
 * @endcode
 */

#ifndef AURAIO_H
#define AURAIO_H

#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * Version Information
 * ============================================================================
 */

#define AURAIO_VERSION_MAJOR 0
#define AURAIO_VERSION_MINOR 2
#define AURAIO_VERSION_PATCH 0

/** Version as a single integer: (major * 10000 + minor * 100 + patch) */
#define AURAIO_VERSION                                                                             \
    (AURAIO_VERSION_MAJOR * 10000 + AURAIO_VERSION_MINOR * 100 + AURAIO_VERSION_PATCH)

/** Version as a string */
#define AURAIO_VERSION_STRING "0.2.0"

/* Ensure version components stay within packed integer limits */
#if AURAIO_VERSION_MINOR > 99 || AURAIO_VERSION_PATCH > 99
#    error "Version minor/patch must be 0-99 for packed AURAIO_VERSION integer"
#endif

/* ============================================================================
 * Symbol Visibility
 * ============================================================================
 *
 * AURAIO_API marks functions exported from the shared library.
 * Build with -fvisibility=hidden to hide internal symbols.
 */

#if defined(AURAIO_SHARED_BUILD)
#    define AURAIO_API __attribute__((visibility("default")))
#elif defined(AURAIO_STATIC_BUILD)
#    define AURAIO_API
#else
#    define AURAIO_API
#endif

/**
 * Warn if return value is ignored
 *
 * Applied to functions that return error codes or allocated resources.
 * Ignoring these return values is almost always a bug.
 */
#if defined(__GNUC__) || defined(__clang__)
#    define AURAIO_WARN_UNUSED __attribute__((warn_unused_result))
#else
#    define AURAIO_WARN_UNUSED
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/uio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Opaque engine handle
 *
 * Created by auraio_create(), destroyed by auraio_destroy().
 * Thread-safe for submissions from multiple threads.
 */
typedef struct auraio_engine auraio_engine_t;

/**
 * Opaque request handle
 *
 * Returned by submission functions. Can be used to cancel in-flight operations
 * or query request state while in-flight.
 *
 * LIFETIME: The request handle is valid from submission until the completion
 * callback BEGINS execution. Once the callback starts, the handle becomes
 * invalid and must not be used. This applies whether the operation completed
 * normally, with an error, or was cancelled (callback receives -ECANCELED).
 *
 * THREAD SAFETY: It is safe to call auraio_cancel() or auraio_request_pending()
 * on a request handle from any thread, as long as the callback has not yet
 * started. If auraio_request_pending() returns false, the handle is about to
 * become invalid.
 */
typedef struct auraio_request auraio_request_t;

/**
 * Completion callback type
 *
 * Called when an async operation completes.
 *
 * @param req       Request handle (valid only during callback)
 * @param result    Bytes transferred on success, negative errno on failure
 *                  -ECANCELED if the operation was cancelled
 * @param user_data User pointer passed to auraio_read/write/fsync
 *
 * NULL CALLBACK: Passing NULL as the callback is permitted. The I/O operation
 * will execute normally, but there is no notification of completion and no way
 * to retrieve the result. This is useful for fire-and-forget operations (e.g.,
 * advisory writes where success is not critical). The request handle returned
 * by the submission function can still be used with auraio_request_pending()
 * and auraio_cancel().
 *
 * RESTRICTIONS:
 * - The callback executes in the context of auraio_poll() or auraio_wait()
 * - Callbacks may submit new I/O operations (auraio_read, auraio_write, etc.)
 * - Callbacks MUST NOT call auraio_destroy() - this will cause undefined
 * behavior
 * - Callbacks should complete quickly to avoid blocking other completions
 * - The request handle (req) becomes invalid after the callback returns
 */
typedef void (*auraio_callback_t)(auraio_request_t *req, ssize_t result, void *user_data);

/**
 * Engine statistics
 *
 * Retrieved via auraio_get_stats() for monitoring and debugging.
 */
typedef struct {
    int64_t ops_completed;         /**< Total operations completed */
    int64_t bytes_transferred;     /**< Total bytes read/written */
    double current_throughput_bps; /**< Current throughput (bytes/sec) */
    double p99_latency_ms;         /**< 99th percentile latency (ms) */
    int current_in_flight;         /**< Current in-flight operations */
    int optimal_in_flight;         /**< Tuned optimal in-flight limit */
    int optimal_batch_size;        /**< Tuned optimal batch size */
    uint64_t adaptive_spills;      /**< ADAPTIVE mode: times a submission spilled to
                                      another ring */
    uint32_t _reserved[4];         /**< Reserved for future use; must be zero */
} auraio_stats_t;

/**
 * Per-ring statistics
 *
 * Provides detailed per-ring metrics including AIMD controller state.
 * Retrieved via auraio_get_ring_stats().
 */
typedef struct {
    int64_t ops_completed;     /**< Total operations completed on this ring */
    int64_t bytes_transferred; /**< Total bytes transferred through this ring */
    int pending_count;         /**< Current in-flight operations */
    int in_flight_limit;       /**< Current AIMD-tuned in-flight limit */
    int batch_threshold;       /**< Current AIMD-tuned batch threshold */
    double p99_latency_ms;     /**< Current P99 latency for this ring (ms) */
    double throughput_bps;     /**< Current throughput for this ring (bytes/sec) */
    int aimd_phase;            /**< Current AIMD phase (see AURAIO_PHASE_* constants) */
    int queue_depth;           /**< Maximum queue depth */
    uint32_t _reserved[4];     /**< Reserved for future use; must be zero */
} auraio_ring_stats_t;

/** AIMD controller phase constants for auraio_ring_stats_t.aimd_phase */
#define AURAIO_PHASE_BASELINE 0  /**< Collecting baseline latency */
#define AURAIO_PHASE_PROBING 1   /**< Increasing in-flight limit */
#define AURAIO_PHASE_STEADY 2    /**< Maintaining optimal config */
#define AURAIO_PHASE_BACKOFF 3   /**< Reducing due to latency spike */
#define AURAIO_PHASE_SETTLING 4  /**< Waiting for metrics to stabilize */
#define AURAIO_PHASE_CONVERGED 5 /**< Tuning complete */

/**
 * Latency histogram snapshot
 *
 * An approximate snapshot of the active latency histogram for a ring.
 * Tracks latencies from 0 to max_tracked_us in bucket_width_us increments.
 * Operations exceeding max_tracked_us are counted in overflow.
 *
 * Because the snapshot is read from a concurrently-written histogram,
 * individual bucket values are atomic but the overall snapshot may not
 * be perfectly consistent (e.g., total_count may differ slightly from
 * the sum of all buckets + overflow).  For monitoring purposes this is
 * negligible.
 *
 * When adaptive tuning is disabled (disable_adaptive = true), the
 * histogram is not periodically reset and accumulates data indefinitely.
 *
 * Retrieved via auraio_get_histogram().
 */
#define AURAIO_HISTOGRAM_BUCKETS 200
#define AURAIO_HISTOGRAM_BUCKET_WIDTH_US 50

typedef struct {
    uint32_t buckets[AURAIO_HISTOGRAM_BUCKETS]; /**< Latency frequency buckets */
    uint32_t overflow;                          /**< Count of operations exceeding max_tracked_us */
    uint32_t total_count;                       /**< Total samples in this snapshot */
    int bucket_width_us;                        /**< Width of each bucket in microseconds */
    int max_tracked_us;                         /**< Maximum tracked latency in microseconds */
    uint32_t _reserved[2];                      /**< Reserved for future use; must be zero */
} auraio_histogram_t;

/**
 * Buffer pool statistics
 *
 * Retrieved via auraio_get_buffer_stats().
 */
typedef struct {
    size_t total_allocated_bytes; /**< Total bytes currently allocated from pool */
    size_t total_buffers;         /**< Total buffer count currently allocated */
    int shard_count;              /**< Number of pool shards */
    uint32_t _reserved[4];        /**< Reserved for future use; must be zero */
} auraio_buffer_stats_t;

/**
 * Ring selection mode
 *
 * Controls how submissions are distributed across io_uring rings.
 */
typedef enum {
    AURAIO_SELECT_ADAPTIVE = 0, /**< CPU-local with power-of-two spilling (default).
                                     Stays on the CPU-local ring when uncongested.
                                     When local ring exceeds 75% of its in-flight limit
                                     and load is within 2x of the global average (broad
                                     pressure), spills to the lighter of two randomly
                                     chosen non-local rings. */
    AURAIO_SELECT_CPU_LOCAL,    /**< CPU-affinity only. Each thread submits to the
                                     ring matching its current CPU (sched_getcpu).
                                     Best cache locality and NUMA friendliness.
                                     Single-threaded workloads use only one ring. */
    AURAIO_SELECT_ROUND_ROBIN   /**< Atomic round-robin across all rings. Maximum
                                     single-thread scaling. Best for benchmarks or
                                     single-thread event loops. */
} auraio_ring_select_t;

/**
 * Engine configuration options
 *
 * Used with auraio_create_with_options() for custom configuration.
 * Initialize with auraio_options_init() before modifying.
 */
typedef struct {
    size_t struct_size;        /**< Set by auraio_options_init(); for ABI
                                  forward-compatibility */
    int queue_depth;           /**< Queue depth per ring (0 = default: 256) */
    int ring_count;            /**< Number of rings, 0 = auto (one per CPU) */
    int initial_in_flight;     /**< Initial in-flight limit (default: queue_depth/4)
                                */
    int min_in_flight;         /**< Minimum in-flight limit (default: 4) */
    double max_p99_latency_ms; /**< Target max P99 latency, 0 = auto */
    size_t buffer_alignment;   /**< Buffer alignment (default: system page size) */
    bool disable_adaptive;     /**< Disable adaptive tuning */

    /* Advanced io_uring features */
    bool enable_sqpoll; /**< Enable SQPOLL mode (requires root/CAP_SYS_NICE) */
    int sqpoll_idle_ms; /**< SQPOLL idle timeout in ms (default: 1000) */

    /* Ring selection */
    auraio_ring_select_t ring_select; /**< Ring selection mode (default: ADAPTIVE) */

    uint32_t _reserved[8]; /**< Reserved for future use; must be zero */
} auraio_options_t;

/**
 * Fsync flags for auraio_fsync()
 *
 * Controls the type of flush operation:
 *   AURAIO_FSYNC_DEFAULT  (0) -- full fsync: flushes data + metadata
 *   AURAIO_FSYNC_DATASYNC (1) -- fdatasync: flushes data, skips metadata
 *                                if it is not needed for data integrity
 */
typedef enum {
    AURAIO_FSYNC_DEFAULT = 0,  /**< Full fsync (metadata + data) */
    AURAIO_FSYNC_DATASYNC = 1, /**< fdatasync (data only, skip metadata if possible) */
} auraio_fsync_flags_t;

/* ============================================================================
 * Buffer Descriptors (Unified Buffer API)
 * ============================================================================
 */

/**
 * Buffer type indicator
 */
typedef enum {
    AURAIO_BUF_UNREGISTERED = 0, /**< Regular user-provided buffer */
    AURAIO_BUF_REGISTERED = 1    /**< Pre-registered buffer (zero-copy) */
} auraio_buf_type_t;

/**
 * Unified buffer descriptor
 *
 * A small value type that can represent either a regular buffer pointer
 * or a registered buffer reference. Pass by value to avoid heap allocation.
 *
 * Create using helper functions:
 *   auraio_buf(ptr)              - regular buffer
 *   auraio_buf_fixed(idx, off)   - registered buffer with offset
 *   auraio_buf_fixed_idx(idx)    - registered buffer at offset 0
 *
 * Example:
 * @code
 *   // Regular buffer
 *   auraio_read(engine, fd, auraio_buf(my_ptr), len, offset, cb, ud);
 *
 *   // Registered buffer (after auraio_register_buffers())
 *   auraio_read(engine, fd, auraio_buf_fixed(0, 0), len, offset, cb, ud);
 * @endcode
 */
typedef struct {
    auraio_buf_type_t type; /**< Buffer type discriminator */
    union {
        void *ptr; /**< Direct buffer pointer (AURAIO_BUF_UNREGISTERED) */
        struct {
            int index;     /**< Registered buffer index */
            size_t offset; /**< Offset within registered buffer */
        } fixed;           /**< Registered buffer reference (AURAIO_BUF_REGISTERED) */
    } u;
} auraio_buf_t;

/**
 * Create a buffer descriptor for an unregistered (regular) buffer
 *
 * @param ptr Pointer to user-provided buffer
 * @return Buffer descriptor
 */
static inline auraio_buf_t auraio_buf(void *ptr) {
    auraio_buf_t buf = {AURAIO_BUF_UNREGISTERED, {0}};
    buf.u.ptr = ptr;
    return buf;
}

/**
 * Create a buffer descriptor for a registered (fixed) buffer
 *
 * The buffer must have been previously registered with
 * auraio_register_buffers().
 *
 * @param index  Index in the registered buffer array
 * @param offset Offset within that buffer (commonly 0)
 * @return Buffer descriptor
 */
static inline auraio_buf_t auraio_buf_fixed(int index, size_t offset) {
    auraio_buf_t buf = {AURAIO_BUF_UNREGISTERED, {0}};
    if (index < 0) {
        /* Return zeroed AURAIO_BUF_UNREGISTERED buffer with ptr=NULL.
         * Submitting this to an I/O function will fail with EINVAL. */
        return buf;
    }
    buf.type = AURAIO_BUF_REGISTERED;
    buf.u.fixed.index = index;
    buf.u.fixed.offset = offset;
    return buf;
}

/**
 * Create a buffer descriptor for a registered buffer at offset 0
 *
 * Convenience function equivalent to auraio_buf_fixed(index, 0).
 *
 * @param index Index in the registered buffer array
 * @return Buffer descriptor
 */
static inline auraio_buf_t auraio_buf_fixed_idx(int index) {
    return auraio_buf_fixed(index, 0);
}

/* ============================================================================
 * Options Initialization
 * ============================================================================
 */

/**
 * Initialize options with default values
 *
 * Always call this before modifying individual options.
 *
 * @param options Options structure to initialize
 */
AURAIO_API void auraio_options_init(auraio_options_t *options);

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

/**
 * Create a new async I/O engine with default options
 *
 * Automatically detects CPU cores, creates io_uring rings, and initializes
 * adaptive controllers. One ring is created per CPU core.
 *
 * @return Engine handle, or NULL on failure (errno set)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_engine_t *auraio_create(void);

/**
 * Create a new async I/O engine with custom options
 *
 * @param options Configuration options (initialize with auraio_options_init
 * first)
 * @return Engine handle, or NULL on failure (errno set)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_engine_t *
auraio_create_with_options(const auraio_options_t *options);

/**
 * Destroy an async I/O engine
 *
 * Signals shutdown, waits for all pending operations to complete, then
 * frees all resources. Once called, all new submission attempts will
 * immediately fail with errno=ESHUTDOWN.
 *
 * Safe to call from any thread. In-flight operations complete normally
 * before this function returns.
 *
 * LIFECYCLE CONSTRAINT: The caller must ensure all worker threads have
 * stopped submitting I/O and completed their buffer pool operations BEFORE
 * calling this function. While the library includes safety checks to detect
 * use-after-destroy, the correct pattern is:
 *   1. Signal worker threads to stop
 *   2. Join/wait for all worker threads
 *   3. Call auraio_destroy()
 *
 * BUFFER POOL NOTE: The engine uses a single internal buffer pool. For optimal
 * performance, use one auraio_engine_t per application. If multiple engines
 * are created, only the first engine's buffer pool will benefit from per-thread
 * caching within each thread.
 *
 * Passing NULL is safe (no-op). Passing an already-destroyed engine is
 * undefined behavior.
 *
 * @param engine Engine to destroy (may be NULL)
 */
AURAIO_API void auraio_destroy(auraio_engine_t *engine);

/* ============================================================================
 * Core I/O Operations
 * ============================================================================
 */

/**
 * Submit an asynchronous read operation
 *
 * The callback is invoked when the read completes. The buffer must remain
 * valid until the callback is called.
 *
 * Supports both regular and registered buffers via auraio_buf_t descriptor:
 * @code
 *   // Regular buffer
 *   auraio_read(engine, fd, auraio_buf(ptr), len, offset, cb, ud);
 *
 *   // Registered buffer (after auraio_register_buffers())
 *   auraio_read(engine, fd, auraio_buf_fixed(0, 0), len, offset, cb, ud);
 * @endcode
 *
 * For best performance with O_DIRECT files, use auraio_buffer_alloc() to get
 * properly aligned buffers, or use registered buffers for zero-copy I/O.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param buf       Buffer descriptor (use auraio_buf() or auraio_buf_fixed())
 * @param len       Number of bytes to read
 * @param offset    File offset to read from
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, ENOENT, EOVERFLOW, or ENOMEM)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_request_t *
auraio_read(auraio_engine_t *engine, int fd, auraio_buf_t buf, size_t len, off_t offset,
            auraio_callback_t callback, void *user_data);

/**
 * Submit an asynchronous write operation
 *
 * The callback is invoked when the write completes. The buffer must remain
 * valid until the callback is called.
 *
 * Supports both regular and registered buffers via auraio_buf_t descriptor:
 * @code
 *   // Regular buffer
 *   auraio_write(engine, fd, auraio_buf(ptr), len, offset, cb, ud);
 *
 *   // Registered buffer (after auraio_register_buffers())
 *   auraio_write(engine, fd, auraio_buf_fixed(0, 0), len, offset, cb, ud);
 * @endcode
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param buf       Buffer descriptor (use auraio_buf() or auraio_buf_fixed())
 * @param len       Number of bytes to write
 * @param offset    File offset to write to
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, ENOENT, EOVERFLOW, or ENOMEM)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_request_t *
auraio_write(auraio_engine_t *engine, int fd, auraio_buf_t buf, size_t len, off_t offset,
             auraio_callback_t callback, void *user_data);

/**
 * Submit an asynchronous fsync operation
 *
 * Ensures all previous writes to the file descriptor are flushed to storage.
 * Pass AURAIO_FSYNC_DEFAULT (0) for a full fsync, or AURAIO_FSYNC_DATASYNC
 * for fdatasync behavior (data only, skip metadata if possible).
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param flags     Fsync flags (AURAIO_FSYNC_DEFAULT or AURAIO_FSYNC_DATASYNC)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_request_t *auraio_fsync(auraio_engine_t *engine, int fd,
                                                             auraio_fsync_flags_t flags,
                                                             auraio_callback_t callback,
                                                             void *user_data);

/* ============================================================================
 * Vectored I/O Operations
 * ============================================================================
 */

/**
 * Submit an asynchronous vectored read operation
 *
 * Reads into multiple buffers in a single operation (scatter read).
 * The iovec array and all buffers must remain valid until callback.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param iov       Array of iovec structures
 * @param iovcnt    Number of elements in iov array
 * @param offset    File offset to read from
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_request_t *
auraio_readv(auraio_engine_t *engine, int fd, const struct iovec *iov, int iovcnt, off_t offset,
             auraio_callback_t callback, void *user_data);

/**
 * Submit an asynchronous vectored write operation
 *
 * Writes from multiple buffers in a single operation (gather write).
 * The iovec array and all buffers must remain valid until callback.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param iov       Array of iovec structures
 * @param iovcnt    Number of elements in iov array
 * @param offset    File offset to write to
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
AURAIO_API AURAIO_WARN_UNUSED auraio_request_t *
auraio_writev(auraio_engine_t *engine, int fd, const struct iovec *iov, int iovcnt, off_t offset,
              auraio_callback_t callback, void *user_data);

/* ============================================================================
 * Cancellation
 * ============================================================================
 */

/**
 * Cancel a pending I/O operation
 *
 * Attempts to cancel the specified request. If successful, the request's
 * callback will be invoked with result = -ECANCELED.
 *
 * Cancellation is best-effort: if the operation has already completed or
 * is being processed, it may not be cancelled.
 *
 * @param engine Engine handle (must be the same engine that created the
 * request)
 * @param req    Request to cancel (returned by read/write/fsync functions).
 *               Must belong to @p engine; passing a request from a different
 *               engine is undefined behavior.
 * @return 0 if cancellation was submitted, -1 on error (errno set to EINVAL,
 *         EALREADY, ESHUTDOWN, or ENOMEM)
 *         Note: 0 does not guarantee the operation will be cancelled
 */
AURAIO_API AURAIO_WARN_UNUSED int auraio_cancel(auraio_engine_t *engine, auraio_request_t *req);

/* ============================================================================
 * Request Introspection
 * ============================================================================
 */

/**
 * Check if a request is still pending
 *
 * @param req Request handle
 * @return true if still in-flight, false if completed or cancelled
 */
AURAIO_API bool auraio_request_pending(const auraio_request_t *req);

/**
 * Get the file descriptor associated with a request
 *
 * @param req Request handle
 * @return File descriptor, or -1 if request is invalid
 */
AURAIO_API int auraio_request_fd(const auraio_request_t *req);

/**
 * Get user data associated with a request
 *
 * @param req Request handle
 * @return User data pointer passed to submission function
 */
AURAIO_API void *auraio_request_user_data(const auraio_request_t *req);

/* ============================================================================
 * Event Processing
 * ============================================================================
 */

/**
 * Get a pollable file descriptor for the engine
 *
 * Returns a file descriptor that becomes readable when completions are
 * available. Use this for integration with event loops (epoll, kqueue, etc).
 *
 * The fd uses level-triggered semantics: it remains readable as long as
 * unprocessed completions exist. Call auraio_poll() to process completions
 * and clear the readable state. Compatible with epoll (EPOLLIN),
 * poll (POLLIN), and select.
 *
 * @param engine Engine handle
 * @return Pollable fd, or -1 on error (errno set)
 */
AURAIO_API int auraio_get_poll_fd(const auraio_engine_t *engine);

/**
 * Process completed operations (non-blocking)
 *
 * Checks for completions without blocking and invokes callbacks for any
 * completed operations.
 *
 * THREADING MODEL: auraio_poll(), auraio_wait(), and auraio_run() must
 * NOT be called concurrently on the same engine. These functions are
 * designed for single-threaded event loop patterns. Multiple threads
 * may submit I/O concurrently, but only one thread should poll/wait.
 *
 * @param engine Engine handle
 * @return Number of completions processed
 */
AURAIO_API int auraio_poll(auraio_engine_t *engine);

/**
 * Wait for at least one completion
 *
 * Blocks until at least one operation completes or timeout expires.
 * Must NOT be called concurrently with auraio_poll() or auraio_run()
 * on the same engine (see auraio_poll() threading model note).
 *
 * @param engine     Engine handle
 * @param timeout_ms Maximum wait time in milliseconds (-1 = forever, 0 = don't
 * block)
 * @return Number of completions processed, or -1 on error
 */
AURAIO_API int auraio_wait(auraio_engine_t *engine, int timeout_ms);

/**
 * Run event loop until stopped
 *
 * Blocks, continuously processing completions until auraio_stop() is called.
 * Useful for dedicating a thread to I/O processing.
 *
 * @param engine Engine handle
 */
AURAIO_API void auraio_run(auraio_engine_t *engine);

/**
 * Signal the event loop to stop
 *
 * Safe to call from any thread, including from within a callback.
 * auraio_run() will return after processing current completions.
 *
 * @param engine Engine handle
 */
AURAIO_API void auraio_stop(auraio_engine_t *engine);

/**
 * Drain all pending I/O operations
 *
 * Waits until all in-flight operations across all rings have completed.
 * Useful for graceful shutdown or synchronization points.
 *
 * New submissions are NOT blocked during drain; if other threads submit
 * operations concurrently, drain will process those as well.
 *
 * On timeout, returns -1 with errno=ETIMEDOUT. Operations that completed
 * before the timeout are still processed (callbacks invoked).
 *
 * @param engine     Engine handle
 * @param timeout_ms Maximum wait time in milliseconds (-1 = wait forever,
 *                   0 = non-blocking poll)
 * @return Total number of completions processed, or -1 on error/timeout
 *         (errno = ETIMEDOUT if deadline exceeded, EINVAL if engine is NULL)
 */
AURAIO_API int auraio_drain(auraio_engine_t *engine, int timeout_ms);

/* ============================================================================
 * Managed Buffers (Optional)
 * ============================================================================
 */

/**
 * Allocate an aligned buffer from the engine's pool
 *
 * Returns page-aligned memory suitable for O_DIRECT I/O. More efficient
 * than posix_memalign() for repeated allocations.
 *
 * Thread-safe: may be called from any thread. Uses per-thread caching
 * for fast allocation. A buffer allocated by one thread may be freed
 * by another.
 *
 * @param engine Engine handle
 * @param size   Buffer size in bytes
 * @return Aligned buffer, or NULL on failure
 */
AURAIO_API AURAIO_WARN_UNUSED void *auraio_buffer_alloc(auraio_engine_t *engine, size_t size);

/**
 * Return a buffer to the engine's pool
 *
 * The buffer must have been allocated by auraio_buffer_alloc() on the same
 * engine. The size parameter MUST match the original allocation size exactly;
 * passing a different size causes undefined behavior (pool corruption).
 *
 * Thread-safe: may be called from any thread.
 *
 * @param engine Engine handle
 * @param buf    Buffer to free (may be NULL)
 * @param size   Size of the buffer (MUST match allocation size exactly)
 */
AURAIO_API void auraio_buffer_free(auraio_engine_t *engine, void *buf, size_t size);

/* ============================================================================
 * Registered Buffers (Advanced)
 * ============================================================================
 *
 * Registered buffers eliminate kernel mapping overhead for small, frequent I/O.
 * After registration, use auraio_buf_fixed() with the standard
 * auraio_read/write.
 *
 * WHEN TO USE REGISTERED BUFFERS:
 *   - Same buffers reused across 1000+ I/O operations
 *   - High-frequency small I/O (< 16KB) where mapping overhead is significant
 *   - Zero-copy is critical for performance
 *
 * WHEN TO USE REGULAR BUFFERS (auraio_buf(ptr)):
 *   - One-off or infrequent operations
 *   - Short-lived buffers or dynamic buffer count
 *   - Simpler code without registration lifecycle
 *
 * Usage:
 *   struct iovec iovs[2] = {{buf1, 4096}, {buf2, 4096}};
 *   auraio_register_buffers(engine, iovs, 2);
 *   auraio_read(engine, fd, auraio_buf_fixed(0, 0), 4096, offset, callback,
 * ud);
 *   // buffer index 0 refers to buf1
 */

/**
 * Register buffers with the kernel for fixed buffer I/O
 *
 * Pre-registers buffers with the kernel to eliminate mapping overhead.
 * Call this once at startup, not per-operation. After registration,
 * use auraio_buf_fixed() to reference buffers by index.
 *
 * Submissions using auraio_buf_fixed() fail with errno=EBUSY while a deferred
 * unregister is draining. Use auraio_request_unregister_buffers() from callback
 * contexts, or auraio_unregister_buffers() for a synchronous wait.
 *
 * @param engine Engine handle
 * @param iovs   Array of iovec describing buffers to register
 * @param count  Number of buffers
 * @return 0 on success, -1 on error (errno set)
 */
AURAIO_API AURAIO_WARN_UNUSED int auraio_register_buffers(auraio_engine_t *engine,
                                                          const struct iovec *iovs, int count);

/**
 * Request deferred unregister of registered buffers (callback-safe)
 *
 * Marks registered buffers as draining and returns immediately. New
 * fixed-buffer submissions fail with errno=EBUSY while draining. Final
 * unregister is completed lazily once in-flight fixed-buffer operations reach
 * zero.
 *
 * Safe to call from completion callbacks.
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
AURAIO_API int auraio_request_unregister_buffers(auraio_engine_t *engine);

/**
 * Unregister previously registered buffers (synchronous)
 *
 * For non-callback callers, this waits until in-flight fixed-buffer operations
 * drain and unregister completes. If called from a callback, it degrades to
 * auraio_request_unregister_buffers() and returns immediately.
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
AURAIO_API int auraio_unregister_buffers(auraio_engine_t *engine);

/* ============================================================================
 * Registered Files (Advanced)
 * ============================================================================
 *
 * Registered file descriptors eliminate fd lookup overhead in the kernel.
 * Useful for workloads with many files.
 */

/**
 * Register file descriptors with the kernel
 *
 * Pre-registers file descriptors to eliminate lookup overhead.
 * After registration, regular I/O calls
 * (auraio_read/auraio_write/readv/writev/fsync) automatically use the
 * registered-file table when the submitted fd is found there.
 *
 * @param engine Engine handle
 * @param fds    Array of file descriptors to register
 * @param count  Number of file descriptors
 * @return 0 on success, -1 on error (errno set)
 */
AURAIO_API AURAIO_WARN_UNUSED int auraio_register_files(auraio_engine_t *engine, const int *fds,
                                                        int count);

/**
 * Update a registered file descriptor
 *
 * Replaces a previously registered fd at the given index.
 * Use -1 to unregister a slot without replacing it.
 *
 * PARTIAL UPDATE WARNING: This function updates each io_uring ring
 * sequentially. If an error occurs on ring N, rings 0..N-1 will have the new fd
 * while rings N..max will retain the old fd. On failure, the caller should
 * unregister all files with auraio_unregister_files() and re-register them.
 *
 * @param engine Engine handle
 * @param index  Index in registered file array
 * @param fd     New file descriptor (-1 to unregister slot)
 * @return 0 on success, -1 on error (errno set; state may be inconsistent)
 */
AURAIO_API AURAIO_WARN_UNUSED int auraio_update_file(auraio_engine_t *engine, int index, int fd);

/**
 * Request deferred unregister of registered files (callback-safe)
 *
 * Marks registered files for unregister and returns immediately. Safe to call
 * from completion callbacks.
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
AURAIO_API int auraio_request_unregister_files(auraio_engine_t *engine);

/**
 * Unregister previously registered files
 *
 * For non-callback callers, this waits until unregister completes. If called
 * from a callback, it degrades to auraio_request_unregister_files() and
 * returns immediately.
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
AURAIO_API int auraio_unregister_files(auraio_engine_t *engine);

/* ============================================================================
 * Statistics (Optional)
 * ============================================================================
 */

/**
 * Get current engine statistics
 *
 * Retrieves throughput, latency, and tuning parameters for monitoring.
 * If engine or stats is NULL, returns without modifying stats.
 *
 * Thread-safe: reads atomic counters and locks each ring briefly.
 *
 * @param engine Engine handle (may be NULL)
 * @param stats  Output statistics structure (may be NULL)
 */
AURAIO_API void auraio_get_stats(const auraio_engine_t *engine, auraio_stats_t *stats);

/**
 * Get the number of io_uring rings in the engine
 *
 * @param engine Engine handle
 * @return Number of rings, or 0 if engine is NULL
 */
AURAIO_API int auraio_get_ring_count(const auraio_engine_t *engine);

/**
 * Get statistics for a specific ring
 *
 * Thread-safe: locks the ring's mutex during the read.
 * If engine or stats is NULL, returns -1 without modifying stats.
 * If ring_idx is out of range, returns -1 and zeroes stats.
 *
 * @param engine   Engine handle
 * @param ring_idx Ring index (0 to auraio_get_ring_count()-1)
 * @param stats    Output statistics structure
 * @return 0 on success, -1 on error (NULL engine/stats or invalid ring_idx)
 */
AURAIO_API int auraio_get_ring_stats(const auraio_engine_t *engine, int ring_idx,
                                     auraio_ring_stats_t *stats);

/**
 * Get a latency histogram snapshot for a specific ring
 *
 * Copies the current active histogram. The snapshot is approximate —
 * see auraio_histogram_t documentation for details.
 *
 * Thread-safe: locks the ring's mutex during the copy.
 * If engine or hist is NULL, returns -1 without modifying hist.
 * If ring_idx is out of range, returns -1 and zeroes hist.
 *
 * @param engine   Engine handle
 * @param ring_idx Ring index (0 to auraio_get_ring_count()-1)
 * @param hist     Output histogram structure
 * @return 0 on success, -1 on error (NULL engine/hist or invalid ring_idx)
 */
AURAIO_API int auraio_get_histogram(const auraio_engine_t *engine, int ring_idx,
                                    auraio_histogram_t *hist);

/**
 * Get buffer pool statistics
 *
 * Thread-safe: reads atomic counters from the buffer pool.
 * If engine or stats is NULL, returns -1 without modifying stats.
 *
 * @param engine Engine handle
 * @param stats  Output statistics structure
 * @return 0 on success, -1 on error (NULL engine or stats)
 */
AURAIO_API int auraio_get_buffer_stats(const auraio_engine_t *engine, auraio_buffer_stats_t *stats);

/**
 * Get human-readable name for an AIMD phase value
 *
 * Valid phase values are 0-5 (AURAIO_PHASE_BASELINE through
 * AURAIO_PHASE_CONVERGED).  Returns "UNKNOWN" for out-of-range values.
 *
 * @param phase Phase value (from auraio_ring_stats_t.aimd_phase)
 * @return Static string like "BASELINE", "PROBING", etc., or "UNKNOWN"
 */
AURAIO_API const char *auraio_phase_name(int phase);

/**
 * Get library version string
 *
 * @return Version string (e.g., "0.1.0")
 */
AURAIO_API const char *auraio_version(void);

/* ============================================================================
 * Logging (Optional)
 * ============================================================================
 */

/** Log severity levels (match syslog priorities). */
#define AURAIO_LOG_ERR 3    /**< Error (matches syslog LOG_ERR) */
#define AURAIO_LOG_WARN 4   /**< Warning (matches syslog LOG_WARNING) */
#define AURAIO_LOG_NOTICE 5 /**< Notice (matches syslog LOG_NOTICE) */
#define AURAIO_LOG_INFO 6   /**< Informational (matches syslog LOG_INFO) */
#define AURAIO_LOG_DEBUG 7  /**< Debug (matches syslog LOG_DEBUG) */

/**
 * Log callback type
 *
 * @param level   Severity (AURAIO_LOG_ERR .. AURAIO_LOG_DEBUG; matches syslog)
 * @param msg     Formatted message string (NUL-terminated)
 * @param userdata Opaque pointer passed to auraio_set_log_handler()
 */
typedef void (*auraio_log_fn)(int level, const char *msg, void *userdata);

/**
 * Set the library-wide log handler
 *
 * By default no handler is installed and the library is silent.
 * The handler is global (process-wide) and may be called from any thread.
 *
 * @param handler  Log callback (NULL to disable logging)
 * @param userdata Opaque pointer forwarded to the callback
 */
AURAIO_API void auraio_set_log_handler(auraio_log_fn handler, void *userdata);

/**
 * Emit a log message through the registered handler
 *
 * No-op when no handler is registered.  Thread-safe.
 * Messages are formatted into a 256-byte internal buffer (truncated if longer).
 *
 * @param level  Severity (AURAIO_LOG_ERR .. AURAIO_LOG_DEBUG)
 * @param fmt    printf-style format string
 * @param ...    Format arguments
 */
AURAIO_API void auraio_log_emit(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * Get library version as integer
 *
 * Format: (major * 10000 + minor * 100 + patch)
 * Example: 0.1.0 = 100
 *
 * @return Version integer
 */
AURAIO_API int auraio_version_int(void);

#ifdef __cplusplus
}
#endif

#endif /* AURAIO_H */
