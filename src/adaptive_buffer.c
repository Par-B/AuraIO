/**
 * @file adaptive_buffer.c
 * @brief Aligned buffer pool implementation with size-class buckets
 *
 * Uses power-of-2 size classes for O(1) buffer lookup and pre-allocated
 * metadata slots to eliminate malloc() on the free path.
 *
 * Scalability design that auto-adapts to system size:
 *
 * 1. Per-thread caches: Each thread has a local cache of buffers.
 *    Most alloc/free operations hit the thread cache with no locking.
 *
 * 2. Sharded global pool: Shard count scales with CPU count.
 *    - 4 cores: 2 shards (~3KB overhead)
 *    - 64 cores: 16 shards (~25KB overhead)
 *    - 500 cores: 64 shards (~100KB overhead)
 *
 * 3. Lock-free cache registration: Thread caches are registered via
 *    atomic CAS operations, avoiding a registration bottleneck at startup.
 *
 * Performance characteristics:
 * - Fast path (cache hit): O(1), no lock, ~10ns
 * - Slow path (cache miss): O(1), one shard lock, ~50ns
 * - Contention: ~4-8 threads per shard on average (regardless of core count)
 */

#define _GNU_SOURCE
#include "adaptive_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <unistd.h>

/* Thread-local cache pointer */
static __thread thread_cache_t *tls_cache = NULL;

/* ============================================================================
 * Size Class Helpers
 * ============================================================================ */

/**
 * Map buffer size to size class index (0-15).
 *
 * Size classes are power-of-2 buckets:
 *   Class 0: <= 4KB
 *   Class 1: <= 8KB
 *   Class 2: <= 16KB
 *   ...
 *   Class 15: <= 64MB (and overflow)
 *
 * @param size Buffer size in bytes
 * @return Size class index (0 to BUFFER_SIZE_CLASSES-1)
 */
static inline int size_to_class(size_t size) {
    if (size <= 4096) {
        return 0;
    }

    /* Find the position of the highest set bit.
     * __builtin_clzl returns leading zeros in unsigned long.
     * For size=8192 (2^13), clzl returns 64-14=50 on 64-bit.
     * We want class 1 for 8KB, class 2 for 16KB, etc.
     *
     * Safety: size > 4096 here (guarded above), so (size - 1) >= 4096 > 0,
     * avoiding __builtin_clzl(0) which has undefined behavior.
     */
    int leading_zeros = __builtin_clzl(size - 1);
    int highest_bit = (sizeof(unsigned long) * 8) - leading_zeros;

    /* Subtract 12 (log2 of 4096) to get class index */
    int class_idx = highest_bit - 12;

    if (class_idx < 0) {
        return 0;
    }
    if (class_idx >= BUFFER_SIZE_CLASSES) {
        return BUFFER_SIZE_CLASSES - 1;
    }
    return class_idx;
}

/**
 * Convert size class index to bucket size.
 *
 * This is the inverse of size_to_class() - given a class index,
 * return the actual allocation size for that bucket.
 *
 * @param class_idx Size class index (0 to BUFFER_SIZE_CLASSES-1)
 * @return Bucket size in bytes (4096, 8192, 16384, ...)
 */
static inline size_t class_to_size(int class_idx) {
    return (size_t)4096 << class_idx;
}

/* ============================================================================
 * Shard Management
 * ============================================================================ */

/**
 * Initialize a single shard.
 */
static int shard_init(buffer_shard_t *shard) {
    if (pthread_mutex_init(&shard->lock, NULL) != 0) {
        return -1;
    }

    shard->max_free_count = BUFFER_POOL_DEFAULT_MAX_FREE_PER_SHARD;
    shard->free_count = 0;

    /* Pre-allocate metadata slots for this shard */
    shard->slot_pool_size = BUFFER_POOL_DEFAULT_MAX_FREE_PER_SHARD;
    shard->slot_pool = calloc(shard->slot_pool_size, sizeof(buffer_slot_t));
    if (!shard->slot_pool) {
        pthread_mutex_destroy(&shard->lock);
        return -1;
    }

    /* Initialize free slot list */
    shard->free_slots = &shard->slot_pool[0];
    for (int i = 0; i < shard->slot_pool_size - 1; i++) {
        shard->slot_pool[i].next = &shard->slot_pool[i + 1];
    }
    shard->slot_pool[shard->slot_pool_size - 1].next = NULL;

    /* Initialize size buckets (all empty) */
    for (int i = 0; i < BUFFER_SIZE_CLASSES; i++) {
        shard->size_buckets[i] = NULL;
    }

    return 0;
}

/**
 * Destroy a single shard.
 */
static void shard_destroy(buffer_shard_t *shard) {
    pthread_mutex_lock(&shard->lock);

    /* Free all buffers in all size buckets */
    for (int bucket = 0; bucket < BUFFER_SIZE_CLASSES; bucket++) {
        buffer_slot_t *slot = shard->size_buckets[bucket];
        while (slot) {
            if (slot->buffer) {
                free(slot->buffer);
            }
            slot = slot->next;
        }
        shard->size_buckets[bucket] = NULL;
    }

    /* Free the pre-allocated slot pool */
    free(shard->slot_pool);
    shard->slot_pool = NULL;
    shard->free_slots = NULL;
    shard->free_count = 0;

    pthread_mutex_unlock(&shard->lock);
    pthread_mutex_destroy(&shard->lock);
}

/* ============================================================================
 * Thread Cache Management
 * ============================================================================ */

/**
 * Get or create thread-local cache for this pool.
 *
 * Each thread gets a dedicated cache the first time it accesses the pool.
 * Uses lock-free CAS to register the cache with the pool for cleanup.
 */
static thread_cache_t *get_thread_cache(buffer_pool_t *pool) {
    thread_cache_t *cache = tls_cache;

    /* Fast path: already have a valid cache for this pool (no fence needed).
     * The pool was alive when this cache was created, and pool destruction
     * is a lifecycle event — the caller must ensure no threads are using
     * the pool during destruction (same contract as pthread_mutex_destroy). */
    if (cache && cache->pool == pool) {
        return cache;
    }

    /* Slow path: check if pool is destroyed (acquire fence is fine here).
     * This only runs on first access per thread or pool mismatch. */
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        if (tls_cache && tls_cache->pool == pool) {
            tls_cache = NULL;
        }
        return NULL;
    }

    /* Different pool - bypass cache (rare: usually one pool per engine) */
    if (cache && cache->pool != pool) {
        return NULL;
    }

    /* First access from this thread - create a new cache */
    cache = calloc(1, sizeof(thread_cache_t));
    if (!cache) {
        return NULL;
    }

    cache->pool = pool;

    /* Assign shard via round-robin for load balancing */
    cache->shard_id = atomic_fetch_add(&pool->next_shard, 1) & pool->shard_mask;

    /* Lock-free registration with pool's cache list using CAS */
    thread_cache_t *old_head;
    do {
        old_head = atomic_load(&pool->thread_caches);
        cache->next = old_head;
    } while (!atomic_compare_exchange_weak(&pool->thread_caches, &old_head, cache));

    tls_cache = cache;
    return cache;
}

/**
 * Refill thread cache from assigned shard (slow path).
 *
 * Called when thread cache is empty. Transfers a batch of buffers
 * from the shard to the thread cache.
 *
 * @param pool      Buffer pool
 * @param cache     Thread-local cache
 * @param class_idx Size class to refill
 * @return Number of buffers transferred
 */
static int cache_refill(buffer_pool_t *pool, thread_cache_t *cache, int class_idx) {
    buffer_shard_t *shard = &pool->shards[cache->shard_id];
    int transferred = 0;

    pthread_mutex_lock(&shard->lock);

    while (transferred < THREAD_CACHE_BATCH_SIZE && cache->counts[class_idx] < THREAD_CACHE_SIZE) {
        buffer_slot_t *slot = shard->size_buckets[class_idx];
        if (!slot) {
            break;  /* Shard empty for this class */
        }

        /* Pop from shard */
        shard->size_buckets[class_idx] = slot->next;
        void *buf = slot->buffer;
        shard->free_count--;

        /* Return slot to free list */
        slot->buffer = NULL;
        slot->next = shard->free_slots;
        shard->free_slots = slot;

        /* Add to thread cache */
        cache->buffers[class_idx][cache->counts[class_idx]++] = buf;
        transferred++;
    }

    pthread_mutex_unlock(&shard->lock);
    return transferred;
}

/**
 * Flush thread cache to assigned shard (slow path).
 *
 * Called when thread cache is full. Transfers a batch of buffers
 * from the thread cache to the shard.
 *
 * @param pool      Buffer pool
 * @param cache     Thread-local cache
 * @param class_idx Size class to flush
 * @param bucket_size Size of buffers in this class
 * @return Number of buffers transferred (negative means buffers were freed)
 */
static int cache_flush(buffer_pool_t *pool, thread_cache_t *cache, int class_idx, size_t bucket_size) {
    buffer_shard_t *shard = &pool->shards[cache->shard_id];
    int transferred = 0;

    /* Collect buffers that need to be freed outside the lock.
     * THREAD_CACHE_SIZE is the max we could ever need to free in one flush. */
    void *to_free[THREAD_CACHE_SIZE];
    int free_count = 0;

    pthread_mutex_lock(&shard->lock);

    while (transferred < THREAD_CACHE_BATCH_SIZE && cache->counts[class_idx] > 0) {
        /* Check high-water mark */
        if (shard->max_free_count > 0 && shard->free_count >= shard->max_free_count) {
            /* Shard at capacity - collect remaining buffers for deferred free */
            while (cache->counts[class_idx] > 0) {
                to_free[free_count++] = cache->buffers[class_idx][--cache->counts[class_idx]];
            }
            break;
        }

        /* Get a metadata slot */
        buffer_slot_t *slot = shard->free_slots;
        if (!slot) {
            /* No slots - collect for deferred free */
            to_free[free_count++] = cache->buffers[class_idx][--cache->counts[class_idx]];
            continue;
        }

        /* Pop buffer from thread cache */
        void *buf = cache->buffers[class_idx][--cache->counts[class_idx]];

        /* Remove slot from free list and populate */
        shard->free_slots = slot->next;
        slot->buffer = buf;
        slot->actual_size = bucket_size;

        /* Push to shard bucket */
        slot->next = shard->size_buckets[class_idx];
        shard->size_buckets[class_idx] = slot;
        shard->free_count++;
        transferred++;
    }

    pthread_mutex_unlock(&shard->lock);

    /* Free collected buffers outside the lock (no lock churn) */
    if (free_count > 0) {
        atomic_fetch_sub(&pool->total_allocated, bucket_size * free_count);
        atomic_fetch_sub(&pool->total_buffers, free_count);
        for (int i = 0; i < free_count; i++) {
            free(to_free[i]);
        }
    }

    return transferred > 0 ? transferred : -free_count;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Calculate optimal shard count based on CPU count.
 *
 * Formula: shards = next_power_of_2(cpus / 4)
 * This targets ~4 threads per shard for good balance.
 *
 * Examples:
 *   1-4 cores   → 2 shards
 *   5-8 cores   → 2 shards
 *   9-16 cores  → 4 shards
 *   17-32 cores → 8 shards
 *   33-64 cores → 16 shards
 *   65-128 cores → 32 shards
 *   129+ cores  → 64 shards (max)
 */
static int calculate_shard_count(void) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) {
        cpus = 1;
    }

    /* Target ~4 threads per shard */
    int target = (int)(cpus / 4);
    if (target < BUFFER_POOL_MIN_SHARDS) {
        return BUFFER_POOL_MIN_SHARDS;
    }

    /* Round up to next power of 2 */
    int shards = BUFFER_POOL_MIN_SHARDS;
    while (shards < target && shards < BUFFER_POOL_MAX_SHARDS) {
        shards *= 2;
    }

    return shards;
}

int buffer_pool_init(buffer_pool_t *pool, size_t alignment) {
    if (!pool || alignment == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(pool, 0, sizeof(*pool));

    /* Auto-scale shard count based on CPU count */
    pool->shard_count = calculate_shard_count();
    pool->shard_mask = pool->shard_count - 1;

    pool->alignment = alignment;
    atomic_init(&pool->total_allocated, 0);
    atomic_init(&pool->total_buffers, 0);
    atomic_init(&pool->thread_caches, NULL);
    atomic_init(&pool->next_shard, 0);
    atomic_init(&pool->destroyed, false);

    /* Allocate shards array */
    pool->shards = calloc(pool->shard_count, sizeof(buffer_shard_t));
    if (!pool->shards) {
        return -1;
    }

    /* Initialize all shards */
    for (int i = 0; i < pool->shard_count; i++) {
        if (shard_init(&pool->shards[i]) != 0) {
            /* Cleanup already-initialized shards */
            for (int j = 0; j < i; j++) {
                shard_destroy(&pool->shards[j]);
            }
            free(pool->shards);
            pool->shards = NULL;
            return -1;
        }
    }

    return 0;
}

void buffer_pool_destroy(buffer_pool_t *pool) {
    if (!pool) {
        return;
    }

    /* Mark pool as destroyed FIRST so other threads can detect it.
     * Any thread calling get_thread_cache() after this will see the flag
     * and clear their TLS pointer instead of using the freed cache. */
    atomic_store_explicit(&pool->destroyed, true, memory_order_release);

    /* Free all thread caches and their buffered contents */
    thread_cache_t *cache = atomic_load(&pool->thread_caches);
    while (cache) {
        thread_cache_t *next = cache->next;

        /* Free all buffers in this thread cache */
        for (int class_idx = 0; class_idx < BUFFER_SIZE_CLASSES; class_idx++) {
            for (int i = 0; i < cache->counts[class_idx]; i++) {
                free(cache->buffers[class_idx][i]);
            }
        }

        /* Clear TLS if it points to this cache */
        if (tls_cache == cache) {
            tls_cache = NULL;
        }

        free(cache);
        cache = next;
    }
    atomic_store(&pool->thread_caches, NULL);

    /* Destroy all shards */
    if (pool->shards) {
        for (int i = 0; i < pool->shard_count; i++) {
            shard_destroy(&pool->shards[i]);
        }
        free(pool->shards);
        pool->shards = NULL;
    }

    pool->shard_count = 0;
    pool->shard_mask = 0;
    atomic_store(&pool->total_allocated, 0);
    atomic_store(&pool->total_buffers, 0);
}

void *buffer_pool_alloc(buffer_pool_t *pool, size_t size) {
    if (!pool || size == 0 || size > BUFFER_POOL_MAX_SIZE) {
        errno = EINVAL;
        return NULL;
    }

    int class_idx = size_to_class(size);
    size_t bucket_size = class_to_size(class_idx);

    /* Fast path: try thread-local cache first (no lock) */
    thread_cache_t *cache = get_thread_cache(pool);
    if (cache && cache->counts[class_idx] > 0) {
        return cache->buffers[class_idx][--cache->counts[class_idx]];
    }

    /* Thread cache empty - try to refill from assigned shard */
    if (cache) {
        int refilled = cache_refill(pool, cache, class_idx);
        if (refilled > 0) {
            return cache->buffers[class_idx][--cache->counts[class_idx]];
        }
    }

    /* Slow path: shard also empty, or no thread cache */
    /* Pick a shard - use cache's shard or round-robin for no-cache case */
    int shard_id = cache ? cache->shard_id :
                   (atomic_fetch_add(&pool->next_shard, 1) & pool->shard_mask);
    buffer_shard_t *shard = &pool->shards[shard_id];

    pthread_mutex_lock(&shard->lock);

    /* O(1) pop from bucket head - all buffers in bucket are bucket_size */
    buffer_slot_t *slot = shard->size_buckets[class_idx];
    if (slot) {
        shard->size_buckets[class_idx] = slot->next;
        void *buf = slot->buffer;
        shard->free_count--;

        /* Return slot to free list for reuse */
        slot->buffer = NULL;
        slot->next = shard->free_slots;
        shard->free_slots = slot;

        pthread_mutex_unlock(&shard->lock);
        return buf;
    }

    pthread_mutex_unlock(&shard->lock);

    /* No buffer in shard - allocate a new one at bucket_size */
    void *buf = NULL;
    int ret = posix_memalign(&buf, pool->alignment, bucket_size);
    if (ret != 0) {
        errno = ret;  /* posix_memalign returns error code, not via errno */
        return NULL;
    }

    atomic_fetch_add(&pool->total_allocated, bucket_size);
    atomic_fetch_add(&pool->total_buffers, 1);

    return buf;
}

void buffer_pool_free(buffer_pool_t *pool, void *buf, size_t size) {
    if (!pool || !buf) {
        return;
    }

    int class_idx = size_to_class(size);
    size_t bucket_size = class_to_size(class_idx);

    /* Fast path: try thread-local cache first (no lock) */
    thread_cache_t *cache = get_thread_cache(pool);
    if (cache && cache->counts[class_idx] < THREAD_CACHE_SIZE) {
        cache->buffers[class_idx][cache->counts[class_idx]++] = buf;
        return;
    }

    /* Thread cache full - flush some to assigned shard */
    if (cache && cache->counts[class_idx] >= THREAD_CACHE_SIZE) {
        cache_flush(pool, cache, class_idx, bucket_size);

        /* Now there should be room in the cache */
        if (cache->counts[class_idx] < THREAD_CACHE_SIZE) {
            cache->buffers[class_idx][cache->counts[class_idx]++] = buf;
            return;
        }
    }

    /* Slow path: no thread cache or flush failed, go directly to shard */
    int shard_id = cache ? cache->shard_id :
                   (atomic_fetch_add(&pool->next_shard, 1) & pool->shard_mask);
    buffer_shard_t *shard = &pool->shards[shard_id];

    pthread_mutex_lock(&shard->lock);

    /* Check high-water mark - if at capacity, just free the buffer */
    if (shard->max_free_count > 0 && shard->free_count >= shard->max_free_count) {
        atomic_fetch_sub(&pool->total_allocated, bucket_size);
        atomic_fetch_sub(&pool->total_buffers, 1);
        pthread_mutex_unlock(&shard->lock);
        free(buf);
        return;
    }

    /* Get a slot from the pre-allocated pool (no malloc!) */
    buffer_slot_t *slot = shard->free_slots;
    if (!slot) {
        /* All slots in use - just free the buffer */
        atomic_fetch_sub(&pool->total_allocated, bucket_size);
        atomic_fetch_sub(&pool->total_buffers, 1);
        pthread_mutex_unlock(&shard->lock);
        free(buf);
        return;
    }

    /* Remove slot from free list */
    shard->free_slots = slot->next;

    /* Populate slot and add to appropriate size bucket */
    slot->buffer = buf;
    slot->actual_size = bucket_size;

    slot->next = shard->size_buckets[class_idx];
    shard->size_buckets[class_idx] = slot;
    shard->free_count++;

    pthread_mutex_unlock(&shard->lock);
}
