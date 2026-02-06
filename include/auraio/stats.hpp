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
    [[nodiscard]] int64_t ops_completed() const noexcept {
        return stats_.ops_completed;
    }

    /**
     * Get total bytes transferred
     * @return Bytes read or written
     */
    [[nodiscard]] int64_t bytes_transferred() const noexcept {
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
    [[nodiscard]] const auraio_stats_t& c_stats() const& noexcept { return stats_; }
    [[nodiscard]] auraio_stats_t c_stats() const&& noexcept { return stats_; }

private:
    friend class Engine;

    auraio_stats_t stats_{};
};

/**
 * Per-ring statistics snapshot
 */
class RingStats {
public:
    [[nodiscard]] int64_t ops_completed() const noexcept { return stats_.ops_completed; }
    [[nodiscard]] int64_t bytes_transferred() const noexcept { return stats_.bytes_transferred; }
    [[nodiscard]] int pending_count() const noexcept { return stats_.pending_count; }
    [[nodiscard]] int in_flight_limit() const noexcept { return stats_.in_flight_limit; }
    [[nodiscard]] int batch_threshold() const noexcept { return stats_.batch_threshold; }
    [[nodiscard]] double p99_latency_ms() const noexcept { return stats_.p99_latency_ms; }
    [[nodiscard]] double throughput_bps() const noexcept { return stats_.throughput_bps; }
    [[nodiscard]] int aimd_phase() const noexcept { return stats_.aimd_phase; }
    [[nodiscard]] const char* aimd_phase_name() const noexcept {
        return auraio_phase_name(stats_.aimd_phase);
    }
    [[nodiscard]] int queue_depth() const noexcept { return stats_.queue_depth; }
    [[nodiscard]] int ring_index() const noexcept { return ring_idx_; }
    [[nodiscard]] const auraio_ring_stats_t& c_stats() const& noexcept { return stats_; }
    [[nodiscard]] auraio_ring_stats_t c_stats() const&& noexcept { return stats_; }

private:
    friend class Engine;
    auraio_ring_stats_t stats_{};
    int ring_idx_{-1};
};

/**
 * Latency histogram snapshot
 */
class Histogram {
public:
    static constexpr int bucket_count = AURAIO_HISTOGRAM_BUCKETS;
    static constexpr int bucket_width_us = AURAIO_HISTOGRAM_BUCKET_WIDTH_US;

    [[nodiscard]] uint32_t bucket(int idx) const noexcept {
        if (idx < 0 || idx >= bucket_count) return 0;
        return hist_.buckets[idx];
    }
    [[nodiscard]] uint32_t overflow() const noexcept { return hist_.overflow; }
    [[nodiscard]] uint32_t total_count() const noexcept { return hist_.total_count; }
    [[nodiscard]] int max_tracked_us() const noexcept { return hist_.max_tracked_us; }

    [[nodiscard]] int bucket_lower_us(int idx) const noexcept {
        if (idx < 0 || idx >= bucket_count) return 0;
        return idx * hist_.bucket_width_us;
    }
    [[nodiscard]] int bucket_upper_us(int idx) const noexcept {
        if (idx < 0 || idx >= bucket_count) return 0;
        return (idx + 1) * hist_.bucket_width_us;
    }

    [[nodiscard]] const auraio_histogram_t& c_histogram() const& noexcept { return hist_; }
    [[nodiscard]] auraio_histogram_t c_histogram() const&& noexcept { return hist_; }

private:
    friend class Engine;
    auraio_histogram_t hist_{};
};

/**
 * Buffer pool statistics snapshot
 */
class BufferStats {
public:
    [[nodiscard]] size_t total_allocated_bytes() const noexcept {
        return stats_.total_allocated_bytes;
    }
    [[nodiscard]] size_t total_buffers() const noexcept { return stats_.total_buffers; }
    [[nodiscard]] int shard_count() const noexcept { return stats_.shard_count; }
    [[nodiscard]] const auraio_buffer_stats_t& c_stats() const& noexcept { return stats_; }
    [[nodiscard]] auraio_buffer_stats_t c_stats() const&& noexcept { return stats_; }

private:
    friend class Engine;
    auraio_buffer_stats_t stats_{};
};

} // namespace auraio

#endif // AURAIO_STATS_HPP
