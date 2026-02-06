/**
 * @file internal.h
 * @brief Shared internal utilities
 *
 * Internal header - not part of public API.
 * Common utilities used across multiple internal modules.
 */

#ifndef AURAIO_INTERNAL_H
#define AURAIO_INTERNAL_H

#include <stdint.h>
#include <time.h>
#include <sys/uio.h>

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
        return 0;  /* Should never happen for CLOCK_MONOTONIC on Linux */
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
            return SIZE_MAX;  /* Overflow - return max value */
        }
        total += iov[i].iov_len;
    }
    return total;
}

#endif /* AURAIO_INTERNAL_H */
