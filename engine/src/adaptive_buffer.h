// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

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
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>

/** Number of size classes (power-of-2 from 4KB to 128MB) */
#define BUFFER_SIZE_CLASSES 16

/** Maximum capacity of the buffer size map (1M entries) */
#define BUF_MAP_MAX_CAPACITY (1 << 20)

/** Maximum buffer size supported by the pool (128MB) */
#define BUFFER_POOL_MAX_SIZE ((size_t)4096 << (BUFFER_SIZE_CLASSES - 1))

/** Maximum buffers to keep in free list per shard (prevents unbounded memory growth).
 *  With 4 shards this allows caching ~1024 buffers total (~4MB at 4K size class).
 *  Increase from 64 to reduce alloc/free churn through posix_memalign. */
#define BUFFER_POOL_DEFAULT_MAX_FREE_PER_SHARD 256

/** Buffers per size class in thread-local cache (power of 2 for efficient batching).
 *  Larger cache reduces frequency of shard lock + posix_memalign on the hot path.
 *  At 4KB size class, 32 buffers = 128KB per thread — acceptable overhead. */
#define THREAD_CACHE_SIZE 32

/** Batch size when transferring between thread cache and global pool */
#define THREAD_CACHE_BATCH_SIZE 16

/** Maximum pool shards (power of 2). Actual count determined at runtime. */
#define BUFFER_POOL_MAX_SHARDS 256

/** Minimum pool shards (even on single-core systems) */
#define BUFFER_POOL_MIN_SHARDS 2

/**
 * Buffer slot (metadata entry)
 *
 * Represents a single cached buffer in the pool.
 * Slots are pre-allocated to avoid malloc() on the free path.
 */
typedef struct buffer_slot {
    void *buffer;             /**< Aligned buffer pointer */
    size_t actual_size;       /**< Actual buffer size */
    struct buffer_slot *next; /**< Next in bucket or free slot list */
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
    /* Hot fields: all in cache line 0 for fast-path alloc/free.
     * pool+pool_id validated on every access, counts checked immediately after. */
    _Atomic(struct buffer_pool *)
        pool;         /**< Parent pool (for slow path, atomic for cross-thread visibility) */
    uint64_t pool_id; /**< Pool generation ID (detect stale cache) */
    struct thread_cache *next;       /**< Next in pool's cache list (for cleanup) */
    pthread_t owner_thread;          /**< Thread that created this cache */
    int shard_id;                    /**< Assigned shard for slow-path operations */
    int counts[BUFFER_SIZE_CLASSES]; /**< Buffer count per size class */

    /* Cold: large array accessed only after counts check passes */
    void *buffers[BUFFER_SIZE_CLASSES][THREAD_CACHE_SIZE]; /**< Cached buffers */

    /* Cold: cleanup coordination (accessed only during destroy/thread exit) */
    pthread_mutex_t
        cleanup_mutex;  /**< Coordinates buffer cleanup between pool destroy and TLS destructor */
    bool cleaned_up;    /**< Buffers already flushed/freed */
    bool thread_exited; /**< Thread ran its TLS destructor */
} thread_cache_t;

/**
 * Pool shard
 *
 * Each shard has its own lock and bucket list to reduce contention.
 * With 256 shards, even 1000+ cores only have ~4 threads per shard on average.
 */
typedef struct {
    pthread_mutex_t lock;                             /**< Shard lock */
    buffer_slot_t *size_buckets[BUFFER_SIZE_CLASSES]; /**< Buckets by size class */
    buffer_slot_t *slot_pool;                         /**< Pre-allocated slot array */
    buffer_slot_t *free_slots;                        /**< Available slots for reuse */
    int slot_pool_size;                               /**< Total slots in shard */
    int free_count;                                   /**< Buffers currently cached */
    int max_free_count;                               /**< High-water mark per shard */
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
 * - 256 cores → 64 shards
 * - 1024 cores → 256 shards (max)
 */
typedef struct buffer_pool {
    /* === CL 0: Read-only after init (no contention) === */
    buffer_shard_t *shards; /**< Dynamically allocated shards */
    int shard_count;        /**< Number of shards (power of 2) */
    int shard_mask;         /**< shard_count - 1 for fast modulo */
    uint64_t pool_id;       /**< Unique generation ID (set once at init) */
    size_t alignment;       /**< Buffer alignment (typically 4096) */

    /* === CL 1: Read-heavy atomics (get_thread_cache hot path) === */
    _Alignas(64) _Atomic(thread_cache_t *) thread_caches; /**< Lock-free list of thread caches */
    _Atomic bool destroyed;                               /**< Pool destroyed flag for safety */
    _Atomic int registrations_inflight; /**< Caches currently in registration path */
    _Atomic int active_users;           /**< Threads currently in shard slow path (alloc/free) */

    /* === CL 2: Write-heavy counters (slow path only) === */
    _Alignas(64) _Atomic size_t total_allocated; /**< Total bytes allocated (atomic) */
    _Atomic size_t total_buffers;                /**< Total buffer count (atomic) */
    _Atomic int next_shard;                      /**< Round-robin shard assignment */
} buffer_pool_t;

/**
 * Initialize buffer pool
 *
 * @param pool      Pool to initialize
 * @param alignment Buffer alignment (typically 4096 for O_DIRECT)
 * @return 0 on success, -1 on error
 */
/**
 * Map buffer size to size-class index (0 to BUFFER_SIZE_CLASSES-1)
 */
int size_to_class(size_t size);

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

/* ============================================================================
 * Buffer Size Map (ptr → size_class lookup)
 *
 * Open-addressing hash table that tracks the size class of each
 * buffer allocated through the pool.  This lets buffer_pool_free_tracked()
 * work without requiring the caller to pass the size.
 * ============================================================================ */

/** Initial capacity (must be power of 2) */
#define BUF_MAP_INITIAL_CAPACITY 1024

/** Load factor threshold for growth (75%) */
#define BUF_MAP_LOAD_FACTOR_NUM 3
#define BUF_MAP_LOAD_FACTOR_DEN 4

typedef struct {
    uintptr_t key;     /**< ptr value, 0 = empty slot */
    uint8_t class_idx; /**< Size class index (0-15) */
} buf_map_entry_t;

typedef struct {
    buf_map_entry_t *entries;
    size_t capacity;      /**< Always a power of 2 */
    size_t count;         /**< Number of live entries (protected by lock) */
    pthread_mutex_t lock; /**< Single lock (striped locks are unsound with open-addressing) */
} buf_size_map_t;

int buf_size_map_init(buf_size_map_t *map);
void buf_size_map_destroy(buf_size_map_t *map);
int buf_size_map_insert(buf_size_map_t *map, void *ptr, int class_idx);
int buf_size_map_remove(buf_size_map_t *map, void *ptr);

/**
 * Free buffer back to pool (size looked up automatically)
 *
 * @param pool Pool to return to
 * @param map  Size map for ptr→class lookup
 * @param buf  Buffer to free
 */
void buffer_pool_free_tracked(buffer_pool_t *pool, buf_size_map_t *map, void *buf);

#endif /* ADAPTIVE_BUFFER_H */
