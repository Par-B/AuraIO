/**
 * @file stats.hpp
 * @brief Statistics class for AuraIO C++ bindings
 */

#ifndef AURAIO_STATS_HPP
#define AURAIO_STATS_HPP

#include <auraio.h>

namespace auraio {

/**
 * Engine statistics snapshot
 *
 * Provides read-only access to engine metrics.
 */
class Stats {
public:
    /**
     * Get total operations completed
     * @return Number of completed I/O operations
     */
    [[nodiscard]] long long ops_completed() const noexcept {
        return stats_.ops_completed;
    }

    /**
     * Get total bytes transferred
     * @return Bytes read or written
     */
    [[nodiscard]] long long bytes_transferred() const noexcept {
        return stats_.bytes_transferred;
    }

    /**
     * Get current throughput
     * @return Throughput in bytes per second
     */
    [[nodiscard]] double throughput_bps() const noexcept {
        return stats_.current_throughput_bps;
    }

    /**
     * Get P99 latency
     * @return 99th percentile latency in milliseconds
     */
    [[nodiscard]] double p99_latency_ms() const noexcept {
        return stats_.p99_latency_ms;
    }

    /**
     * Get current in-flight operation count
     * @return Number of currently in-flight operations
     */
    [[nodiscard]] int current_in_flight() const noexcept {
        return stats_.current_in_flight;
    }

    /**
     * Get optimal in-flight limit (tuned value)
     * @return Optimal in-flight limit determined by adaptive controller
     */
    [[nodiscard]] int optimal_in_flight() const noexcept {
        return stats_.optimal_in_flight;
    }

    /**
     * Get optimal batch size (tuned value)
     * @return Optimal batch size determined by adaptive controller
     */
    [[nodiscard]] int optimal_batch_size() const noexcept {
        return stats_.optimal_batch_size;
    }

    /**
     * Get underlying C stats struct
     * @return Reference to auraio_stats_t
     */
    [[nodiscard]] const auraio_stats_t& c_stats() const noexcept { return stats_; }

private:
    friend class Engine;

    auraio_stats_t stats_{};
};

} // namespace auraio

#endif // AURAIO_STATS_HPP
