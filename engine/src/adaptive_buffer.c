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
 *    - 500 cores: 128 shards (~200KB overhead)
 *    - 1024 cores: 256 shards (~400KB overhead)
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
#include <sched.h>
#include <unistd.h>

/* Global pool generation counter for unique IDs */
static _Atomic uint64_t pool_generation_counter = 0;

/* Thread-local cache pointer (fast-path access, zero overhead) */
static _Thread_local thread_cache_t *tls_cache = NULL;

/* pthread key for TLS destructor (fires on thread exit to clean up cache) */
static pthread_key_t tls_key;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;
static bool tls_key_valid = false; /* Set once by pthread_once; only read after */

/* Forward declarations for TLS destructor (defined after cache_flush) */
static void tls_destructor(void *arg);
static int cache_flush(buffer_pool_t *pool, thread_cache_t *cache, int class_idx,
                       size_t bucket_size);

static void tls_key_init(void) {
    if (pthread_key_create(&tls_key, tls_destructor) == 0) {
        tls_key_valid = true;
    }
}

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
 *   Class 15: <= 128MB (and overflow)
 *
 * Uses __builtin_clzl for branchless size→class mapping.  A single
 * predictable branch handles the common case (size <= 4KB = class 0).
 *
 * @param size Buffer size in bytes
 * @return Size class index (0 to BUFFER_SIZE_CLASSES-1)
 */
int size_to_class(size_t size) {
    /* Fast path: most common I/O buffer size (well-predicted). */
    if (size <= 4096) return 0;

    /* Branchless path for sizes > 4096.
     * ceil(log2(size)) = wordsize - clzl(size - 1).
     * class_idx = ceil(log2(size)) - 12, since class 0 = 2^12 = 4096.
     * Safety: size > 4096 guarantees (size - 1) > 0, so clzl is defined.
     * Also guarantees bits >= 13, so class_idx >= 1 (no underflow). */
    int bits = (int)(sizeof(unsigned long) * 8) - __builtin_clzl(size - 1);
    int class_idx = bits - 12;

    if (class_idx >= BUFFER_SIZE_CLASSES) return BUFFER_SIZE_CLASSES - 1;
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
 *
 * TLS lifecycle:
 * - _Thread_local tls_cache provides zero-overhead fast-path access
 * - pthread_key_t tls_key destructor fires on thread exit for cleanup
 * - buffer_pool_destroy coordinates via cleanup_mutex on each cache
 */
static thread_cache_t *get_thread_cache(buffer_pool_t *pool) {
    thread_cache_t *cache = tls_cache;

    /* Fast path: already have a valid cache for this pool.
     * pool pointer match guarantees this cache belongs to this pool instance.
     * The destroyed check (acquire) ensures we see any concurrent destruction.
     * pool_id is checked on the slow path only as defense-in-depth against
     * the ABA scenario (pool freed + new pool at same address). */
    if (cache && cache->pool == pool &&
        !atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        return cache;
    }

    /* Slow path: pool mismatch, no cache, or first call. */
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        tls_cache = NULL;
        return NULL;
    }

    /* Cache belongs to a different pool — clean up before returning NULL.
     * The caller will get NULL and fall through to the shard slow path. */
    if (cache && cache->pool != pool) {
        if (cache->pool == NULL) {
            /* Orphaned by pool destroy: pool set pool=NULL and left the
             * struct for us to free (since our TLS still referenced it). */
            pthread_mutex_destroy(&cache->cleanup_mutex);
            free(cache);
        } else {
            /* Old pool still alive — flush cached buffers back to it.
             * The cache is still in the old pool's thread_caches list,
             * so we can't free the struct (pool destroy will do that).
             * Mark cleaned_up so pool destroy skips the redundant flush. */
            buffer_pool_t *old_pool = cache->pool;
            pthread_mutex_lock(&cache->cleanup_mutex);
            if (!cache->cleaned_up) {
                for (int ci = 0; ci < BUFFER_SIZE_CLASSES; ci++) {
                    while (cache->counts[ci] > 0) {
                        cache_flush(old_pool, cache, ci, class_to_size(ci));
                    }
                }
                cache->cleaned_up = true;
            }
            pthread_mutex_unlock(&cache->cleanup_mutex);
        }
        tls_cache = NULL;
        if (tls_key_valid) {
            pthread_setspecific(tls_key, NULL);
        }
        return NULL;
    }

    atomic_fetch_add_explicit(&pool->registrations_inflight, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&pool->registrations_inflight, 1, memory_order_acq_rel);
        tls_cache = NULL;
        return NULL;
    }

    /* First access from this thread — create a new cache */
    pthread_once(&tls_key_once, tls_key_init);

    cache = calloc(1, sizeof(thread_cache_t));
    if (!cache) {
        atomic_fetch_sub_explicit(&pool->registrations_inflight, 1, memory_order_acq_rel);
        return NULL;
    }

    cache->pool = pool;
    cache->pool_id = pool->pool_id;
    pthread_mutex_init(&cache->cleanup_mutex, NULL);

    /* Assign shard via round-robin for load balancing */
    cache->shard_id = atomic_fetch_add(&pool->next_shard, 1) & pool->shard_mask;

    /* Lock-free registration with pool's cache list using CAS */
    thread_cache_t *old_head;
    do {
        old_head = atomic_load(&pool->thread_caches);
        cache->next = old_head;
    } while (!atomic_compare_exchange_weak(&pool->thread_caches, &old_head, cache));

    /* Re-check liveness after registration. If pool was destroyed between
     * our first check and the CAS, destroy may already be walking the list
     * and could access this cache. Do NOT free or detach it here; leave
     * ownership with destroy/list traversal to avoid UAF races. */
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        pthread_mutex_lock(&cache->cleanup_mutex);
        cache->cleaned_up = true; /* Empty cache — nothing to flush */
        /* Keep pool pointer intact until destroy processes this node.
         * Otherwise tls_destructor can free it while destroy still has a
         * detached list pointer to this cache. */
        pthread_mutex_unlock(&cache->cleanup_mutex);
        /* Keep orphan reachable so the same thread can reclaim it on the
         * next pool access (or at thread exit via TLS destructor). */
        if (tls_key_valid) {
            pthread_setspecific(tls_key, cache);
        }
        tls_cache = cache;
        atomic_fetch_sub_explicit(&pool->registrations_inflight, 1, memory_order_acq_rel);
        return NULL;
    }

    /* Register with pthread key for destructor on thread exit */
    if (tls_key_valid) {
        pthread_setspecific(tls_key, cache);
    }

    tls_cache = cache;
    atomic_fetch_sub_explicit(&pool->registrations_inflight, 1, memory_order_acq_rel);
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
            break; /* Shard empty for this class */
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
static int cache_flush(buffer_pool_t *pool, thread_cache_t *cache, int class_idx,
                       size_t bucket_size) {
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
 * TLS Destructor (fires on thread exit via pthread_key_create)
 * ============================================================================ */

/**
 * Called automatically when a thread exits (via pthread_key_create destructor).
 *
 * Coordinates with buffer_pool_destroy via cleanup_mutex to ensure exactly
 * one side handles buffer cleanup and exactly one side frees the struct:
 *
 * - If destructor runs first (pool still alive): flush buffers to shards,
 *   mark cleaned_up. Pool destroy later sees cleaned_up=true, skips cleanup,
 *   and frees the struct (because thread_exited=true).
 *
 * - If pool destroy runs first: it freed buffers and set pool=NULL.
 *   Destructor sees cleaned_up=true and pool_gone=true, frees the struct.
 *
 * - Concurrent: cleanup_mutex serializes. Loser skips buffer cleanup.
 */
static void tls_destructor(void *arg) {
    thread_cache_t *cache = arg;
    if (!cache) return;

    tls_cache = NULL;

    pthread_mutex_lock(&cache->cleanup_mutex);
    cache->thread_exited = true;

    if (!cache->cleaned_up) {
        /* We got here first — flush buffers back to shards.
         * Shards are guaranteed alive: pool destroy tears down shards
         * only after processing all caches (which blocks on our mutex). */
        buffer_pool_t *pool = cache->pool;
        if (pool) {
            for (int class_idx = 0; class_idx < BUFFER_SIZE_CLASSES; class_idx++) {
                while (cache->counts[class_idx] > 0) {
                    cache_flush(pool, cache, class_idx, class_to_size(class_idx));
                }
            }
        }
        cache->cleaned_up = true;
    }

    bool pool_gone = (cache->pool == NULL);
    pthread_mutex_unlock(&cache->cleanup_mutex);

    if (pool_gone) {
        /* Pool destroy already ran and left this struct for us to free. */
        pthread_mutex_destroy(&cache->cleanup_mutex);
        free(cache);
    }
    /* Otherwise pool is alive and pool destroy will free the struct
     * when it sees thread_exited=true. */
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
 *   129-256 cores → 64 shards
 *   257-512 cores → 128 shards
 *   513+ cores    → 256 shards (max)
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
    pool->pool_id = atomic_fetch_add(&pool_generation_counter, 1);
    atomic_init(&pool->total_allocated, 0);
    atomic_init(&pool->total_buffers, 0);
    atomic_init(&pool->thread_caches, NULL);
    atomic_init(&pool->next_shard, 0);
    atomic_init(&pool->destroyed, false);
    atomic_init(&pool->registrations_inflight, 0);
    atomic_init(&pool->active_users, 0);

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

    /* Mark pool as destroyed FIRST so new get_thread_cache() calls bail out.
     *
     * Known limitation: a thread that has already passed the destroyed check
     * in get_thread_cache() but not yet registered its cache may retain its
     * TLS cache (~2KB) until thread exit. This is acceptable for the shutdown
     * path — the memory is freed when the thread's TLS destructor fires. */
    atomic_store_explicit(&pool->destroyed, true, memory_order_release);

    /* Wait for in-flight cache registrations to quiesce so no new nodes can
     * be published after the final atomic_exchange(NULL) in the drain loop. */
    while (atomic_load_explicit(&pool->registrations_inflight, memory_order_acquire) > 0) {
        sched_yield();
    }

    /* Wait for threads in the shard slow path (alloc/free) to finish.
     * These threads have already passed the destroyed check and are
     * accessing pool->shards — we must not free shards until they exit. */
    while (atomic_load_explicit(&pool->active_users, memory_order_acquire) > 0) {
        sched_yield();
    }

    /* Process all thread caches.
     *
     * For each cache, coordinate with the TLS destructor via cleanup_mutex:
     * - If the thread already exited (thread_exited=true): its destructor
     *   handled buffer cleanup. We just free the struct.
     * - If the thread is still alive: we handle buffer cleanup, set pool=NULL
     *   to mark the cache as orphaned, and leave the struct for the thread's
     *   destructor (or get_thread_cache orphan cleanup) to free.
     * - If racing with the destructor: cleanup_mutex serializes. Whoever
     *   gets the lock first handles buffers; the other skips. */
    for (;;) {
        /* Detach the currently visible cache list. If a late registrar races
         * and pushes a new node, it lands in pool->thread_caches and is picked
         * up by the next iteration. */
        thread_cache_t *cache = atomic_exchange(&pool->thread_caches, NULL);
        if (!cache) {
            break;
        }

        while (cache) {
            thread_cache_t *next = cache->next;

            pthread_mutex_lock(&cache->cleanup_mutex);

            if (!cache->cleaned_up) {
                /* Free all cached buffers (destructor hasn't done it yet) */
                for (int class_idx = 0; class_idx < BUFFER_SIZE_CLASSES; class_idx++) {
                    for (int i = 0; i < cache->counts[class_idx]; i++) {
                        free(cache->buffers[class_idx][i]);
                    }
                    cache->counts[class_idx] = 0;
                }
                cache->cleaned_up = true;
            }

            cache->pool = NULL;
            bool exited = cache->thread_exited;

            pthread_mutex_unlock(&cache->cleanup_mutex);

            if (tls_cache == cache) {
                /* This is the calling thread's own cache */
                tls_cache = NULL;
                if (tls_key_valid) {
                    pthread_setspecific(tls_key, NULL);
                }
                pthread_mutex_destroy(&cache->cleanup_mutex);
                free(cache);
            } else if (exited) {
                /* Thread already exited — destructor left the struct for us */
                pthread_mutex_destroy(&cache->cleanup_mutex);
                free(cache);
            }
            /* else: thread is alive. Its destructor or get_thread_cache orphan
             * cleanup will see pool==NULL and free the struct. */

            cache = next;
        }
    }

    /* Destroy all shards (safe: any racing destructor that held cleanup_mutex
     * has already released it and finished flushing to shards above). */
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

    /* Slow path: shard also empty, or no thread cache.
     * Acquire active_users BEFORE checking destroyed to prevent a race with
     * buffer_pool_destroy: destroy sets destroyed=true then waits for
     * active_users==0 before freeing shards.  By incrementing first, we
     * guarantee destroy will wait for us to finish shard access. */
    atomic_fetch_add_explicit(&pool->active_users, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
        errno = ENXIO;
        return NULL;
    }

    /* Pick a shard - use cache's shard or round-robin for no-cache case */
    int shard_id =
        cache ? cache->shard_id : (atomic_fetch_add(&pool->next_shard, 1) & pool->shard_mask);
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
        atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
        return buf;
    }

    pthread_mutex_unlock(&shard->lock);
    atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);

    /* No buffer in shard - allocate from heap.
     * Batch-allocate to fill the thread cache so subsequent allocs avoid
     * the heap entirely.  This amortises posix_memalign overhead across
     * THREAD_CACHE_BATCH_SIZE allocations instead of one. */
    void *buf = NULL;
    int ret = posix_memalign(&buf, pool->alignment, bucket_size);
    if (ret != 0) {
        errno = ret; /* posix_memalign returns error code, not via errno */
        return NULL;
    }

    atomic_fetch_add(&pool->total_allocated, bucket_size);
    atomic_fetch_add(&pool->total_buffers, 1);

    /* Pre-fill thread cache with extra buffers to avoid future heap trips */
    if (cache) {
        int prefill = 0;
        while (prefill < THREAD_CACHE_BATCH_SIZE - 1 &&
               cache->counts[class_idx] < THREAD_CACHE_SIZE) {
            void *extra = NULL;
            if (posix_memalign(&extra, pool->alignment, bucket_size) != 0) {
                break;
            }
            cache->buffers[class_idx][cache->counts[class_idx]++] = extra;
            prefill++;
        }
        if (prefill > 0) {
            atomic_fetch_add(&pool->total_allocated, bucket_size * prefill);
            atomic_fetch_add(&pool->total_buffers, prefill);
        }
    }

    return buf;
}

void buffer_pool_free(buffer_pool_t *pool, void *buf, size_t size) {
    if (!pool || !buf) {
        return;
    }

    /* Pool destroyed — shards are gone, just free the buffer directly.
     * This can happen when a late callback frees a buffer after destroy. */
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        atomic_fetch_sub(&pool->total_allocated, size);
        atomic_fetch_sub(&pool->total_buffers, 1);
        free(buf);
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

    /* Slow path: no thread cache or flush failed, go directly to shard.
     * Same active_users protocol as buffer_pool_alloc slow path — see
     * comment there for the synchronization rationale. */
    atomic_fetch_add_explicit(&pool->active_users, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
        free(buf);
        return;
    }

    int shard_id =
        cache ? cache->shard_id : (atomic_fetch_add(&pool->next_shard, 1) & pool->shard_mask);
    buffer_shard_t *shard = &pool->shards[shard_id];

    pthread_mutex_lock(&shard->lock);

    /* Check high-water mark - if at capacity, just free the buffer */
    if (shard->max_free_count > 0 && shard->free_count >= shard->max_free_count) {
        atomic_fetch_sub(&pool->total_allocated, bucket_size);
        atomic_fetch_sub(&pool->total_buffers, 1);
        pthread_mutex_unlock(&shard->lock);
        atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
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
        atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
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
    atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
}

/* ============================================================================
 * Buffer Size Map (ptr → size_class)
 * ============================================================================ */

static inline size_t buf_map_hash(uintptr_t key, size_t mask) {
    /* Mix bits — pointers are page-aligned so low 12 bits are zero.
     * Fibonacci hashing distributes well for aligned pointers. */
    key ^= key >> 16;
    key *= 0x9E3779B97F4A7C15ULL;
    key ^= key >> 16;
    return key & mask;
}

int buf_size_map_init(buf_size_map_t *map) {
    map->capacity = BUF_MAP_INITIAL_CAPACITY;
    atomic_init(&map->count, 0);
    map->entries = calloc(map->capacity, sizeof(buf_map_entry_t));
    if (!map->entries) return -1;
    pthread_mutex_init(&map->lock, NULL);
    return 0;
}

void buf_size_map_destroy(buf_size_map_t *map) {
    free(map->entries);
    map->entries = NULL;
    map->capacity = 0;
    pthread_mutex_destroy(&map->lock);
}

/* Grow the table (caller must hold lock).
 * Returns 0 on success, -1 on allocation failure. */
static int buf_map_grow(buf_size_map_t *map) {
    size_t old_cap = map->capacity;
    size_t new_cap = old_cap * 2;
    buf_map_entry_t *old = map->entries;
    buf_map_entry_t *new_entries = calloc(new_cap, sizeof(buf_map_entry_t));
    if (!new_entries) return -1;

    size_t new_mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].key != 0) {
            size_t idx = buf_map_hash(old[i].key, new_mask);
            while (new_entries[idx].key != 0) {
                idx = (idx + 1) & new_mask;
            }
            new_entries[idx] = old[i];
        }
    }

    map->entries = new_entries;
    map->capacity = new_cap;
    free(old);
    return 0;
}

int buf_size_map_insert(buf_size_map_t *map, void *ptr, int class_idx) {
    uintptr_t key = (uintptr_t)ptr;

    pthread_mutex_lock(&map->lock);

    /* Check load factor and grow if needed */
    size_t count = atomic_load_explicit(&map->count, memory_order_relaxed);
    if (count * BUF_MAP_LOAD_FACTOR_DEN >= map->capacity * BUF_MAP_LOAD_FACTOR_NUM) {
        if (buf_map_grow(map) != 0) {
            pthread_mutex_unlock(&map->lock);
            errno = ENOMEM;
            return -1;
        }
    }

    size_t mask = map->capacity - 1;
    size_t idx = buf_map_hash(key, mask);
    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) {
            /* Duplicate key: update in place instead of inserting again */
            map->entries[idx].class_idx = (uint8_t)class_idx;
            pthread_mutex_unlock(&map->lock);
            return 0;
        }
        idx = (idx + 1) & mask;
    }
    map->entries[idx].key = key;
    map->entries[idx].class_idx = (uint8_t)class_idx;
    atomic_fetch_add_explicit(&map->count, 1, memory_order_relaxed);
    pthread_mutex_unlock(&map->lock);
    return 0;
}

int buf_size_map_remove(buf_size_map_t *map, void *ptr) {
    uintptr_t key = (uintptr_t)ptr;

    pthread_mutex_lock(&map->lock);

    size_t mask = map->capacity - 1;
    size_t idx = buf_map_hash(key, mask);

    /* Linear probe to find the key */
    while (map->entries[idx].key != 0) {
        if (map->entries[idx].key == key) {
            int class_idx = map->entries[idx].class_idx;
            /* Delete with backward-shift to maintain probe chains */
            size_t hole = idx;
            for (;;) {
                size_t next = (hole + 1) & mask;
                if (map->entries[next].key == 0) break;
                size_t natural = buf_map_hash(map->entries[next].key, mask);
                /* Check if 'next' is displaced past 'hole' */
                bool displaced = (hole < next) ? (natural <= hole || natural > next)
                                               : (natural <= hole && natural > next);
                if (displaced) {
                    map->entries[hole] = map->entries[next];
                    hole = next;
                } else {
                    break;
                }
            }
            map->entries[hole].key = 0;
            atomic_fetch_sub_explicit(&map->count, 1, memory_order_relaxed);
            pthread_mutex_unlock(&map->lock);
            return class_idx;
        }
        idx = (idx + 1) & mask;
    }

    pthread_mutex_unlock(&map->lock);
    return -1; /* Not found */
}

void buffer_pool_free_tracked(buffer_pool_t *pool, buf_size_map_t *map, void *buf) {
    if (!pool || !buf) return;

    int class_idx = buf_size_map_remove(map, buf);
    if (class_idx < 0) {
        /* Unknown buffer — just free it directly */
        free(buf);
        return;
    }

    size_t size = class_to_size(class_idx);
    buffer_pool_free(pool, buf, size);
}
