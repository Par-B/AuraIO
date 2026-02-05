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
 *   auraio_request_t *req = auraio_read(engine, fd, auraio_buf(buf), size, 0, callback, user_data);
 *   auraio_wait(engine, -1);
 *   auraio_buffer_free(engine, buf, size);
 *   auraio_destroy(engine);
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

/* ============================================================================
 * Version Information
 * ============================================================================
 */

#define AURAIO_VERSION_MAJOR 1
#define AURAIO_VERSION_MINOR 0
#define AURAIO_VERSION_PATCH 1

/** Version as a single integer: (major * 10000 + minor * 100 + patch) */
#define AURAIO_VERSION \
  (AURAIO_VERSION_MAJOR * 10000 + AURAIO_VERSION_MINOR * 100 + AURAIO_VERSION_PATCH)

/** Version as a string */
#define AURAIO_VERSION_STRING "1.0.1"
#include <stddef.h>
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
 * RESTRICTIONS:
 * - The callback executes in the context of auraio_poll() or auraio_wait()
 * - Callbacks may submit new I/O operations (auraio_read, auraio_write, etc.)
 * - Callbacks MUST NOT call auraio_destroy() - this will cause undefined behavior
 * - Callbacks should complete quickly to avoid blocking other completions
 * - The request handle (req) becomes invalid after the callback returns
 */
typedef void (*auraio_callback_t)(auraio_request_t *req, ssize_t result,
                                  void *user_data);

/**
 * Engine statistics
 *
 * Retrieved via auraio_get_stats() for monitoring and debugging.
 */
typedef struct {
  long long ops_completed;       /**< Total operations completed */
  long long bytes_transferred;   /**< Total bytes read/written */
  double current_throughput_bps; /**< Current throughput (bytes/sec) */
  double p99_latency_ms;         /**< 99th percentile latency (ms) */
  int current_in_flight;         /**< Current in-flight operations */
  int optimal_in_flight;         /**< Tuned optimal in-flight limit */
  int optimal_batch_size;        /**< Tuned optimal batch size */
} auraio_stats_t;

/**
 * Engine configuration options
 *
 * Used with auraio_create_with_options() for custom configuration.
 * Initialize with auraio_options_init() before modifying.
 */
typedef struct {
  int queue_depth;           /**< Queue depth per ring (default: 256) */
  int ring_count;            /**< Number of rings, 0 = auto (one per CPU) */
  int initial_in_flight;     /**< Initial in-flight limit (default: queue_depth/4) */
  int min_in_flight;         /**< Minimum in-flight limit (default: 4) */
  double max_p99_latency_ms; /**< Target max P99 latency, 0 = auto */
  size_t buffer_alignment;   /**< Buffer alignment (default: system page size) */
  bool disable_adaptive;     /**< Disable adaptive tuning */

  /* Advanced io_uring features (Phase 5) */
  bool enable_sqpoll;        /**< Enable SQPOLL mode (requires root/CAP_SYS_NICE) */
  int sqpoll_idle_ms;        /**< SQPOLL idle timeout in ms (default: 1000) */
} auraio_options_t;

/**
 * Fsync flags for auraio_fsync_ex()
 */
typedef enum {
  AURAIO_FSYNC_DEFAULT = 0,      /**< Full fsync (metadata + data) */
  AURAIO_FSYNC_DATASYNC = 1,     /**< fdatasync (data only, skip metadata if possible) */
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
      int index;      /**< Registered buffer index */
      size_t offset;  /**< Offset within registered buffer */
    } fixed;          /**< Registered buffer reference (AURAIO_BUF_REGISTERED) */
  } u;
} auraio_buf_t;

/**
 * Create a buffer descriptor for an unregistered (regular) buffer
 *
 * @param ptr Pointer to user-provided buffer
 * @return Buffer descriptor
 */
static inline auraio_buf_t auraio_buf(void *ptr) {
  auraio_buf_t buf;
  buf.type = AURAIO_BUF_UNREGISTERED;
  buf.u.ptr = ptr;
  return buf;
}

/**
 * Create a buffer descriptor for a registered (fixed) buffer
 *
 * The buffer must have been previously registered with auraio_register_buffers().
 *
 * @param index  Index in the registered buffer array
 * @param offset Offset within that buffer (commonly 0)
 * @return Buffer descriptor
 */
static inline auraio_buf_t auraio_buf_fixed(int index, size_t offset) {
  auraio_buf_t buf;
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
void auraio_options_init(auraio_options_t *options);

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
auraio_engine_t *auraio_create(void);

/**
 * Create a new async I/O engine with custom options
 *
 * @param options Configuration options (initialize with auraio_options_init first)
 * @return Engine handle, or NULL on failure (errno set)
 */
auraio_engine_t *auraio_create_with_options(const auraio_options_t *options);

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
 * @param engine Engine to destroy (may be NULL)
 */
void auraio_destroy(auraio_engine_t *engine);

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
auraio_request_t *auraio_read(auraio_engine_t *engine, int fd, auraio_buf_t buf,
                               size_t len, off_t offset,
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
auraio_request_t *auraio_write(auraio_engine_t *engine, int fd, auraio_buf_t buf,
                                size_t len, off_t offset,
                                auraio_callback_t callback, void *user_data);

/**
 * Submit an asynchronous fsync operation
 *
 * Ensures all previous writes to the file descriptor are flushed to storage.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
auraio_request_t *auraio_fsync(auraio_engine_t *engine, int fd,
                                auraio_callback_t callback, void *user_data);

/**
 * Submit an asynchronous fsync with flags
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param flags     Fsync flags (AURAIO_FSYNC_DATASYNC for fdatasync behavior)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
auraio_request_t *auraio_fsync_ex(auraio_engine_t *engine, int fd,
                                   auraio_fsync_flags_t flags,
                                   auraio_callback_t callback, void *user_data);

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
auraio_request_t *auraio_readv(auraio_engine_t *engine, int fd,
                                const struct iovec *iov, int iovcnt,
                                off_t offset, auraio_callback_t callback,
                                void *user_data);

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
auraio_request_t *auraio_writev(auraio_engine_t *engine, int fd,
                                 const struct iovec *iov, int iovcnt,
                                 off_t offset, auraio_callback_t callback,
                                 void *user_data);

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
 * @param engine Engine handle
 * @param req    Request to cancel (returned by read/write/fsync functions)
 * @return 0 if cancellation was submitted, -1 on error (errno set to EINVAL,
 *         EALREADY, ESHUTDOWN, or ENOMEM)
 *         Note: 0 does not guarantee the operation will be cancelled
 */
int auraio_cancel(auraio_engine_t *engine, auraio_request_t *req);

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
bool auraio_request_pending(const auraio_request_t *req);

/**
 * Get the file descriptor associated with a request
 *
 * @param req Request handle
 * @return File descriptor, or -1 if request is invalid
 */
int auraio_request_fd(const auraio_request_t *req);

/**
 * Get user data associated with a request
 *
 * @param req Request handle
 * @return User data pointer passed to submission function
 */
void *auraio_request_user_data(const auraio_request_t *req);

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
 * When the fd is readable, call auraio_poll() to process completions.
 *
 * @param engine Engine handle
 * @return Pollable fd, or -1 on error (errno set)
 */
int auraio_get_poll_fd(auraio_engine_t *engine);

/**
 * Process completed operations (non-blocking)
 *
 * Checks for completions without blocking and invokes callbacks for any
 * completed operations.
 *
 * @param engine Engine handle
 * @return Number of completions processed
 */
int auraio_poll(auraio_engine_t *engine);

/**
 * Wait for at least one completion
 *
 * Blocks until at least one operation completes or timeout expires.
 *
 * @param engine     Engine handle
 * @param timeout_ms Maximum wait time in milliseconds (-1 = forever, 0 = don't
 * block)
 * @return Number of completions processed, or -1 on error
 */
int auraio_wait(auraio_engine_t *engine, int timeout_ms);

/**
 * Run event loop until stopped
 *
 * Blocks, continuously processing completions until auraio_stop() is called.
 * Useful for dedicating a thread to I/O processing.
 *
 * @param engine Engine handle
 */
void auraio_run(auraio_engine_t *engine);

/**
 * Signal the event loop to stop
 *
 * Safe to call from any thread, including from within a callback.
 * auraio_run() will return after processing current completions.
 *
 * @param engine Engine handle
 */
void auraio_stop(auraio_engine_t *engine);

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
 * @param engine Engine handle
 * @param size   Buffer size in bytes
 * @return Aligned buffer, or NULL on failure
 */
void *auraio_buffer_alloc(auraio_engine_t *engine, size_t size);

/**
 * Return a buffer to the engine's pool
 *
 * The buffer must have been allocated by auraio_buffer_alloc() on the same
 * engine. The size must match the original allocation size.
 *
 * @param engine Engine handle
 * @param buf    Buffer to free (may be NULL)
 * @param size   Size of the buffer (must match allocation size)
 */
void auraio_buffer_free(auraio_engine_t *engine, void *buf, size_t size);

/* ============================================================================
 * Registered Buffers (Advanced)
 * ============================================================================
 *
 * Registered buffers eliminate kernel mapping overhead for small, frequent I/O.
 * After registration, use auraio_buf_fixed() with the standard auraio_read/write.
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
 *   auraio_read(engine, fd, auraio_buf_fixed(0, 0), 4096, offset, callback, ud);
 *   // buffer index 0 refers to buf1
 */

/**
 * Register buffers with the kernel for fixed buffer I/O
 *
 * Pre-registers buffers with the kernel to eliminate mapping overhead.
 * Call this once at startup, not per-operation. After registration,
 * use auraio_buf_fixed() to reference buffers by index.
 *
 * LIFECYCLE CONSTRAINT: The caller must ensure no I/O operations using
 * these registered buffers are in-flight when calling auraio_unregister_buffers()
 * or auraio_destroy(). The library does not synchronize between I/O operations
 * and buffer unregistration.
 *
 * @param engine Engine handle
 * @param iovs   Array of iovec describing buffers to register
 * @param count  Number of buffers
 * @return 0 on success, -1 on error (errno set)
 */
int auraio_register_buffers(auraio_engine_t *engine, const struct iovec *iovs, int count);

/**
 * Unregister previously registered buffers
 *
 * LIFECYCLE CONSTRAINT: The caller must ensure no I/O operations using
 * registered buffers (via auraio_buf_fixed()) are in-flight when calling
 * this function. Unregistering while I/O is in-flight results in undefined
 * behavior.
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
int auraio_unregister_buffers(auraio_engine_t *engine);

/* ============================================================================
 * Registered Files (Advanced - Phase 5)
 * ============================================================================
 *
 * Registered file descriptors eliminate fd lookup overhead in the kernel.
 * Useful for workloads with many files.
 */

/**
 * Register file descriptors with the kernel
 *
 * Pre-registers file descriptors to eliminate lookup overhead.
 * After registration, use the index (not fd) with _registered variants.
 *
 * @param engine Engine handle
 * @param fds    Array of file descriptors to register
 * @param count  Number of file descriptors
 * @return 0 on success, -1 on error (errno set)
 */
int auraio_register_files(auraio_engine_t *engine, const int *fds, int count);

/**
 * Update a registered file descriptor
 *
 * Replaces a previously registered fd at the given index.
 * Use -1 to unregister a slot without replacing it.
 *
 * PARTIAL UPDATE WARNING: This function updates each io_uring ring sequentially.
 * If an error occurs on ring N, rings 0..N-1 will have the new fd while rings
 * N..max will retain the old fd. On failure, the caller should unregister all
 * files with auraio_unregister_files() and re-register them.
 *
 * @param engine Engine handle
 * @param index  Index in registered file array
 * @param fd     New file descriptor (-1 to unregister slot)
 * @return 0 on success, -1 on error (errno set; state may be inconsistent)
 */
int auraio_update_file(auraio_engine_t *engine, int index, int fd);

/**
 * Unregister previously registered files
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error
 */
int auraio_unregister_files(auraio_engine_t *engine);

/* ============================================================================
 * Statistics (Optional)
 * ============================================================================
 */

/**
 * Get current engine statistics
 *
 * Retrieves throughput, latency, and tuning parameters for monitoring.
 *
 * @param engine Engine handle
 * @param stats  Output statistics structure
 */
void auraio_get_stats(auraio_engine_t *engine, auraio_stats_t *stats);

/**
 * Get library version string
 *
 * @return Version string (e.g., "1.0.1")
 */
const char *auraio_version(void);

/**
 * Get library version as integer
 *
 * Format: (major * 10000 + minor * 100 + patch)
 * Example: 1.0.1 = 10001
 *
 * @return Version integer
 */
int auraio_version_int(void);

#ifdef __cplusplus
}
#endif

#endif /* AURAIO_H */
