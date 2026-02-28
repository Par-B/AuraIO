// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file aura.h
 * @brief Self-tuning async I/O library built on io_uring
 *
 * A high-performance async I/O library that automatically tunes itself
 * for optimal throughput and latency using AIMD congestion control.
 *
 * Basic usage:
 * @code
 *   aura_engine_t *engine = aura_create();
 *   void *buf = aura_buffer_alloc(engine, size);
 *   aura_request_t *req = aura_read(engine, fd, aura_buf(buf), size, 0,
 *                                    callback, user_data);
 *   aura_wait(engine, -1);
 *   aura_buffer_free(engine, buf);
 *   aura_destroy(engine);
 * @endcode
 *
 * Event loop integration:
 * @code
 *   int poll_fd = aura_get_poll_fd(engine);
 *   // Add poll_fd to your epoll/kqueue/select
 *   // When readable, call aura_poll(engine)
 * @endcode
 */

#ifndef AURA_H
#define AURA_H

#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#ifdef __linux__
#    include <linux/stat.h> /* struct statx */
#endif

/* ============================================================================
 * Version Information
 * ============================================================================
 */

#define AURA_VERSION_MAJOR 0
#define AURA_VERSION_MINOR 6
#define AURA_VERSION_PATCH 0

/** Version as a single integer: (major * 10000 + minor * 100 + patch) */
#define AURA_VERSION (AURA_VERSION_MAJOR * 10000 + AURA_VERSION_MINOR * 100 + AURA_VERSION_PATCH)

/** Version as a string (auto-generated from components) */
#define AURA_STRINGIFY_(x) #x
#define AURA_STRINGIFY(x) AURA_STRINGIFY_(x)
#define AURA_VERSION_STRING                                                                        \
    AURA_STRINGIFY(AURA_VERSION_MAJOR)                                                             \
    "." AURA_STRINGIFY(AURA_VERSION_MINOR) "." AURA_STRINGIFY(AURA_VERSION_PATCH)

/* Ensure version components stay within packed integer limits */
#if AURA_VERSION_MINOR > 99 || AURA_VERSION_PATCH > 99
#    error "Version minor/patch must be 0-99 for packed AURA_VERSION integer"
#endif

/* ============================================================================
 * Threading Model
 * ============================================================================
 *
 * SUBMISSIONS (aura_read, aura_write, aura_fsync, aura_readv, aura_writev,
 * aura_openat, aura_close, aura_statx, aura_fallocate, aura_ftruncate,
 * aura_sync_file_range, aura_cancel):
 *   Thread-safe. Multiple threads may submit I/O concurrently. Each
 *   submission acquires a per-ring mutex briefly; contention is low because
 *   the ADAPTIVE ring selector distributes load across rings.
 *
 * EVENT LOOP (aura_poll, aura_wait, aura_run, aura_drain):
 *   Single-threaded. These functions must NOT be called concurrently on the
 *   same engine. Designate one thread as the event loop thread. Submissions
 *   from other threads are safe during event loop processing.
 *
 * CALLBACKS:
 *   Execute on the thread that calls aura_poll() / aura_wait() / aura_run().
 *   Within a single poll/wait call, callbacks are invoked sequentially (never
 *   concurrently). See the aura_callback_t documentation for what is safe to
 *   call from within a callback.
 *
 * STATISTICS (aura_get_stats, aura_get_ring_stats, aura_get_histogram,
 * aura_get_buffer_stats):
 *   Thread-safe. Reads atomic counters and briefly locks each ring.
 *
 * REGISTRATION (aura_register_buffers, aura_register_files, aura_update_file,
 * aura_unregister, aura_request_unregister):
 *   Registration operations must be serialized (no concurrent register/
 *   unregister calls). I/O submissions from other threads are safe during
 *   registration.
 *
 * BUFFER POOL (aura_buffer_alloc, aura_buffer_free):
 *   Thread-safe. Uses per-thread caching for fast allocation. Buffers may
 *   be freed from a different thread than they were allocated on.
 */

/* ============================================================================
 * Symbol Visibility
 * ============================================================================
 *
 * AURA_API marks functions exported from the shared library.
 * Build with -fvisibility=hidden to hide internal symbols.
 */

#if defined(AURA_SHARED_BUILD)
#    define AURA_API __attribute__((visibility("default")))
#elif defined(AURA_STATIC_BUILD)
#    define AURA_API
#else
#    define AURA_API
#endif

/**
 * Warn if return value is ignored
 *
 * Applied to functions that return error codes or allocated resources.
 * Ignoring these return values is almost always a bug.
 */
#if defined(__GNUC__) || defined(__clang__)
#    define AURA_WARN_UNUSED __attribute__((warn_unused_result))
#else
#    define AURA_WARN_UNUSED
#endif

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
 * Created by aura_create(), destroyed by aura_destroy().
 * Thread-safe for submissions from multiple threads.
 */
typedef struct aura_engine aura_engine_t;

/**
 * Opaque request handle
 *
 * Returned by submission functions. Can be used to cancel in-flight operations
 * or query request state while in-flight.
 *
 * LIFETIME: The request handle is valid from submission until the completion
 * callback RETURNS. Inside the callback, the req parameter is valid and may
 * be passed to aura_request_fd(), aura_request_user_data(), etc. After the
 * callback returns, the handle is recycled and must not be used. This applies
 * whether the operation completed normally, with an error, or was cancelled
 * (callback receives -ECANCELED).
 *
 * THREAD SAFETY: It is safe to call aura_cancel() or aura_request_pending()
 * on a request handle from any thread, as long as the callback has not yet
 * returned. If aura_request_pending() returns false, the request has completed
 * but may still be inside its callback.
 */
typedef struct aura_request aura_request_t;

/**
 * Operation type identifiers
 *
 * Returned by aura_request_op_type() for dispatch in generic completion
 * handlers.  For read/write ops the callback result is bytes transferred;
 * for openat it is the new fd; for other ops it is 0 on success.
 */
typedef enum {
    AURA_OP_READ = 0,
    AURA_OP_WRITE = 1,
    AURA_OP_READV = 2,
    AURA_OP_WRITEV = 3,
    AURA_OP_FSYNC = 4,
    AURA_OP_FDATASYNC = 5,
    AURA_OP_CANCEL = 6,
    AURA_OP_READ_FIXED = 7,  /**< Read using registered buffer */
    AURA_OP_WRITE_FIXED = 8, /**< Write using registered buffer */
    AURA_OP_OPENAT = 9,      /**< Result is new fd (>= 0) */
    AURA_OP_CLOSE = 10,
    AURA_OP_STATX = 11,
    AURA_OP_FALLOCATE = 12,
    AURA_OP_FTRUNCATE = 13,
    AURA_OP_SYNC_FILE_RANGE = 14,
    AURA_OP__RESERVED_15 = 15, /**< Reserved for future ops */
    AURA_OP__RESERVED_16 = 16,
    AURA_OP__COUNT, /**< Number of op types (for dispatch tables) */
} aura_op_type_t;

/**
 * Submit flags for I/O operations
 *
 * Passed to aura_read(), aura_write(), etc. to control submission behavior.
 * Multiple flags may be OR'd together.
 */
typedef unsigned int aura_submit_flags_t;

/** No submit flags — pass to the flags parameter for default behavior. */
#define AURA_NO_FLAGS 0u

/** fd argument is a registered file index (from aura_register_files), not a raw fd.
 *  Skips auto-detection and sets IOSQE_FIXED_FILE directly. */
#define AURA_FIXED_FILE (1u << 0)

/**
 * Completion callback type
 *
 * Called when an async operation completes.
 *
 * @param req       Request handle (valid for the duration of the callback)
 * @param result    Bytes transferred on success, negative errno on failure
 *                  -ECANCELED if the operation was cancelled
 * @param user_data User pointer passed to aura_read/write/fsync
 *
 * NULL CALLBACK: Passing NULL as the callback is permitted. The I/O operation
 * will execute normally, but there is no notification of completion and no way
 * to retrieve the result. This is useful for fire-and-forget operations (e.g.,
 * advisory writes where success is not critical). The request handle returned
 * by the submission function can still be used with aura_request_pending()
 * and aura_cancel().
 *
 * CALLBACK SAFETY:
 *
 * The callback executes on the thread that called aura_poll(), aura_wait(),
 * or aura_run(). Callbacks within a single poll/wait call are sequential.
 * The request handle (req) is valid for the duration of the callback and
 * becomes invalid after the callback returns.
 *
 * Allowed in callbacks:
 * - All I/O submission functions (aura_read, aura_write, aura_fsync, etc.)
 * - aura_cancel()
 * - aura_stop()
 * - aura_request_unregister() (deferred unregister)
 * - All aura_request_*() introspection functions
 * - All aura_get_*() statistics functions
 * - aura_buffer_alloc() / aura_buffer_free()
 *
 * Forbidden in callbacks:
 * - aura_destroy() — undefined behavior
 * - aura_poll() / aura_wait() / aura_run() / aura_drain() — deadlock
 *
 * Auto-deferred in callbacks:
 * - aura_unregister() — detects callback context and automatically degrades
 *   to the deferred (non-blocking) path
 *
 * Callbacks must not block for extended periods. They run on the event loop
 * thread, so a slow callback delays processing of all other completions.
 */
typedef void (*aura_callback_t)(aura_request_t *req, ssize_t result, void *user_data);

/**
 * Engine statistics
 *
 * Retrieved via aura_get_stats() for monitoring and debugging.
 */
typedef struct {
    int64_t ops_completed;         /**< Total operations completed */
    int64_t bytes_transferred;     /**< Total bytes read/written */
    double current_throughput_bps; /**< Current throughput (bytes/sec) */
    double p99_latency_ms;         /**< 99th percentile latency (ms) */
    int current_in_flight;         /**< Current in-flight operations */
    int optimal_in_flight;         /**< Tuned optimal in-flight limit (sum across all rings) */
    int peak_in_flight;            /**< Observed peak in-flight across all rings */
    int optimal_batch_size;        /**< Tuned optimal batch size (per-ring average) */
    uint64_t adaptive_spills;      /**< ADAPTIVE mode: times a submission spilled to
                                      another ring */
    uint32_t _reserved[4];         /**< Reserved for future use; must be zero */
} aura_stats_t;

/** ABI stability: catch unexpected layout changes at compile time. */
#ifdef __cplusplus
static_assert(sizeof(aura_stats_t) == 72, "aura_stats_t ABI size changed");
#else
_Static_assert(sizeof(aura_stats_t) == 72, "aura_stats_t ABI size changed");
#endif

/**
 * Per-ring statistics
 *
 * Provides detailed per-ring metrics including AIMD controller state.
 * Retrieved via aura_get_ring_stats().
 */
typedef struct {
    int64_t ops_completed;     /**< Total operations completed on this ring */
    int64_t bytes_transferred; /**< Total bytes transferred through this ring */
    int pending_count;         /**< Current in-flight operations */
    int peak_in_flight;        /**< Observed peak in-flight (high-water mark) */
    int in_flight_limit;       /**< Current AIMD-tuned in-flight limit */
    int batch_threshold;       /**< Current AIMD-tuned batch threshold */
    double p99_latency_ms;     /**< Current P99 latency for this ring (ms) */
    double throughput_bps;     /**< Current throughput for this ring (bytes/sec) */
    int aimd_phase;            /**< Current AIMD phase (see AURA_PHASE_* constants) */
    int queue_depth;           /**< Maximum queue depth */
    uint32_t _reserved[4];     /**< Reserved for future use; must be zero */
} aura_ring_stats_t;

#ifdef __cplusplus
static_assert(sizeof(aura_ring_stats_t) == 72, "aura_ring_stats_t ABI size changed");
#else
_Static_assert(sizeof(aura_ring_stats_t) == 72, "aura_ring_stats_t ABI size changed");
#endif

/** AIMD controller phase constants for aura_ring_stats_t.aimd_phase */
#define AURA_PHASE_BASELINE 0    /**< Collecting baseline latency */
#define AURA_PHASE_PROBING 1     /**< Increasing in-flight limit */
#define AURA_PHASE_STEADY 2      /**< Maintaining optimal config */
#define AURA_PHASE_BACKOFF 3     /**< Reducing due to latency spike */
#define AURA_PHASE_SETTLING 4    /**< Waiting for metrics to stabilize */
#define AURA_PHASE_CONVERGED 5   /**< Tuning complete */
#define AURA_PHASE_PASSTHROUGH 6 /**< No AIMD gating (low-pressure default) */

/**
 * Latency histogram snapshot
 *
 * An approximate snapshot of the active latency histogram for a ring.
 * Tracks latencies from 0 to max_tracked_us using tiered bucket widths.
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
 * Retrieved via aura_get_histogram().
 */
#define AURA_HISTOGRAM_BUCKETS 320
#define AURA_HISTOGRAM_TIER_COUNT 4
#define AURA_HISTOGRAM_MAX_US 100000

typedef struct {
    uint32_t buckets[AURA_HISTOGRAM_BUCKETS]; /**< Latency frequency buckets */
    uint32_t overflow;                        /**< Count of operations exceeding max_tracked_us */
    uint32_t total_count;                     /**< Total samples in this snapshot */
    int max_tracked_us;                       /**< Maximum tracked latency in microseconds */
    int tier_count;                           /**< Number of tiers (always 4) */
    int tier_start_us[AURA_HISTOGRAM_TIER_COUNT];    /**< Start of each tier (µs) */
    int tier_width_us[AURA_HISTOGRAM_TIER_COUNT];    /**< Bucket width per tier (µs) */
    int tier_base_bucket[AURA_HISTOGRAM_TIER_COUNT]; /**< First bucket index per tier */
    uint32_t _reserved[2];                           /**< Reserved for future use; must be zero */
} aura_histogram_t;

#ifdef __cplusplus
static_assert(sizeof(aura_histogram_t) == 1352, "aura_histogram_t ABI size changed");
#else
_Static_assert(sizeof(aura_histogram_t) == 1352, "aura_histogram_t ABI size changed");
#endif

/**
 * Buffer pool statistics
 *
 * Retrieved via aura_get_buffer_stats().
 */
typedef struct {
    size_t total_allocated_bytes; /**< Total bytes currently allocated from pool */
    size_t total_buffers;         /**< Total buffer count currently allocated */
    int shard_count;              /**< Number of pool shards */
    uint32_t _reserved[4];        /**< Reserved for future use; must be zero */
} aura_buffer_stats_t;

#ifdef __cplusplus
static_assert(sizeof(aura_buffer_stats_t) == 40, "aura_buffer_stats_t ABI size changed");
#else
_Static_assert(sizeof(aura_buffer_stats_t) == 40, "aura_buffer_stats_t ABI size changed");
#endif

/**
 * Ring selection mode
 *
 * Controls how submissions are distributed across io_uring rings.
 */
typedef enum {
    AURA_SELECT_ADAPTIVE = 0, /**< CPU-local with power-of-two spilling (default).
                                     Stays on the CPU-local ring when uncongested.
                                     When local ring exceeds 75% of its in-flight limit
                                     and load is within 2x of the global average (broad
                                     pressure), spills to the lighter of two randomly
                                     chosen non-local rings. */
    AURA_SELECT_CPU_LOCAL,    /**< CPU-affinity only. Each thread submits to the
                                     ring matching its current CPU (sched_getcpu).
                                     Best cache locality and NUMA friendliness.
                                     Single-threaded workloads use only one ring. */
    AURA_SELECT_ROUND_ROBIN,  /**< Atomic round-robin across all rings. Maximum
                                     single-thread scaling. Best for benchmarks or
                                     single-thread event loops. */
    AURA_SELECT_THREAD_LOCAL  /**< Thread-local ring ownership. Each thread claims
                                     a ring on first submission and reuses it exclusively.
                                     Rings use SINGLE_ISSUER for kernel optimization.
                                     Eliminates all mutex contention and eventfd overhead.
                                     Best for fixed thread pools where each thread does
                                     its own submit+poll (database backends, web servers).
                                     Not suitable for dynamic thread pools or cross-thread
                                     completion harvesting. */
} aura_ring_select_t;

/**
 * Engine configuration options
 *
 * Used with aura_create_with_options() for custom configuration.
 * Initialize with aura_options_init() before modifying.
 */
typedef struct {
    size_t struct_size;        /**< Set by aura_options_init(); for ABI
                                  forward-compatibility */
    int queue_depth;           /**< Queue depth per ring (0 = default: 1024) */
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
    aura_ring_select_t ring_select; /**< Ring selection mode (default: ADAPTIVE) */

    /* Performance */
    bool single_thread; /**< Skip ring mutexes (caller guarantees single-thread access) */

    int batch_threshold; /**< Batch flush threshold: -1 = auto-tune (default),
                              0 = never auto-flush, >0 = fixed threshold */

    uint32_t _reserved[6]; /**< Reserved for future use; must be zero */
} aura_options_t;

#ifdef __cplusplus
static_assert(sizeof(aura_options_t) == 88, "aura_options_t ABI size changed");
#else
_Static_assert(sizeof(aura_options_t) == 88, "aura_options_t ABI size changed");
#endif

/* ============================================================================
 * AURA Flag Constants
 *
 * Wrappers around kernel flags.  Using AURA_* constants instead of raw
 * kernel defines lets the library validate inputs and prevents breakage
 * if new kernel capabilities appear that AURA doesn't yet handle.
 * ============================================================================ */

/** @name Fsync flags (for aura_fsync) */
/**@{*/
#define AURA_FSYNC_DEFAULT 0  /**< Full fsync (metadata + data) */
#define AURA_FSYNC_DATASYNC 1 /**< fdatasync (data only, skip metadata if possible) */
/**@}*/

/** @name Fallocate modes (for aura_fallocate) */
/**@{*/
#define AURA_FALLOC_DEFAULT 0x00    /**< Default: allocate and extend size */
#define AURA_FALLOC_KEEP_SIZE 0x01  /**< Allocate space without changing file size */
#define AURA_FALLOC_PUNCH_HOLE 0x02 /**< Deallocate space (must combine with KEEP_SIZE) */
#define AURA_FALLOC_COLLAPSE 0x08   /**< Remove a range and collapse file */
#define AURA_FALLOC_ZERO 0x10       /**< Zero a range without deallocating */
#define AURA_FALLOC_INSERT 0x20     /**< Insert space, shifting existing data */
/**@}*/

/** @name sync_file_range flags (for aura_sync_file_range) */
/**@{*/
#define AURA_SYNC_RANGE_WAIT_BEFORE 0x01 /**< Wait for prior writeout to complete */
#define AURA_SYNC_RANGE_WRITE 0x02       /**< Initiate writeout of dirty pages */
#define AURA_SYNC_RANGE_WAIT_AFTER 0x04  /**< Wait for writeout to complete */
/**@}*/

/** @name Statx lookup flags (for aura_statx flags parameter)
 *
 * These are AT_* flags passed in the `flags` parameter, NOT field masks.
 * Do not OR these with AURA_STATX_* field masks — they are separate parameters.
 */
/**@{*/
#define AURA_AT_SYMLINK_NOFOLLOW 0x100 /**< Don't follow symlinks */
#define AURA_AT_EMPTY_PATH 0x1000      /**< Operate on fd itself (pathname="") */
/* Backwards-compatible aliases (deprecated — use AURA_AT_* instead) */
#define AURA_STATX_SYMLINK_NOFOLLOW AURA_AT_SYMLINK_NOFOLLOW
#define AURA_STATX_EMPTY_PATH AURA_AT_EMPTY_PATH
/**@}*/

/** @name Statx field mask (for aura_statx mask parameter) */
/**@{*/
#define AURA_STATX_TYPE 0x01U    /**< Request stx_type (file type) */
#define AURA_STATX_MODE 0x02U    /**< Request stx_mode */
#define AURA_STATX_NLINK 0x04U   /**< Request stx_nlink */
#define AURA_STATX_UID 0x08U     /**< Request stx_uid */
#define AURA_STATX_GID 0x10U     /**< Request stx_gid */
#define AURA_STATX_ATIME 0x20U   /**< Request stx_atime */
#define AURA_STATX_MTIME 0x40U   /**< Request stx_mtime */
#define AURA_STATX_CTIME 0x80U   /**< Request stx_ctime */
#define AURA_STATX_INO 0x100U    /**< Request stx_ino */
#define AURA_STATX_SIZE 0x200U   /**< Request stx_size */
#define AURA_STATX_BLOCKS 0x400U /**< Request stx_blocks */
#define AURA_STATX_BTIME 0x800U  /**< Request stx_btime (birth/creation time) */
#define AURA_STATX_ALL 0xFFFU    /**< Request all basic fields */
/**@}*/

/** @name Open flags (for aura_openat flags parameter) */
/**@{*/
#define AURA_O_RDONLY 0x0000 /**< Open for reading only */
#define AURA_O_WRONLY 0x0001 /**< Open for writing only */
#define AURA_O_RDWR 0x0002   /**< Open for reading and writing */
#define AURA_O_CREAT 0x0040  /**< Create file if it doesn't exist */
#define AURA_O_TRUNC 0x0200  /**< Truncate file to zero length */
#define AURA_O_APPEND 0x0400 /**< Append to end of file */
#ifdef O_DIRECT
#    define AURA_O_DIRECT O_DIRECT /**< Direct I/O (bypass page cache) */
#else
#    define AURA_O_DIRECT 0 /**< O_DIRECT not available on this platform */
#endif
/**@}*/

/* ============================================================================
 * Buffer Descriptors (Unified Buffer API)
 * ============================================================================
 */

/**
 * Buffer type indicator
 */
typedef enum {
    AURA_BUF_UNREGISTERED = 0, /**< Regular user-provided buffer */
    AURA_BUF_REGISTERED = 1    /**< Pre-registered buffer (zero-copy) */
} aura_buf_type_t;

/**
 * Unified buffer descriptor
 *
 * A small value type that can represent either a regular buffer pointer
 * or a registered buffer reference. Pass by value to avoid heap allocation.
 *
 * Create using helper functions:
 *   aura_buf(ptr)              - regular buffer
 *   aura_buf_fixed(idx, off)   - registered buffer with offset
 *   aura_buf_fixed_idx(idx)    - registered buffer at offset 0
 *
 * Example:
 * @code
 *   // Regular buffer
 *   aura_read(engine, fd, aura_buf(my_ptr), len, offset, cb, ud);
 *
 *   // Registered buffer (after aura_register_buffers())
 *   aura_read(engine, fd, aura_buf_fixed(0, 0), len, offset, cb, ud);
 * @endcode
 */
typedef struct {
    aura_buf_type_t type; /**< Buffer type discriminator */
    union {
        void *ptr; /**< Direct buffer pointer (AURA_BUF_UNREGISTERED) */
        struct {
            int index;     /**< Registered buffer index */
            size_t offset; /**< Offset within registered buffer */
        } fixed;           /**< Registered buffer reference (AURA_BUF_REGISTERED) */
    } u;
} aura_buf_t;

#ifdef __cplusplus
static_assert(sizeof(aura_buf_t) == 24, "aura_buf_t ABI size check");
#else
_Static_assert(sizeof(aura_buf_t) == 24, "aura_buf_t ABI size check");
#endif

/**
 * Create a buffer descriptor for an unregistered (regular) buffer
 *
 * Accepts const void* so callers with const buffers (e.g., for writes)
 * don't need to cast. The internal pointer is stored as void* because
 * read operations require a mutable target buffer.
 *
 * @param ptr Pointer to user-provided buffer
 * @return Buffer descriptor
 */
static inline aura_buf_t aura_buf(const void *ptr) {
    aura_buf_t buf = {AURA_BUF_UNREGISTERED, {0}};
    buf.u.ptr = (void *)ptr;
    return buf;
}

/**
 * Create a buffer descriptor for a registered (fixed) buffer
 *
 * The buffer must have been previously registered with
 * aura_register_buffers().
 *
 * @param index  Index in the registered buffer array
 * @param offset Offset within that buffer (commonly 0)
 * @return Buffer descriptor
 */
static inline aura_buf_t aura_buf_fixed(int index, size_t offset) {
    aura_buf_t buf = {AURA_BUF_UNREGISTERED, {0}};
    if (index < 0) {
        /* Return zeroed AURA_BUF_UNREGISTERED buffer with ptr=NULL.
         * Submitting this to an I/O function will fail with EINVAL. */
        return buf;
    }
    buf.type = AURA_BUF_REGISTERED;
    buf.u.fixed.index = index;
    buf.u.fixed.offset = offset;
    return buf;
}

/**
 * Create a buffer descriptor for a registered buffer at offset 0
 *
 * Convenience function equivalent to aura_buf_fixed(index, 0).
 *
 * @param index Index in the registered buffer array
 * @return Buffer descriptor
 */
static inline aura_buf_t aura_buf_fixed_idx(int index) {
    return aura_buf_fixed(index, 0);
}

/* ============================================================================
 * Options Initialization
 * ============================================================================
 */

/**
 * Initialize options with default values
 *
 * Always call this before modifying individual options.
 * Not thread-safe: operates on the caller's structure.
 *
 * @param options Options structure to initialize
 */
AURA_API void aura_options_init(aura_options_t *options);

/* ============================================================================
 * Lifecycle Functions
 * ============================================================================
 */

/**
 * Create a new async I/O engine with default options
 *
 * Automatically detects CPU cores, creates io_uring rings, and initializes
 * adaptive controllers. One ring is created per CPU core.
 * Thread-safe (no shared mutable state).
 *
 * @return Engine handle, or NULL on failure (errno set to ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_engine_t *aura_create(void);

/**
 * Create a new async I/O engine with custom options
 *
 * Thread-safe (no shared mutable state).
 *
 * @param options Configuration options (initialize with aura_options_init
 * first)
 * @return Engine handle, or NULL on failure (errno set to EINVAL for
 *         invalid options, or ENOMEM for allocation failure)
 */
AURA_API AURA_WARN_UNUSED aura_engine_t *aura_create_with_options(const aura_options_t *options);

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
 *   3. Call aura_destroy()
 *
 * BUFFER POOL NOTE: The engine uses a single internal buffer pool. For optimal
 * performance, use one aura_engine_t per application. If multiple engines
 * are created, only the first engine's buffer pool will benefit from per-thread
 * caching within each thread.
 *
 * Passing NULL is safe (no-op). Passing an already-destroyed engine is
 * undefined behavior.
 *
 * @param engine Engine to destroy (may be NULL)
 */
AURA_API void aura_destroy(aura_engine_t *engine);

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
 * Supports both regular and registered buffers via aura_buf_t descriptor:
 * @code
 *   // Regular buffer
 *   aura_read(engine, fd, aura_buf(ptr), len, offset, cb, ud);
 *
 *   // Registered buffer (after aura_register_buffers())
 *   aura_read(engine, fd, aura_buf_fixed(0, 0), len, offset, cb, ud);
 * @endcode
 *
 * For best performance with O_DIRECT files, use aura_buffer_alloc() to get
 * properly aligned buffers, or use registered buffers for zero-copy I/O.
 *
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param buf       Buffer descriptor (use aura_buf() or aura_buf_fixed())
 * @param len       Number of bytes to read
 * @param offset    File offset to read from
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error with errno set:
 *         - EINVAL:    NULL engine, invalid fd, zero length, or NULL buffer
 *         - EAGAIN:    All rings at capacity. Poll completions and retry.
 *         - ESHUTDOWN: Engine is shutting down or has a fatal error
 *         - ENOENT:    Registered buffer requested but none are registered
 *         - EOVERFLOW: Registered buffer offset+length exceeds buffer bounds
 *         - EBUSY:     Buffer/file unregistration is in progress
 *         - ENOMEM:    No free request slots available
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_read(aura_engine_t *engine, int fd, aura_buf_t buf,
                                                    size_t len, off_t offset,
                                                    aura_submit_flags_t flags,
                                                    aura_callback_t callback, void *user_data);

/**
 * Submit an asynchronous write operation
 *
 * The callback is invoked when the write completes. The buffer must remain
 * valid until the callback is called.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * Supports both regular and registered buffers via aura_buf_t descriptor:
 * @code
 *   // Regular buffer
 *   aura_write(engine, fd, aura_buf(ptr), len, offset, cb, ud);
 *
 *   // Registered buffer (after aura_register_buffers())
 *   aura_write(engine, fd, aura_buf_fixed(0, 0), len, offset, cb, ud);
 * @endcode
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param buf       Buffer descriptor (use aura_buf() or aura_buf_fixed())
 * @param len       Number of bytes to write
 * @param offset    File offset to write to
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error with errno set:
 *         - EINVAL:    NULL engine, invalid fd, zero length, or NULL buffer
 *         - EAGAIN:    All rings at capacity. Poll completions and retry.
 *         - ESHUTDOWN: Engine is shutting down or has a fatal error
 *         - ENOENT:    Registered buffer requested but none are registered
 *         - EOVERFLOW: Registered buffer offset+length exceeds buffer bounds
 *         - EBUSY:     Buffer/file unregistration is in progress
 *         - ENOMEM:    No free request slots available
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_write(aura_engine_t *engine, int fd, aura_buf_t buf,
                                                     size_t len, off_t offset,
                                                     aura_submit_flags_t flags,
                                                     aura_callback_t callback, void *user_data);

/**
 * Submit an asynchronous fsync operation
 *
 * Ensures all previous writes to the file descriptor are flushed to storage.
 * Pass AURA_FSYNC_DEFAULT (0) for a full fsync, or AURA_FSYNC_DATASYNC
 * for fdatasync behavior (data only, skip metadata if possible).
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param fsync_flags Fsync flags (AURA_FSYNC_DEFAULT or AURA_FSYNC_DATASYNC)
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_fsync(aura_engine_t *engine, int fd,
                                                     unsigned int fsync_flags,
                                                     aura_submit_flags_t flags,
                                                     aura_callback_t callback, void *user_data);

/* ============================================================================
 * Lifecycle Metadata Operations
 *
 * Async wrappers for file lifecycle syscalls.  These skip AIMD latency
 * sampling (metadata is not throughput-sensitive) but benefit from io_uring
 * batching and async execution.
 *
 * Minimum kernel versions: openat/close/statx/fallocate 5.6+,
 * sync_file_range 5.2+, ftruncate 6.9+.
 * ============================================================================ */

/**
 * Submit an asynchronous openat operation.
 *
 * Opens a file relative to a directory fd.  The callback receives the new
 * file descriptor as the result (>= 0 on success, negative errno on failure).
 * The pathname must remain valid until the callback fires.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param dirfd     Directory fd (AT_FDCWD for current directory)
 * @param pathname  Path to file (relative to dirfd)
 * @param open_flags Open flags (AURA_O_RDONLY, AURA_O_WRONLY, AURA_O_CREAT, etc.)
 * @param mode      File mode (used when O_CREAT is set)
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Completion callback (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle, or NULL on error (errno set to EINVAL,
 *         ESHUTDOWN, EAGAIN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_openat(aura_engine_t *engine, int dirfd,
                                                      const char *pathname, int open_flags,
                                                      mode_t mode, aura_submit_flags_t flags,
                                                      aura_callback_t callback, void *user_data);

/**
 * Submit an asynchronous close operation.
 *
 * Callback receives 0 on success, negative errno on failure.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        File descriptor to close
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Completion callback (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle, or NULL on error (errno set to EINVAL,
 *         ESHUTDOWN, EAGAIN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_close(aura_engine_t *engine, int fd,
                                                     aura_submit_flags_t flags,
                                                     aura_callback_t callback, void *user_data);

#ifdef __linux__
/**
 * Submit an asynchronous statx operation.
 *
 * Retrieves file metadata into the caller-provided statx buffer.
 * Both pathname and statxbuf must remain valid until the callback.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param dirfd     Directory fd (AT_FDCWD for current directory)
 * @param pathname  Path (relative to dirfd; "" with AT_EMPTY_PATH for fd-based stat)
 * @param statx_flags Lookup flags (AURA_AT_EMPTY_PATH, AURA_AT_SYMLINK_NOFOLLOW)
 * @param mask      Requested fields (AURA_STATX_SIZE, AURA_STATX_MTIME, etc.)
 * @param statxbuf  Output buffer -- kernel writes directly here
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Completion callback (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle, or NULL on error (errno set to EINVAL,
 *         ESHUTDOWN, EAGAIN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_statx(aura_engine_t *engine, int dirfd,
                                                     const char *pathname, int statx_flags,
                                                     unsigned int mask, struct statx *statxbuf,
                                                     aura_submit_flags_t flags,
                                                     aura_callback_t callback, void *user_data);
#endif

/**
 * Submit an asynchronous fallocate operation.
 *
 * Preallocates or deallocates file space.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        File descriptor
 * @param mode      Allocation mode (AURA_FALLOC_DEFAULT, AURA_FALLOC_KEEP_SIZE, etc.)
 * @param offset    Starting byte offset
 * @param len       Length of region
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Completion callback (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle, or NULL on error (errno set to EINVAL,
 *         ESHUTDOWN, EAGAIN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_fallocate(aura_engine_t *engine, int fd, int mode,
                                                         off_t offset, off_t len,
                                                         aura_submit_flags_t flags,
                                                         aura_callback_t callback, void *user_data);

/**
 * Submit an asynchronous ftruncate operation.
 *
 * Truncates a file to the specified length.  Requires kernel 6.9+ and
 * liburing >= 2.7.  If liburing lacks support, returns NULL with errno=ENOSYS.
 * If the kernel rejects the SQE, the callback receives -ENOSYS.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        File descriptor
 * @param length    New file length
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Completion callback (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle, or NULL on error (errno set to EINVAL,
 *         ESHUTDOWN, EAGAIN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_ftruncate(aura_engine_t *engine, int fd,
                                                         off_t length, aura_submit_flags_t flags,
                                                         aura_callback_t callback, void *user_data);

/**
 * Submit an asynchronous sync_file_range operation.
 *
 * Syncs a byte range without flushing metadata.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        File descriptor
 * @param offset    Starting byte offset
 * @param nbytes    Number of bytes to sync (0 = to end of file)
 * @param range_flags AURA_SYNC_RANGE_WAIT_BEFORE, AURA_SYNC_RANGE_WRITE, AURA_SYNC_RANGE_WAIT_AFTER
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Completion callback (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle, or NULL on error (errno set to EINVAL,
 *         ESHUTDOWN, EAGAIN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *
aura_sync_file_range(aura_engine_t *engine, int fd, off_t offset, off_t nbytes,
                     unsigned int range_flags, aura_submit_flags_t flags, aura_callback_t callback,
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
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param iov       Array of iovec structures
 * @param iovcnt    Number of elements in iov array
 * @param offset    File offset to read from
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_readv(aura_engine_t *engine, int fd,
                                                     const struct iovec *iov, int iovcnt,
                                                     off_t offset, aura_submit_flags_t flags,
                                                     aura_callback_t callback, void *user_data);

/**
 * Submit an asynchronous vectored write operation
 *
 * Writes from multiple buffers in a single operation (gather write).
 * The iovec array and all buffers must remain valid until callback.
 * Thread-safe: may be called concurrently from multiple threads.
 *
 * @param engine    Engine handle
 * @param fd        Open file descriptor
 * @param iov       Array of iovec structures
 * @param iovcnt    Number of elements in iov array
 * @param offset    File offset to write to
 * @param flags     Submit flags (0 or AURA_FIXED_FILE)
 * @param callback  Function called on completion (may be NULL)
 * @param user_data Passed to callback
 * @return Request handle on success, NULL on error (errno set to EINVAL,
 *         EAGAIN, ESHUTDOWN, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED aura_request_t *aura_writev(aura_engine_t *engine, int fd,
                                                      const struct iovec *iov, int iovcnt,
                                                      off_t offset, aura_submit_flags_t flags,
                                                      aura_callback_t callback, void *user_data);

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
 * Thread-safe: may be called from any thread.
 *
 * @param engine Engine handle (must be the same engine that created the
 * request)
 * @param req    Request to cancel (returned by read/write/fsync functions).
 *               Must belong to @p engine; passing a request from a different
 *               engine is undefined behavior.
 * @return 0 if cancellation was submitted, -1 on error (errno set):
 *         - EINVAL: invalid engine or request handle
 *         - EALREADY: request has already completed
 *         - ESHUTDOWN: engine is shutting down
 *         - ENOMEM: no request slots available for the cancel SQE
 *         - EBUSY: submission queue is full
 *         Note: 0 does not guarantee the operation will be cancelled.
 *         If the operation completed before the cancel reaches the kernel,
 *         the original completion callback fires normally (not with -ECANCELED).
 */
AURA_API AURA_WARN_UNUSED int aura_cancel(aura_engine_t *engine, aura_request_t *req);

/* ============================================================================
 * Request Introspection
 * ============================================================================
 */

/**
 * Check if a request is still pending
 *
 * Thread-safe: may be called from any thread while the request is valid.
 *
 * @note A return value of false means the request has completed or been
 * cancelled, but the completion callback may still be executing.  Do not
 * free resources associated with the request until after the callback
 * has returned.
 *
 * @param req Request handle
 * @return true if still in-flight, false if completed or cancelled
 */
AURA_API bool aura_request_pending(const aura_request_t *req);

/**
 * Get the file descriptor associated with a request
 *
 * Thread-safe: may be called from any thread while the request is valid.
 *
 * @param req Request handle
 * @return File descriptor, or -1 if request is invalid
 */
AURA_API int aura_request_fd(const aura_request_t *req);

/**
 * Get user data associated with a request
 *
 * Thread-safe: may be called from any thread while the request is valid.
 *
 * @param req Request handle
 * @return User data pointer passed to submission function
 */
AURA_API void *aura_request_user_data(const aura_request_t *req);

/**
 * Get the operation type of a request
 *
 * Useful in generic completion handlers to distinguish between operations
 * (e.g., openat returns a new fd, read/write return bytes transferred).
 * Thread-safe: may be called from any thread while the request is valid.
 *
 * @param req Request handle
 * @return Operation type (AURA_OP_READ, AURA_OP_OPENAT, etc.), or -1 if NULL
 */
AURA_API int aura_request_op_type(const aura_request_t *req);

/**
 * Mark a request as linked
 *
 * When a request is linked, the next submission on the same thread will be
 * chained to it via io_uring's IOSQE_IO_LINK mechanism. The chained operation
 * will not start until this one completes successfully. If this operation
 * fails, the chained operation receives -ECANCELED.
 *
 * Linked requests are pinned to the same ring to ensure they share one SQ.
 * Call this between aura_read/write/fsync and the next submission to build
 * a chain. The final operation in a chain should NOT be marked as linked.
 *
 * @param req Request handle (returned by aura_read, aura_write, etc.)
 */
AURA_API void aura_request_set_linked(aura_request_t *req);

/**
 * Check if a request is marked as linked
 *
 * @param req Request handle
 * @return true if the request is linked, false otherwise
 */
AURA_API bool aura_request_is_linked(const aura_request_t *req);

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
 * unprocessed completions exist. Call aura_poll() to process completions
 * and clear the readable state. Compatible with epoll (EPOLLIN),
 * poll (POLLIN), and select.
 *
 * Thread-safe: the returned fd is valid for the engine's lifetime.
 *
 * @param engine Engine handle
 * @return Pollable fd, or -1 on error (errno set to EINVAL)
 */
AURA_API AURA_WARN_UNUSED int aura_get_poll_fd(aura_engine_t *engine);

/**
 * Process completed operations (non-blocking)
 *
 * Checks for completions without blocking and invokes callbacks for any
 * completed operations.
 *
 * THREADING MODEL: aura_poll(), aura_wait(), and aura_run() must
 * NOT be called concurrently on the same engine. These functions are
 * designed for single-threaded event loop patterns. Multiple threads
 * may submit I/O concurrently, but only one thread should poll/wait.
 *
 * @param engine Engine handle
 * @return Number of completions processed
 */
AURA_API int aura_poll(aura_engine_t *engine);

/**
 * Wait for at least one completion
 *
 * Blocks until at least one operation completes or timeout expires.
 * Must NOT be called concurrently with aura_poll() or aura_run()
 * on the same engine (see aura_poll() threading model note).
 *
 * @param engine     Engine handle
 * @param timeout_ms Maximum wait time in milliseconds (-1 = forever, 0 = don't
 * block)
 * @return Number of completions processed (>0), 0 if nothing is pending,
 *         or -1 on error. On timeout with pending operations, returns -1
 *         with errno set to ETIMEDOUT.
 */
AURA_API int aura_wait(aura_engine_t *engine, int timeout_ms);

/**
 * Force-flush all pending SQEs across all rings
 *
 * Submits any queued SQEs that have not yet been submitted to the kernel.
 * Normally flushing happens automatically when the batch threshold is reached
 * or during aura_poll/aura_wait. This function is an escape hatch for when
 * you need to ensure SQEs (especially linked chains) are submitted immediately.
 *
 * Thread-safe: may be called from any thread.
 *
 * @param engine Engine handle
 * @return 0 on success, -1 on error (errno set to EINVAL)
 */
AURA_API int aura_flush(aura_engine_t *engine);

/**
 * Run event loop until stopped
 *
 * Blocks, continuously processing completions until aura_stop() is called.
 * Useful for dedicating a thread to I/O processing.
 *
 * After aura_stop() is called, drains remaining in-flight I/O with a
 * 10-second timeout. If I/O is still in-flight after the timeout, returns
 * with a warning log. Callers requiring guaranteed completion should use
 * aura_drain() with an explicit timeout after aura_run() returns.
 *
 * Must NOT be called concurrently with aura_poll(), aura_wait(), or
 * aura_drain() on the same engine (see aura_poll() threading model note).
 *
 * @param engine Engine handle
 */
AURA_API void aura_run(aura_engine_t *engine);

/**
 * Signal the event loop to stop
 *
 * Safe to call from any thread, including from within a callback.
 * aura_run() will return after processing current completions.
 *
 * @param engine Engine handle
 */
AURA_API void aura_stop(aura_engine_t *engine);

/**
 * Drain all pending I/O operations
 *
 * Waits until ALL in-flight operations across all rings have completed.
 * Useful for graceful shutdown or synchronization points.
 *
 * Must NOT be called concurrently with aura_poll(), aura_wait(), or
 * aura_run() on the same engine (see aura_poll() threading model note).
 *
 * New submissions are NOT blocked during drain; if other threads submit
 * operations concurrently, drain will process those as well. To guarantee
 * all operations complete, stop submitting from other threads before calling
 * drain (e.g., signal workers to stop, join them, then drain).
 *
 * On timeout, returns -1 with errno=ETIMEDOUT. Operations that completed
 * before the timeout are still processed (callbacks invoked).
 *
 * **When to use which:**
 * - aura_poll()  — non-blocking, for event loops
 * - aura_wait()  — block until at least one completion, for simple loops
 * - aura_drain() — block until ALL completions finish, for shutdown/sync
 *
 * @param engine     Engine handle
 * @param timeout_ms Maximum wait time in milliseconds (-1 = wait forever,
 *                   0 = non-blocking poll)
 * @return Total number of completions processed, or -1 on error/timeout
 *         (errno = ETIMEDOUT if deadline exceeded, EINVAL if engine is NULL)
 */
AURA_API int aura_drain(aura_engine_t *engine, int timeout_ms);

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
AURA_API AURA_WARN_UNUSED void *aura_buffer_alloc(aura_engine_t *engine, size_t size);

/**
 * Return a buffer to the engine's pool
 *
 * The buffer must have been allocated by aura_buffer_alloc() on the same
 * engine. The size is looked up automatically from an internal table.
 * Passing a pointer not allocated by aura_buffer_alloc() is undefined
 * behavior (the pointer will be passed to free() directly).
 *
 * Thread-safe: may be called from any thread.
 *
 * @param engine Engine handle
 * @param buf    Buffer to free (may be NULL)
 */
AURA_API void aura_buffer_free(aura_engine_t *engine, void *buf);

/* ============================================================================
 * Registered Buffers (Advanced)
 * ============================================================================
 *
 * Registered buffers eliminate kernel mapping overhead for small, frequent I/O.
 * After registration, use aura_buf_fixed() with the standard
 * aura_read/write.
 *
 * WHEN TO USE REGISTERED BUFFERS:
 *   - Same buffers reused across 1000+ I/O operations
 *   - High-frequency small I/O (< 16KB) where mapping overhead is significant
 *   - Zero-copy is critical for performance
 *
 * WHEN TO USE REGULAR BUFFERS (aura_buf(ptr)):
 *   - One-off or infrequent operations
 *   - Short-lived buffers or dynamic buffer count
 *   - Simpler code without registration lifecycle
 *
 * Usage:
 *   struct iovec iovs[2] = {{buf1, 4096}, {buf2, 4096}};
 *   aura_register_buffers(engine, iovs, 2);
 *   aura_read(engine, fd, aura_buf_fixed(0, 0), 4096, offset, callback,
 * ud);
 *   // buffer index 0 refers to buf1
 */

/**
 * Register buffers with the kernel for fixed buffer I/O
 *
 * Pre-registers buffers with the kernel to eliminate mapping overhead.
 * Call this once at startup, not per-operation. After registration,
 * use aura_buf_fixed() to reference buffers by index.
 *
 * Submissions using aura_buf_fixed() fail with errno=EBUSY while a deferred
 * unregister is draining. Use aura_request_unregister() from callback
 * contexts, or aura_unregister() for a synchronous wait.
 *
 * Not thread-safe with other registration operations. I/O submissions
 * may proceed concurrently.
 *
 * @param engine Engine handle
 * @param iovs   Array of iovec describing buffers to register
 * @param count  Number of buffers
 * @return 0 on success, -1 on error (errno set to EINVAL, EBUSY if
 *         already registered, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED int aura_register_buffers(aura_engine_t *engine, const struct iovec *iovs,
                                                    unsigned int count);

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
 * (aura_read/aura_write/readv/writev/fsync) automatically use the
 * registered-file table when the submitted fd is found there.
 *
 * Not thread-safe with other registration operations. I/O submissions
 * may proceed concurrently.
 *
 * @param engine Engine handle
 * @param fds    Array of file descriptors to register
 * @param count  Number of file descriptors
 * @return 0 on success, -1 on error (errno set to EINVAL, EBUSY if
 *         already registered, or ENOMEM)
 */
AURA_API AURA_WARN_UNUSED int aura_register_files(aura_engine_t *engine, const int *fds,
                                                  unsigned int count);

/**
 * Update a registered file descriptor
 *
 * Replaces a previously registered fd at the given index.
 * Use -1 to unregister a slot without replacing it.
 *
 * PARTIAL UPDATE WARNING: This function updates each io_uring ring
 * sequentially. On failure, the library attempts to roll back already-updated
 * rings to the old fd. If the rollback itself fails (rare kernel error), the
 * file table may be inconsistent across rings. Recovery: call
 * aura_unregister(engine, AURA_REG_FILES) and re-register.
 *
 * Not thread-safe with other registration operations.
 *
 * @param engine Engine handle
 * @param index  Index in registered file array
 * @param fd     New file descriptor (-1 to unregister slot)
 * @return 0 on success, -1 on error (errno set to EINVAL, ENOENT if
 *         files not registered, or EBUSY if unregister pending;
 *         state may be inconsistent on kernel error)
 */
AURA_API AURA_WARN_UNUSED int aura_update_file(aura_engine_t *engine, int index, int fd);

/* ============================================================================
 * Unified Registration Lifecycle
 * ============================================================================
 */

/**
 * Registration resource type
 *
 * Identifies the type of registered resource for aura_unregister() and
 * aura_request_unregister().
 */
typedef enum {
    AURA_REG_BUFFERS = 0, /**< Registered buffers (see aura_register_buffers) */
    AURA_REG_FILES = 1    /**< Registered files (see aura_register_files) */
} aura_reg_type_t;

/**
 * Unregister previously registered buffers or files (synchronous)
 *
 * For non-callback callers, this waits until in-flight operations using
 * registered resources drain and unregister completes. If called from a
 * completion callback, it automatically degrades to the deferred
 * (non-blocking) path — equivalent to aura_request_unregister().
 *
 * Thread-safe: safe to call from any thread, but must NOT be called
 * concurrently with aura_poll/aura_wait/aura_run on the same engine
 * (it calls aura_wait internally to drain in-flight operations).
 *
 * @param engine Engine handle
 * @param type   Resource type (AURA_REG_BUFFERS or AURA_REG_FILES)
 * @return 0 on success, -1 on error (errno set to EINVAL, ETIMEDOUT)
 */
AURA_API int aura_unregister(aura_engine_t *engine, aura_reg_type_t type);

/**
 * Request deferred unregister (callback-safe)
 *
 * Marks registered resources as draining. If no in-flight fixed-buffer/file
 * operations remain, the unregistration completes synchronously (acquires
 * reg_lock and makes a blocking kernel call). Otherwise, unregistration is
 * deferred until in-flight operations drain via aura_poll/aura_wait.
 *
 * Thread-safe: safe to call from completion callbacks or any thread.
 *
 * @note Despite being callback-safe, this function may block briefly when
 * it completes the unregistration synchronously.  It will NOT block waiting
 * for in-flight operations to complete.
 *
 * @param engine Engine handle
 * @param type   Resource type (AURA_REG_BUFFERS or AURA_REG_FILES)
 * @return 0 on success, -1 on error (errno set to EINVAL)
 */
AURA_API int aura_request_unregister(aura_engine_t *engine, aura_reg_type_t type);

/* ============================================================================
 * Statistics (Optional)
 * ============================================================================
 */

/**
 * Get current engine statistics
 *
 * Retrieves throughput, latency, and tuning parameters for monitoring.
 *
 * The stats_size parameter enables forward compatibility: pass
 * sizeof(aura_stats_t) so the library writes at most that many bytes.
 * Code compiled against an older header with a smaller struct will
 * receive fewer fields without buffer overruns.
 *
 * Thread-safe: reads atomic counters and locks each ring briefly.
 *
 * @param engine     Engine handle
 * @param stats      Output statistics structure
 * @param stats_size sizeof(aura_stats_t) from caller's compilation
 * @return 0 on success, -1 on error (errno set to EINVAL if engine, stats,
 *         or stats_size is invalid)
 */
AURA_API int aura_get_stats(const aura_engine_t *engine, aura_stats_t *stats, size_t stats_size);

/**
 * Get the number of io_uring rings in the engine
 *
 * Thread-safe: returns a constant set at engine creation.
 *
 * @param engine Engine handle
 * @return Number of rings, or 0 if engine is NULL
 */
AURA_API int aura_get_ring_count(const aura_engine_t *engine);

/**
 * Get statistics for a specific ring
 *
 * Thread-safe: locks the ring's mutex during the read.
 * If engine or stats is NULL, returns -1 without modifying stats.
 * If ring_idx is out of range, returns -1 and zeroes stats.
 *
 * @param engine     Engine handle
 * @param ring_idx   Ring index (0 to aura_get_ring_count()-1)
 * @param stats      Output statistics structure
 * @param stats_size sizeof(aura_ring_stats_t) from caller's compilation
 * @return 0 on success, -1 on error (NULL engine/stats or invalid ring_idx)
 */
AURA_API int aura_get_ring_stats(const aura_engine_t *engine, int ring_idx,
                                 aura_ring_stats_t *stats, size_t stats_size);

/**
 * Get a latency histogram snapshot for a specific ring
 *
 * Copies the current active histogram. The snapshot is approximate —
 * see aura_histogram_t documentation for details.
 *
 * Thread-safe: locks the ring's mutex during the copy.
 * If engine or hist is NULL, returns -1 without modifying hist.
 * If ring_idx is out of range, returns -1 and zeroes hist.
 *
 * The hist_size parameter enables forward compatibility: pass
 * sizeof(aura_histogram_t) so the library writes at most that many bytes.
 *
 * @param engine    Engine handle
 * @param ring_idx  Ring index (0 to aura_get_ring_count()-1)
 * @param hist      Output histogram structure
 * @param hist_size sizeof(aura_histogram_t) from caller's compilation
 * @return 0 on success, -1 on error (NULL engine/hist or invalid ring_idx)
 */
AURA_API int aura_get_histogram(const aura_engine_t *engine, int ring_idx, aura_histogram_t *hist,
                                size_t hist_size);

/**
 * Get buffer pool statistics
 *
 * Thread-safe: reads atomic counters from the buffer pool.
 * If engine or stats is NULL, returns -1 without modifying stats.
 *
 * The stats_size parameter enables forward compatibility: pass
 * sizeof(aura_buffer_stats_t) so the library writes at most that many bytes.
 *
 * @param engine     Engine handle
 * @param stats      Output statistics structure
 * @param stats_size sizeof(aura_buffer_stats_t) from caller's compilation
 * @return 0 on success, -1 on error (NULL engine or stats)
 */
AURA_API int aura_get_buffer_stats(const aura_engine_t *engine, aura_buffer_stats_t *stats,
                                   size_t stats_size);

/**
 * Get human-readable name for an AIMD phase value
 *
 * Valid phase values are 0-6 (AURA_PHASE_BASELINE through
 * AURA_PHASE_PASSTHROUGH).  Returns "UNKNOWN" for out-of-range values.
 * Thread-safe: returns a pointer to a static string.
 *
 * @param phase Phase value (from aura_ring_stats_t.aimd_phase)
 * @return Static string like "BASELINE", "PROBING", etc., or "UNKNOWN"
 */
AURA_API const char *aura_phase_name(int phase);

/**
 * Get library version string
 *
 * Thread-safe: returns a pointer to a static string.
 *
 * @return Version string (e.g., "0.3.0")
 */
AURA_API const char *aura_version(void);

/* ============================================================================
 * Diagnostics
 * ============================================================================
 */

/**
 * Check if the engine has a fatal error
 *
 * Once a fatal error is latched (e.g., io_uring ring fd becomes invalid),
 * all subsequent submissions fail with ESHUTDOWN. Use this to distinguish
 * a permanently broken engine from transient EAGAIN.
 *
 * Thread-safe.
 *
 * @param engine Engine handle
 * @return 0 if healthy, positive errno value if fatally broken, -1 if
 *         engine is NULL (errno set to EINVAL)
 */
AURA_API int aura_get_fatal_error(const aura_engine_t *engine);

/**
 * Check if the current thread is inside a completion callback
 *
 * Useful for libraries building on AuraIO to choose between synchronous
 * and deferred code paths (e.g., aura_unregister vs aura_request_unregister).
 *
 * Thread-safe (uses thread-local state).
 *
 * @return true if the calling thread is currently inside a completion
 *         callback, false otherwise
 */
AURA_API bool aura_in_callback_context(void);

/**
 * Compute a latency percentile from a histogram snapshot
 *
 * Iterates histogram buckets to find the requested percentile value.
 * The histogram must have been obtained via aura_get_histogram().
 *
 * @param hist       Histogram snapshot
 * @param percentile Percentile to compute (0.0 to 100.0, e.g. 99.0 for p99)
 * @return Latency in milliseconds (consistent with p99_latency_ms in stats),
 *         or -1.0 if histogram is empty or percentile is out of range
 */
AURA_API double aura_histogram_percentile(const aura_histogram_t *hist, double percentile);

/**
 * Get the upper bound of a histogram bucket in microseconds
 *
 * Uses the tier metadata in the histogram to compute the upper bound
 * for the given bucket index. Needed by Prometheus/OTel integrations
 * that must emit per-bucket boundary values.
 *
 * @param hist   Histogram snapshot (must have tier metadata populated)
 * @param bucket Bucket index (0 to AURA_HISTOGRAM_BUCKETS-1)
 * @return Upper bound in microseconds, or 0 if bucket is out of range
 */
AURA_API int aura_histogram_bucket_upper_bound_us(const aura_histogram_t *hist, int bucket);

/* ============================================================================
 * Logging (Optional)
 * ============================================================================
 */

/** Log severity levels (match syslog priorities). */
#define AURA_LOG_ERR 3    /**< Error (matches syslog LOG_ERR) */
#define AURA_LOG_WARN 4   /**< Warning (matches syslog LOG_WARNING) */
#define AURA_LOG_NOTICE 5 /**< Notice (matches syslog LOG_NOTICE) */
#define AURA_LOG_INFO 6   /**< Informational (matches syslog LOG_INFO) */
#define AURA_LOG_DEBUG 7  /**< Debug (matches syslog LOG_DEBUG) */

/**
 * Log callback type
 *
 * The callback may be invoked concurrently from multiple threads.
 * Implementations MUST be thread-safe (e.g. use a mutex, write to
 * a thread-safe logger, or only call async-signal-safe functions).
 *
 * @param level   Severity (AURA_LOG_ERR .. AURA_LOG_DEBUG; matches syslog)
 * @param msg     Formatted message string (NUL-terminated, max 256 bytes
 *                including NUL; longer messages are truncated by the library)
 * @param userdata Opaque pointer passed to aura_set_log_handler()
 */
typedef void (*aura_log_fn)(int level, const char *msg, void *userdata);

/**
 * Set the library-wide log handler
 *
 * By default no handler is installed and the library is silent.
 * The handler is global (process-wide) and may be called from any thread
 * (including internal tick threads and io_uring completion threads).
 * The handler MUST be thread-safe.
 *
 * @param handler  Log callback (NULL to disable logging).  Must be
 *                 safe to call concurrently from multiple threads.
 * @param userdata Opaque pointer forwarded to the callback
 */
AURA_API void aura_set_log_handler(aura_log_fn handler, void *userdata);

/**
 * Emit a log message through the registered handler
 *
 * No-op when no handler is registered.  Thread-safe.
 * Messages are formatted into a 256-byte internal buffer (truncated if longer).
 *
 * @param level  Severity (AURA_LOG_ERR .. AURA_LOG_DEBUG)
 * @param fmt    printf-style format string
 * @param ...    Format arguments
 */
AURA_API void aura_log_emit(int level, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/**
 * Get library version as integer
 *
 * Format: (major * 10000 + minor * 100 + patch)
 * Example: 0.1.0 = 100
 * Thread-safe.
 *
 * @return Version integer
 */
AURA_API int aura_version_int(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_H */
