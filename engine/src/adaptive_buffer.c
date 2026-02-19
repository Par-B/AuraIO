// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

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
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <sched.h>
#include <unistd.h>
#include <assert.h>

/* Global pool generation counter for unique IDs */
static _Atomic uint64_t pool_generation_counter = 0;

/* Thread-local cache pointer (fast-path access, zero overhead) */
static _Thread_local thread_cache_t *tls_cache = NULL;

/* pthread key for TLS destructor (fires on thread exit to clean up cache) */
static pthread_key_t tls_key;
static pthread_once_t tls_key_once = PTHREAD_ONCE_INIT;
static _Atomic bool tls_key_valid =
    false; /* Set once by pthread_once; read from multiple threads */

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
 * Cleanup mode for cache buffer disposal.
 */
typedef enum {
    CACHE_CLEANUP_FLUSH, /**< Flush buffers back to pool (pool still alive) */
    CACHE_CLEANUP_FREE /**< Free buffers directly (pool being destroyed) */
} cache_cleanup_mode_t;

/**
 * Clean up cached buffers with the specified disposal strategy.
 *
 * Must be called with cache->cleanup_mutex held.
 *
 * @param pool Pool to flush to (NULL allowed for FREE mode)
 * @param cache Thread cache to clean
 * @param mode FLUSH (return to pool) or FREE (destroy directly)
 */
static void cleanup_cache_buffers_locked(buffer_pool_t *pool, thread_cache_t *cache,
                                         cache_cleanup_mode_t mode) {
    if (cache->cleaned_up) {
        return; /* Already cleaned by another thread */
    }

    if (mode == CACHE_CLEANUP_FLUSH && pool) {
        /* Flush buffers back to pool shards */
        for (int class_idx = 0; class_idx < BUFFER_SIZE_CLASSES; class_idx++) {
            while (cache->counts[class_idx] > 0) {
                cache_flush(pool, cache, class_idx, class_to_size(class_idx));
            }
        }
    } else {
        /* Free buffers directly (pool being destroyed or NULL) */
        for (int class_idx = 0; class_idx < BUFFER_SIZE_CLASSES; class_idx++) {
            int count = cache->counts[class_idx];
            if (count > 0) {
                size_t bucket_size = class_to_size(class_idx);
                for (int i = 0; i < count; i++) {
                    free(cache->buffers[class_idx][i]);
                }
                /* Update stats counters so aura_get_buffer_stats() stays accurate.
                 * pool may be NULL if destroy already completed. */
                if (pool) {
                    atomic_fetch_sub_explicit(&pool->total_allocated, bucket_size * count,
                                              memory_order_relaxed);
                    atomic_fetch_sub_explicit(&pool->total_buffers, count, memory_order_relaxed);
                }
                cache->counts[class_idx] = 0;
            }
        }
    }

    cache->cleaned_up = true;
}

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
     * pool pointer + pool_id match guards against the ABA scenario where a
     * pool is freed and a new pool is allocated at the same address.
     * The destroyed check (acquire) ensures we see any concurrent destruction. */
    if (cache && atomic_load_explicit(&cache->pool, memory_order_acquire) == pool &&
        cache->pool_id == pool->pool_id &&
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
    if (cache && atomic_load_explicit(&cache->pool, memory_order_acquire) != pool) {
        if (atomic_load_explicit(&cache->pool, memory_order_acquire) == NULL) {
            /* Orphaned by pool destroy: pool set pool=NULL and left the
             * struct for us to free (since our TLS still referenced it). */
            pthread_mutex_destroy(&cache->cleanup_mutex);
            free(cache);
        } else {
            /* Old pool still alive — flush cached buffers back to it.
             * The cache is still in the old pool's thread_caches list,
             * so we can't free the struct (pool destroy will do that).
             * Mark cleaned_up so pool destroy skips the redundant flush.
             * If old pool is being destroyed concurrently, fall through to
             * FREE mode (direct free) to avoid dereferencing freed shards.
             *
             * We must hold active_users while accessing shards to prevent
             * a concurrent pool_destroy from freeing them under us. */
            pthread_mutex_lock(&cache->cleanup_mutex);
            buffer_pool_t *old_pool = atomic_load_explicit(&cache->pool, memory_order_acquire);
            if (!old_pool) {
                /* Pool was destroyed between our checks — free the cache. */
                pthread_mutex_unlock(&cache->cleanup_mutex);
                pthread_mutex_destroy(&cache->cleanup_mutex);
                free(cache);
                tls_cache = NULL;
                if (tls_key_valid) {
                    pthread_setspecific(tls_key, NULL);
                }
                return NULL;
            }
            atomic_fetch_add_explicit(&old_pool->active_users, 1, memory_order_acq_rel);
            if (atomic_load_explicit(&old_pool->destroyed, memory_order_acquire)) {
                cleanup_cache_buffers_locked(old_pool, cache, CACHE_CLEANUP_FREE);
            } else {
                cleanup_cache_buffers_locked(old_pool, cache, CACHE_CLEANUP_FLUSH);
            }
            pthread_mutex_unlock(&cache->cleanup_mutex);
            atomic_fetch_sub_explicit(&old_pool->active_users, 1, memory_order_acq_rel);
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

    atomic_store_explicit(&cache->pool, pool, memory_order_release);
    cache->pool_id = pool->pool_id;
    cache->owner_thread = pthread_self();
    if (pthread_mutex_init(&cache->cleanup_mutex, NULL) != 0) {
        free(cache);
        atomic_fetch_sub_explicit(&pool->registrations_inflight, 1, memory_order_acq_rel);
        return NULL;
    }

    /* Assign shard via round-robin for load balancing */
    cache->shard_id =
        atomic_fetch_add_explicit(&pool->next_shard, 1, memory_order_relaxed) & pool->shard_mask;

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
     * The "no slot" path can accumulate up to THREAD_CACHE_BATCH_SIZE entries,
     * then the high-water early-exit can drain all remaining (up to
     * THREAD_CACHE_SIZE), so we need room for both. */
    void *to_free[THREAD_CACHE_SIZE + THREAD_CACHE_BATCH_SIZE];
    int free_count = 0;

    pthread_mutex_lock(&shard->lock);

    while (transferred < THREAD_CACHE_BATCH_SIZE && cache->counts[class_idx] > 0) {
        /* Check high-water mark */
        if (shard->max_free_count > 0 && shard->free_count >= shard->max_free_count) {
            /* Shard at capacity - release lock early since the drain
             * below only touches the thread-local cache (no shared state). */
            pthread_mutex_unlock(&shard->lock);
            while (cache->counts[class_idx] > 0) {
                to_free[free_count++] = cache->buffers[class_idx][--cache->counts[class_idx]];
            }
            goto free_path;
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

free_path:
    /* Free collected buffers outside the lock (no lock churn) */
    if (free_count > 0) {
        atomic_fetch_sub_explicit(&pool->total_allocated, bucket_size * free_count,
                                  memory_order_relaxed);
        atomic_fetch_sub_explicit(&pool->total_buffers, free_count, memory_order_relaxed);
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

    /* Flush buffers back to shards if we got here first.
     *
     * We must hold active_users while flushing to shards to prevent a
     * concurrent buffer_pool_destroy from freeing pool->shards under us.
     * The protocol mirrors buffer_pool_alloc/free slow path:
     *   1. Increment active_users BEFORE checking destroyed.
     *   2. Re-check destroyed after increment; if now true, decrement and
     *      fall back to FREE mode (destroy is already past its active_users
     *      wait and may have freed shards at any moment).
     *   3. Flush to shards while holding the active_users count.
     *   4. Decrement active_users when done.
     *
     * If pool is NULL (destroy already completed and orphaned this cache),
     * free buffers directly — there are no shards to return to. */
    buffer_pool_t *pool = atomic_load_explicit(&cache->pool, memory_order_acquire);
    if (pool) {
        atomic_fetch_add_explicit(&pool->active_users, 1, memory_order_acq_rel);
        if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
            /* Destroy already completed (or is past its active_users wait)
             * — shards may be freed at any moment, do not access them. */
            atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_acq_rel);
            cleanup_cache_buffers_locked(NULL, cache, CACHE_CLEANUP_FREE);
        } else {
            cleanup_cache_buffers_locked(pool, cache, CACHE_CLEANUP_FLUSH);
            atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_acq_rel);
        }
    } else {
        cleanup_cache_buffers_locked(NULL, cache, CACHE_CLEANUP_FREE);
    }

    bool pool_gone = (atomic_load_explicit(&cache->pool, memory_order_acquire) == NULL);
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
 * Shared Inline Helpers
 * ============================================================================ */

/** Select shard: use cache's assigned shard, or round-robin fallback. */
static inline int select_shard(buffer_pool_t *pool, thread_cache_t *cache) {
    return cache ? cache->shard_id
                 : (atomic_fetch_add_explicit(&pool->next_shard, 1, memory_order_relaxed) &
                    pool->shard_mask);
}

/**
 * Enter pool slow path: increment active_users and check destroyed.
 * Returns true if pool is still alive (caller must call pool_leave when done).
 * Returns false if pool was destroyed (active_users already decremented).
 */
static inline bool pool_enter(buffer_pool_t *pool) {
    atomic_fetch_add_explicit(&pool->active_users, 1, memory_order_acq_rel);
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
        return false;
    }
    return true;
}

/** Leave pool slow path. */
static inline void pool_leave(buffer_pool_t *pool) {
    atomic_fetch_sub_explicit(&pool->active_users, 1, memory_order_release);
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
    if (!pool || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        alignment < sizeof(void *)) {
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

/**
 * Phase 1: Mark pool destroyed and wait for in-flight operations to quiesce.
 *
 * Sets destroyed flag with memory_order_release to ensure all prior pool
 * modifications are visible to threads checking the flag. Then waits for:
 * - registrations_inflight: threads currently registering new caches
 * - active_users: threads currently in shard alloc/free slow path
 */
static void cleanup_phase_1_mark_destroyed(buffer_pool_t *pool) {
    /* Mark pool as destroyed FIRST so new get_thread_cache() calls bail out.
     *
     * Known limitation: a thread that has already passed the destroyed check
     * in get_thread_cache() but not yet registered its cache may retain its
     * TLS cache (~2KB) until thread exit. This is acceptable for the shutdown
     * path — the memory is freed when the thread's TLS destructor fires. */
    atomic_store_explicit(&pool->destroyed, true, memory_order_release);

    /* Wait for in-flight cache registrations to quiesce so no new nodes can
     * be published after the final atomic_exchange(NULL) in the drain loop.
     * Bounded to avoid infinite hang if a worker thread misbehaves. */
    int yield_count = 0;
    while (atomic_load_explicit(&pool->registrations_inflight, memory_order_acquire) > 0 &&
           yield_count < 10000) {
        sched_yield();
        yield_count++;
    }
    if (atomic_load_explicit(&pool->registrations_inflight, memory_order_acquire) > 0) {
        aura_log(AURA_LOG_WARN,
                 "buffer_pool cleanup: timed out waiting for registrations_inflight (%d)",
                 (int)atomic_load_explicit(&pool->registrations_inflight, memory_order_acquire));
    }

    /* Wait for threads in the shard slow path (alloc/free) to finish.
     * These threads have already passed the destroyed check and are
     * accessing pool->shards — we must not free shards until they exit. */
    yield_count = 0;
    while (atomic_load_explicit(&pool->active_users, memory_order_acquire) > 0 &&
           yield_count < 10000) {
        sched_yield();
        yield_count++;
    }
    if (atomic_load_explicit(&pool->active_users, memory_order_acquire) > 0) {
        aura_log(AURA_LOG_WARN, "buffer_pool cleanup: timed out waiting for active_users (%d)",
                 (int)atomic_load_explicit(&pool->active_users, memory_order_acquire));
    }
}

/**
 * Phase 2: Process all thread caches and coordinate cleanup.
 *
 * For each cache, coordinate with the TLS destructor via cleanup_mutex:
 * - If thread exited: destructor handled cleanup, we just free the struct
 * - If thread alive: we handle cleanup, orphan the cache (pool=NULL)
 * - Racing with destructor: cleanup_mutex serializes, first one wins
 */
static void cleanup_phase_2_process_caches(buffer_pool_t *pool) {
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

            /* Free all cached buffers (destructor hasn't done it yet) */
            cleanup_cache_buffers_locked(NULL, cache, CACHE_CLEANUP_FREE);

            atomic_store_explicit(&cache->pool, NULL, memory_order_release);
            bool exited = cache->thread_exited;

            pthread_mutex_unlock(&cache->cleanup_mutex);

            if (pthread_equal(cache->owner_thread, pthread_self())) {
                /* This is the calling thread's own cache.  Always clear both
                 * tls_cache and the pthread key to prevent use-after-free in
                 * tls_destructor when the thread eventually exits. */
                if (tls_cache == cache) {
                    tls_cache = NULL;
                }
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
}

/**
 * Phase 3: Destroy all shards and reset pool metadata.
 *
 * Safe to call after phase 2 completes - all caches have been processed
 * and any racing destructors have finished flushing to shards.
 */
static void cleanup_phase_3_destroy_shards(buffer_pool_t *pool) {
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

void buffer_pool_destroy(buffer_pool_t *pool) {
    if (!pool) {
        return;
    }

    cleanup_phase_1_mark_destroyed(pool);
    cleanup_phase_2_process_caches(pool);
    cleanup_phase_3_destroy_shards(pool);
}

void *buffer_pool_alloc(buffer_pool_t *pool, size_t size) {
    if (!pool || size == 0 || size > BUFFER_POOL_MAX_SIZE) {
        errno = EINVAL;
        return NULL;
    }

    int class_idx = size_to_class(size);
    size_t bucket_size = class_to_size(class_idx);

    /* Fast path: try thread-local cache first (no lock, no shard access) */
    thread_cache_t *cache = get_thread_cache(pool);
    if (cache && cache->counts[class_idx] > 0) {
        return cache->buffers[class_idx][--cache->counts[class_idx]];
    }

    /* All paths below access pool->shards, so hold active_users to prevent
     * buffer_pool_destroy from freeing the shards array under us. */
    if (!pool_enter(pool)) {
        errno = ENXIO;
        return NULL;
    }

    /* Thread cache empty - try to refill from assigned shard */
    if (cache) {
        int refilled = cache_refill(pool, cache, class_idx);
        if (refilled > 0) {
            pool_leave(pool);
            return cache->buffers[class_idx][--cache->counts[class_idx]];
        }
    }

    buffer_shard_t *shard = &pool->shards[select_shard(pool, cache)];

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
        pool_leave(pool);
        return buf;
    }

    pthread_mutex_unlock(&shard->lock);
    pool_leave(pool);

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

    atomic_fetch_add_explicit(&pool->total_allocated, bucket_size, memory_order_relaxed);
    atomic_fetch_add_explicit(&pool->total_buffers, 1, memory_order_relaxed);

    /* Pre-fill thread cache with extra buffers to avoid future heap trips.
     * Check cleaned_up under cleanup_mutex to avoid storing into a cache
     * that buffer_pool_destroy has already processed (would leak buffers). */
    if (cache) {
        pthread_mutex_lock(&cache->cleanup_mutex);
        if (!cache->cleaned_up && !atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
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
                atomic_fetch_add_explicit(&pool->total_allocated, bucket_size * prefill,
                                          memory_order_relaxed);
                atomic_fetch_add_explicit(&pool->total_buffers, prefill, memory_order_relaxed);
            }
        }
        pthread_mutex_unlock(&cache->cleanup_mutex);
    }

    return buf;
}

void buffer_pool_free(buffer_pool_t *pool, void *buf, size_t size) {
    if (!pool || !buf) {
        return;
    }

    /* Guard against size=0: size_to_class(0) returns class 0, which would
     * place the buffer in the wrong bucket. Free directly instead. */
    if (size == 0) {
        /* Caller error: size=0 buffers can't be pool-allocated, so we can't
         * update stats or return to a bucket.  Free directly. */
        assert(0 && "buffer_pool_free called with size=0");
        free(buf);
        return;
    }

    /* Pool destroyed — shards are gone, just free the buffer directly.
     * This can happen when a late callback frees a buffer after destroy. */
    if (atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
        /* Counters were already zeroed during destroy — don't subtract
         * from 0, which would wrap the unsigned atomics. */
        free(buf);
        return;
    }

    int class_idx = size_to_class(size);
    size_t bucket_size = class_to_size(class_idx);

    /* Fast path: try thread-local cache first (no lock, no shard access).
     *
     * Safety against concurrent pool destroy:
     *   buffer_pool_destroy sets destroyed=true (with release) BEFORE it calls
     *   cleanup_phase_2_process_caches.  So if we see destroyed=false here,
     *   destroy has not yet touched any cache.  If destroy races past our
     *   destroyed check and cleans our cache before we store, we re-check
     *   destroyed under active_users on the slow path below (and free directly
     *   if set).  The window where a buffer could be stored into an already-
     *   cleaned cache is closed by the ordering: destroy sets destroyed, then
     *   waits for active_users==0, then cleans caches.  We hold active_users
     *   on the slow path, so destroy cannot clean caches while we're there.
     *
     * Calling get_thread_cache without holding active_users is intentional:
     * get_thread_cache can acquire cleanup_mutex internally, and holding
     * active_users while doing so would deadlock with buffer_pool_destroy
     * (which holds cleanup_mutex and waits for active_users==0). */
    thread_cache_t *cache = get_thread_cache(pool);
    if (cache && cache->counts[class_idx] < THREAD_CACHE_SIZE) {
        /* Serialize with cleanup_phase_2_process_caches to prevent storing
         * into a cache that is concurrently being cleaned.  Without this,
         * a buffer stored after cleanup_cache_buffers_locked reads counts[]
         * would be leaked (never freed). */
        pthread_mutex_lock(&cache->cleanup_mutex);
        if (cache->cleaned_up || atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
            pthread_mutex_unlock(&cache->cleanup_mutex);
            free(buf);
            return;
        }
        cache->buffers[class_idx][cache->counts[class_idx]++] = buf;
        pthread_mutex_unlock(&cache->cleanup_mutex);
        return;
    }

    /* All paths below access pool->shards, so hold active_users to prevent
     * buffer_pool_destroy from freeing the shards array under us. */
    if (!pool_enter(pool)) {
        free(buf);
        return;
    }

    if (cache && cache->counts[class_idx] >= THREAD_CACHE_SIZE) {
        cache_flush(pool, cache, class_idx, bucket_size);

        /* Now there should be room in the cache.  Must hold cleanup_mutex
         * to prevent a race with buffer_pool_destroy's cleanup phase. */
        if (cache->counts[class_idx] < THREAD_CACHE_SIZE) {
            pool_leave(pool);
            pthread_mutex_lock(&cache->cleanup_mutex);
            if (cache->cleaned_up || atomic_load_explicit(&pool->destroyed, memory_order_acquire)) {
                pthread_mutex_unlock(&cache->cleanup_mutex);
                free(buf);
                return;
            }
            cache->buffers[class_idx][cache->counts[class_idx]++] = buf;
            pthread_mutex_unlock(&cache->cleanup_mutex);
            return;
        }
    }

    buffer_shard_t *shard = &pool->shards[select_shard(pool, cache)];

    pthread_mutex_lock(&shard->lock);

    /* Get a slot — bail to free_and_return if shard full or no slots */
    buffer_slot_t *slot = NULL;
    if (shard->max_free_count > 0 && shard->free_count >= shard->max_free_count) {
        goto free_and_return;
    }
    slot = shard->free_slots;
    if (!slot) {
        goto free_and_return;
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
    pool_leave(pool);
    return;

free_and_return:
    atomic_fetch_sub_explicit(&pool->total_allocated, bucket_size, memory_order_relaxed);
    atomic_fetch_sub_explicit(&pool->total_buffers, 1, memory_order_relaxed);
    pthread_mutex_unlock(&shard->lock);
    pool_leave(pool);
    free(buf);
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
    if (pthread_mutex_init(&map->lock, NULL) != 0) {
        free(map->entries);
        map->entries = NULL;
        return -1;
    }
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
    if (new_cap > BUF_MAP_MAX_CAPACITY) {
        return -1;
    }
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
    if (!ptr) return -1;
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
    for (size_t probe = 0; probe < map->capacity; probe++) {
        if (map->entries[idx].key == 0) break;
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

    /* Linear probe to find the key (bounded by capacity to prevent
     * infinite loops if the table is unexpectedly full) */
    for (size_t probe = 0; probe < map->capacity && map->entries[idx].key != 0; probe++) {
        if (map->entries[idx].key == key) {
            int class_idx = map->entries[idx].class_idx;
            /* Delete with backward-shift to maintain probe chains */
            size_t hole = idx;
            size_t next = (hole + 1) & mask;
            for (size_t shift = 0; shift < map->capacity && map->entries[next].key != 0; shift++) {
                size_t natural = buf_map_hash(map->entries[next].key, mask);
                /* Backward-shift: move entry at 'next' to 'hole' if 'hole'
                 * lies on the probe path from 'natural' to 'next'.  Use
                 * circular distance to handle wrap-around correctly. */
                size_t dist_natural_to_next = (next - natural) & mask;
                size_t dist_natural_to_hole = (hole - natural) & mask;
                if (dist_natural_to_hole <= dist_natural_to_next) {
                    map->entries[hole] = map->entries[next];
                    hole = next;
                }
                next = (next + 1) & mask;
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
