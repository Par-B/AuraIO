/**
 * @file adaptive_buffer.h
 * @brief Aligned buffer pool for O_DIRECT I/O
 *
 * Internal header - not part of public API.
 *
 * Uses size-class buckets for O(1) allocation and pre-allocated
 * metadata slots to eliminate malloc() on the free path.
 */

#ifndef ADAPTIVE_BUFFER_H
#define ADAPTIVE_BUFFER_H

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/** Number of size classes (power-of-2 from 4KB to 128MB) */
#define BUFFER_SIZE_CLASSES 16

/** Maximum buffer size supported by the pool (128MB) */
#define BUFFER_POOL_MAX_SIZE ((size_t)4096 << (BUFFER_SIZE_CLASSES - 1))

/** Maximum buffers to keep in free list per shard (prevents unbounded memory growth).
 *  With 4 shards this allows caching ~1024 buffers total (~4MB at 4K size class).
 *  Increase from 64 to reduce alloc/free churn through posix_memalign. */
#define BUFFER_POOL_DEFAULT_MAX_FREE_PER_SHARD 256

/** Buffers per size class in thread-local cache (power of 2 for efficient batching) */
#define THREAD_CACHE_SIZE 16

/** Batch size when transferring between thread cache and global pool */
#define THREAD_CACHE_BATCH_SIZE 8

/** Maximum pool shards (power of 2). Actual count determined at runtime. */
#define BUFFER_POOL_MAX_SHARDS 64

/** Minimum pool shards (even on single-core systems) */
#define BUFFER_POOL_MIN_SHARDS 2

/**
 * Buffer slot (metadata entry)
 *
 * Represents a single cached buffer in the pool.
 * Slots are pre-allocated to avoid malloc() on the free path.
 */
typedef struct buffer_slot {
    void *buffer;               /**< Aligned buffer pointer */
    size_t actual_size;         /**< Actual buffer size */
    struct buffer_slot *next;   /**< Next in bucket or free slot list */
} buffer_slot_t;

/* Forward declaration */
struct buffer_pool;

/**
 * Per-thread buffer cache
 *
 * Each thread gets its own cache to avoid lock contention.
 * The cache holds a small number of buffers per size class.
 * Operations on the thread cache are lock-free.
 */
typedef struct thread_cache {
    struct buffer_pool *pool;   /**< Parent pool (for slow path) */
    int shard_id;               /**< Assigned shard for slow-path operations */
    void *buffers[BUFFER_SIZE_CLASSES][THREAD_CACHE_SIZE]; /**< Cached buffers */
    int counts[BUFFER_SIZE_CLASSES];  /**< Buffer count per size class */
    struct thread_cache *next;  /**< Next in pool's cache list (for cleanup) */
} thread_cache_t;

/**
 * Pool shard
 *
 * Each shard has its own lock and bucket list to reduce contention.
 * With 64 shards, even 500 cores only have ~8 threads per shard on average.
 */
typedef struct {
    pthread_mutex_t lock;                           /**< Shard lock */
    buffer_slot_t *size_buckets[BUFFER_SIZE_CLASSES]; /**< Buckets by size class */
    buffer_slot_t *slot_pool;                       /**< Pre-allocated slot array */
    buffer_slot_t *free_slots;                      /**< Available slots for reuse */
    int slot_pool_size;                             /**< Total slots in shard */
    int free_count;                                 /**< Buffers currently cached */
    int max_free_count;                             /**< High-water mark per shard */
} buffer_shard_t;

/**
 * Buffer pool
 *
 * Thread-safe pool of aligned buffers with O(1) allocation.
 * Buffers are organized into size-class buckets for fast lookup.
 *
 * Scalability design:
 * - Per-thread caches: fast path with no locks
 * - Sharded global pool: shard count scales with CPU count
 * - Lock-free cache registration: atomic operations for thread cache list
 *
 * Auto-scaling:
 * - 4 cores  → 2 shards  (minimal overhead)
 * - 16 cores → 4 shards
 * - 64 cores → 16 shards
 * - 256 cores → 64 shards (max)
 */
typedef struct buffer_pool {
    buffer_shard_t *shards;                         /**< Dynamically allocated shards */
    int shard_count;                                /**< Number of shards (power of 2) */
    int shard_mask;                                 /**< shard_count - 1 for fast modulo */
    size_t alignment;                               /**< Buffer alignment (typically 4096) */
    _Atomic size_t total_allocated;                 /**< Total bytes allocated (atomic) */
    _Atomic size_t total_buffers;                   /**< Total buffer count (atomic) */
    _Atomic(thread_cache_t *) thread_caches;        /**< Lock-free list of thread caches */
    _Atomic int next_shard;                         /**< Round-robin shard assignment */
    _Atomic bool destroyed;                         /**< Pool destroyed flag for safety */
} buffer_pool_t;

/**
 * Initialize buffer pool
 *
 * @param pool      Pool to initialize
 * @param alignment Buffer alignment (typically 4096 for O_DIRECT)
 * @return 0 on success, -1 on error
 */
int buffer_pool_init(buffer_pool_t *pool, size_t alignment);

/**
 * Destroy buffer pool
 *
 * Frees all buffers in the pool.
 *
 * @param pool Pool to destroy
 */
void buffer_pool_destroy(buffer_pool_t *pool);

/**
 * Allocate aligned buffer
 *
 * Returns a buffer from the appropriate size-class bucket if available,
 * otherwise allocates a new aligned buffer.
 *
 * @param pool Pool to allocate from
 * @param size Buffer size
 * @return Aligned buffer, or NULL on failure
 */
void *buffer_pool_alloc(buffer_pool_t *pool, size_t size);

/**
 * Free buffer back to pool
 *
 * Returns the buffer to the appropriate size-class bucket for reuse.
 * Does not call malloc() - uses pre-allocated metadata slots.
 *
 * @param pool Pool to return to
 * @param buf  Buffer to free
 * @param size Buffer size (must match allocation size)
 */
void buffer_pool_free(buffer_pool_t *pool, void *buf, size_t size);

#endif /* ADAPTIVE_BUFFER_H */
