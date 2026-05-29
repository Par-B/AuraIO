// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file internal.h
 * @brief Shared internal utilities
 *
 * Internal header - not part of public API.
 * Common utilities used across multiple internal modules.
 */

#ifndef AURA_INTERNAL_H
#define AURA_INTERNAL_H

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/uio.h>

/* ThreadSanitizer annotations for kernel-mediated synchronization.
 * io_uring's SQ→CQ path provides memory ordering guarantees that TSan
 * cannot observe. These annotations inform TSan of the happens-before
 * edge between submission and completion. */
#if defined(__SANITIZE_THREAD__)
#    define AURA_TSAN_ENABLED
#elif defined(__has_feature)
#    if __has_feature(thread_sanitizer)
#        define AURA_TSAN_ENABLED
#    endif
#endif

#ifdef AURA_TSAN_ENABLED
void __tsan_acquire(void *addr);
void __tsan_release(void *addr);
#    define TSAN_RELEASE(addr) __tsan_release(addr)
#    define TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#else
#    define TSAN_RELEASE(addr) ((void)(addr))
#    define TSAN_ACQUIRE(addr) ((void)(addr))
#endif

/**
 * Get monotonic time in nanoseconds.
 *
 * Uses CLOCK_MONOTONIC for consistent timing that is immune to
 * system clock adjustments.
 *
 * @return Current time in nanoseconds
 */
static inline int64_t get_time_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0; /* Should never happen for CLOCK_MONOTONIC on Linux */
    }
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/**
 * Calculate total bytes for vectored I/O.
 *
 * @param iov    Array of iovec structures
 * @param iovcnt Number of elements in array
 * @return Total bytes across all iovecs, or SIZE_MAX on overflow
 */
static inline size_t iovec_total_len(const struct iovec *iov, int iovcnt) {
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (total > SIZE_MAX - iov[i].iov_len) {
            return SIZE_MAX; /* Overflow - return max value */
        }
        total += iov[i].iov_len;
    }
    return total;
}

/**
 * Allocate zero-initialized memory honoring an over-aligned type's alignment.
 *
 * malloc()/calloc() only guarantee alignment up to max_align_t (typically 16),
 * but several engine structs use _Alignas(64) to avoid false sharing between
 * hot atomics. Accessing such a struct through an under-aligned pointer is
 * undefined behavior (flagged by UBSan). Use this for those allocations.
 *
 * @param alignment Required alignment in bytes (must be a power of two)
 * @param size      Number of bytes to allocate
 * @return Zeroed, suitably-aligned pointer, or NULL on failure (errno set).
 *         Free with free().
 */
static inline void *aura_aligned_calloc(size_t alignment, size_t size) {
    if (alignment == 0) {
        alignment = 1;
    }
    /* aligned_alloc requires the size to be a multiple of the alignment. */
    size_t rem = size % alignment;
    if (rem != 0) {
        if (size > SIZE_MAX - (alignment - rem)) {
            errno = ENOMEM;
            return NULL;
        }
        size += alignment - rem;
    }
    void *p = aligned_alloc(alignment, size);
    if (p) {
        memset(p, 0, size);
    }
    return p;
}

#endif /* AURA_INTERNAL_H */
