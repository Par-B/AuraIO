/**
 * @file test_buffer.c
 * @brief Unit tests for buffer pool
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <assert.h>
#include <errno.h>

#include "../src/adaptive_buffer.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name) do { \
    printf("  %-40s", #name); \
    test_##name(); \
    printf(" OK\n"); \
    test_count++; \
} while(0)

/* ============================================================================
 * Buffer Pool Tests
 * ============================================================================ */

TEST(pool_init) {
    buffer_pool_t pool;

    int ret = buffer_pool_init(&pool, 4096);
    assert(ret == 0);
    assert(pool.alignment == 4096);
    assert(pool.total_buffers == 0);

    buffer_pool_destroy(&pool);
}

TEST(pool_init_invalid) {
    buffer_pool_t pool;

    /* NULL pool */
    assert(buffer_pool_init(NULL, 4096) == -1);

    /* Zero alignment */
    assert(buffer_pool_init(&pool, 0) == -1);
}

TEST(pool_alloc_basic) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    void *buf = buffer_pool_alloc(&pool, 1024);
    assert(buf != NULL);
    assert(pool.total_buffers == 1);

    /* Check alignment */
    assert(((uintptr_t)buf % 4096) == 0);

    /* Can write to buffer */
    memset(buf, 0xAB, 1024);

    buffer_pool_free(&pool, buf, 1024);
    buffer_pool_destroy(&pool);
}

TEST(pool_alloc_multiple) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    void *bufs[10];
    for (int i = 0; i < 10; i++) {
        bufs[i] = buffer_pool_alloc(&pool, 4096);
        assert(bufs[i] != NULL);
        assert(((uintptr_t)bufs[i] % 4096) == 0);
    }

    assert(pool.total_buffers == 10);

    /* Free all */
    for (int i = 0; i < 10; i++) {
        buffer_pool_free(&pool, bufs[i], 4096);
    }

    buffer_pool_destroy(&pool);
}

TEST(pool_reuse) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    /* Allocate and free */
    void *buf1 = buffer_pool_alloc(&pool, 4096);
    buffer_pool_free(&pool, buf1, 4096);

    /* Allocate again - should reuse */
    void *buf2 = buffer_pool_alloc(&pool, 4096);
    assert(buf2 == buf1);  /* Same buffer reused */

    buffer_pool_free(&pool, buf2, 4096);
    buffer_pool_destroy(&pool);
}

TEST(pool_different_sizes) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    void *buf_small = buffer_pool_alloc(&pool, 1024);
    void *buf_large = buffer_pool_alloc(&pool, 1024 * 1024);

    assert(buf_small != NULL);
    assert(buf_large != NULL);
    assert(buf_small != buf_large);

    buffer_pool_free(&pool, buf_small, 1024);
    buffer_pool_free(&pool, buf_large, 1024 * 1024);

    /* Allocate small again - should get cached small buffer */
    void *buf_small2 = buffer_pool_alloc(&pool, 1024);
    assert(buf_small2 == buf_small);

    buffer_pool_free(&pool, buf_small2, 1024);
    buffer_pool_destroy(&pool);
}

TEST(pool_alloc_invalid) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    /* NULL pool */
    errno = 0;
    assert(buffer_pool_alloc(NULL, 1024) == NULL);
    assert(errno == EINVAL);

    /* Zero size */
    errno = 0;
    assert(buffer_pool_alloc(&pool, 0) == NULL);
    assert(errno == EINVAL);

    /* Oversized buffer (> 128MB max) */
    errno = 0;
    assert(buffer_pool_alloc(&pool, BUFFER_POOL_MAX_SIZE + 1) == NULL);
    assert(errno == EINVAL);

    /* Exactly at max should succeed */
    void *buf = buffer_pool_alloc(&pool, BUFFER_POOL_MAX_SIZE);
    assert(buf != NULL);
    buffer_pool_free(&pool, buf, BUFFER_POOL_MAX_SIZE);

    buffer_pool_destroy(&pool);
}

TEST(pool_free_null) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    /* Should not crash */
    buffer_pool_free(&pool, NULL, 1024);
    buffer_pool_free(NULL, NULL, 0);

    buffer_pool_destroy(&pool);
}

TEST(pool_destroy_with_cached) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    /* Allocate and free some buffers (leave in cache) */
    for (int i = 0; i < 5; i++) {
        void *buf = buffer_pool_alloc(&pool, 4096);
        buffer_pool_free(&pool, buf, 4096);
    }

    /* Destroy should clean up cached buffers */
    buffer_pool_destroy(&pool);
    /* No memory leak if valgrind passes */
}

/* ============================================================================
 * Size Class Tests
 * ============================================================================ */

TEST(size_class_mapping) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);

    /* Test that buffers of same size class are reused.
     * Size classes are power-of-2 buckets starting at 4KB.
     * Class 0: <= 4KB, Class 1: <= 8KB, Class 2: <= 16KB, etc. */

    /* Allocate and free a 4KB buffer */
    void *buf_4k = buffer_pool_alloc(&pool, 4096);
    buffer_pool_free(&pool, buf_4k, 4096);

    /* Allocate 4KB again - should get same buffer (same class) */
    void *buf_4k_2 = buffer_pool_alloc(&pool, 4096);
    assert(buf_4k_2 == buf_4k);
    buffer_pool_free(&pool, buf_4k_2, 4096);

    /* Allocate 8KB - different class, should NOT get 4KB buffer */
    void *buf_8k = buffer_pool_alloc(&pool, 8192);
    assert(buf_8k != buf_4k);  /* Different buffer (different class) */
    buffer_pool_free(&pool, buf_8k, 8192);

    /* Allocate 8KB again - should reuse */
    void *buf_8k_2 = buffer_pool_alloc(&pool, 8192);
    assert(buf_8k_2 == buf_8k);
    buffer_pool_free(&pool, buf_8k_2, 8192);

    /* Verify larger sizes go to different classes */
    void *buf_16k = buffer_pool_alloc(&pool, 16384);
    void *buf_32k = buffer_pool_alloc(&pool, 32768);
    void *buf_64k = buffer_pool_alloc(&pool, 65536);

    assert(buf_16k != buf_8k);
    assert(buf_32k != buf_16k);
    assert(buf_64k != buf_32k);

    buffer_pool_free(&pool, buf_16k, 16384);
    buffer_pool_free(&pool, buf_32k, 32768);
    buffer_pool_free(&pool, buf_64k, 65536);

    /* Each should be reused from its class */
    void *buf_16k_2 = buffer_pool_alloc(&pool, 16384);
    void *buf_32k_2 = buffer_pool_alloc(&pool, 32768);
    void *buf_64k_2 = buffer_pool_alloc(&pool, 65536);

    assert(buf_16k_2 == buf_16k);
    assert(buf_32k_2 == buf_32k);
    assert(buf_64k_2 == buf_64k);

    buffer_pool_free(&pool, buf_16k_2, 16384);
    buffer_pool_free(&pool, buf_32k_2, 32768);
    buffer_pool_free(&pool, buf_64k_2, 65536);

    buffer_pool_destroy(&pool);
}

/* ============================================================================
 * Concurrent Stress Tests
 * ============================================================================ */

#include <pthread.h>

#define STRESS_THREADS 4
#define STRESS_ITERATIONS 1000
#define STRESS_SIZES_COUNT 4

static buffer_pool_t *stress_pool;
static const size_t stress_sizes[STRESS_SIZES_COUNT] = {4096, 8192, 16384, 65536};

static void *stress_thread_func(void *arg) {
    int thread_id = *(int *)arg;
    void *buffers[10];
    size_t buf_sizes[10];  /* Track allocation size for correct free */
    int buf_count = 0;

    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        /* Pick a size based on iteration */
        size_t size = stress_sizes[(thread_id + i) % STRESS_SIZES_COUNT];

        /* Allocate */
        if (buf_count < 10) {
            void *buf = buffer_pool_alloc(stress_pool, size);
            if (buf) {
                /* Write to verify it's valid memory */
                memset(buf, (unsigned char)(thread_id + i), size < 256 ? size : 256);
                buffers[buf_count] = buf;
                buf_sizes[buf_count] = size;
                buf_count++;
            }
        }

        /* Randomly free some */
        if (buf_count > 5 || (i % 3 == 0 && buf_count > 0)) {
            buf_count--;
            buffer_pool_free(stress_pool, buffers[buf_count], buf_sizes[buf_count]);
        }
    }

    /* Free remaining */
    while (buf_count > 0) {
        buf_count--;
        buffer_pool_free(stress_pool, buffers[buf_count], buf_sizes[buf_count]);
    }

    return NULL;
}

TEST(pool_concurrent_stress) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);
    stress_pool = &pool;

    pthread_t threads[STRESS_THREADS];
    int thread_ids[STRESS_THREADS];

    /* Start threads */
    for (int i = 0; i < STRESS_THREADS; i++) {
        thread_ids[i] = i;
        int ret = pthread_create(&threads[i], NULL, stress_thread_func, &thread_ids[i]);
        assert(ret == 0);
    }

    /* Wait for completion */
    for (int i = 0; i < STRESS_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    buffer_pool_destroy(&pool);
}

/* ============================================================================
 * Thread-Local Cache Tests
 * ============================================================================ */

#define CACHE_TEST_THREADS 8
#define CACHE_TEST_ITERATIONS 5000

static buffer_pool_t *cache_test_pool;
static _Atomic int cache_test_ops_completed;

/**
 * Thread function for high-contention cache test.
 *
 * Each thread rapidly allocates and frees buffers of the same size class.
 * With per-thread caches, most operations should hit the local cache
 * without needing the global lock.
 */
static void *cache_contention_thread(void *arg) {
    (void)arg;

    for (int i = 0; i < CACHE_TEST_ITERATIONS; i++) {
        /* Allocate a 4KB buffer */
        void *buf = buffer_pool_alloc(cache_test_pool, 4096);
        assert(buf != NULL);

        /* Touch it to ensure it's valid */
        ((char *)buf)[0] = (char)i;

        /* Free immediately - this should go to thread-local cache */
        buffer_pool_free(cache_test_pool, buf, 4096);

        atomic_fetch_add(&cache_test_ops_completed, 1);
    }

    return NULL;
}

TEST(pool_thread_cache_contention) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);
    cache_test_pool = &pool;
    atomic_store(&cache_test_ops_completed, 0);

    pthread_t threads[CACHE_TEST_THREADS];

    /* Start threads */
    for (int i = 0; i < CACHE_TEST_THREADS; i++) {
        int ret = pthread_create(&threads[i], NULL, cache_contention_thread, NULL);
        assert(ret == 0);
    }

    /* Wait for completion */
    for (int i = 0; i < CACHE_TEST_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Verify all operations completed */
    int expected_ops = CACHE_TEST_THREADS * CACHE_TEST_ITERATIONS;
    assert(atomic_load(&cache_test_ops_completed) == expected_ops);

    buffer_pool_destroy(&pool);
}

/**
 * Test that thread-local cache properly handles multiple size classes.
 */
static void *cache_multisize_thread(void *arg) {
    int thread_id = *(int *)arg;
    void *bufs[4];

    for (int i = 0; i < 500; i++) {
        /* Allocate different sizes */
        bufs[0] = buffer_pool_alloc(cache_test_pool, 4096);   /* Class 0 */
        bufs[1] = buffer_pool_alloc(cache_test_pool, 8192);   /* Class 1 */
        bufs[2] = buffer_pool_alloc(cache_test_pool, 16384);  /* Class 2 */
        bufs[3] = buffer_pool_alloc(cache_test_pool, 32768);  /* Class 3 */

        /* Touch each to ensure valid */
        for (int j = 0; j < 4; j++) {
            assert(bufs[j] != NULL);
            ((char *)bufs[j])[0] = (char)(thread_id + i + j);
        }

        /* Free in different order */
        buffer_pool_free(cache_test_pool, bufs[2], 16384);
        buffer_pool_free(cache_test_pool, bufs[0], 4096);
        buffer_pool_free(cache_test_pool, bufs[3], 32768);
        buffer_pool_free(cache_test_pool, bufs[1], 8192);
    }

    return NULL;
}

TEST(pool_thread_cache_multisize) {
    buffer_pool_t pool;
    buffer_pool_init(&pool, 4096);
    cache_test_pool = &pool;

    pthread_t threads[CACHE_TEST_THREADS];
    int thread_ids[CACHE_TEST_THREADS];

    /* Start threads */
    for (int i = 0; i < CACHE_TEST_THREADS; i++) {
        thread_ids[i] = i;
        int ret = pthread_create(&threads[i], NULL, cache_multisize_thread, &thread_ids[i]);
        assert(ret == 0);
    }

    /* Wait for completion */
    for (int i = 0; i < CACHE_TEST_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    buffer_pool_destroy(&pool);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("Running buffer pool tests...\n");

    RUN_TEST(pool_init);
    RUN_TEST(pool_init_invalid);
    RUN_TEST(pool_alloc_basic);
    RUN_TEST(pool_alloc_multiple);
    RUN_TEST(pool_reuse);
    RUN_TEST(pool_different_sizes);
    RUN_TEST(pool_alloc_invalid);
    RUN_TEST(pool_free_null);
    RUN_TEST(pool_destroy_with_cached);
    RUN_TEST(size_class_mapping);
    RUN_TEST(pool_concurrent_stress);
    RUN_TEST(pool_thread_cache_contention);
    RUN_TEST(pool_thread_cache_multisize);

    printf("\n%d tests passed\n", test_count);
    return 0;
}
