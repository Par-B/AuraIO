/**
 * @file auraio_prometheus.c
 * @brief Prometheus exposition text formatter for AuraIO metrics
 */

#include "auraio_prometheus.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Append formatted text to the output buffer, tracking position.
 * NOTE: Jumps to 'overflow' label on buffer exhaustion. */
#define PROM_APPEND(fmt, ...)                                         \
    do {                                                              \
        int _n = snprintf(pos, remain, fmt, ##__VA_ARGS__);           \
        if (_n < 0 || (size_t)_n >= remain) goto overflow;            \
        pos += _n;                                                    \
        remain -= (size_t)_n;                                         \
        written += _n;                                                \
    } while (0)

/* Clamp non-finite doubles to 0.0 to avoid emitting lowercase "nan"/"inf"
 * which violates Prometheus exposition format capitalization requirements. */
static inline double prom_finite(double v) {
    return isfinite(v) ? v : 0.0;
}

int auraio_metrics_prometheus(auraio_engine_t *engine, char *buf, size_t buf_size) {
    if (!engine || !buf || buf_size == 0) return -1;

    char *pos = buf;
    size_t remain = buf_size;
    int written = 0;
    int ring_count = auraio_get_ring_count(engine);
    auraio_ring_stats_t *rs = NULL;

    /* --- Aggregate engine stats --- */
    auraio_stats_t stats;
    auraio_get_stats(engine, &stats);

    PROM_APPEND(
        "# HELP auraio_ops_completed_total Total I/O operations completed\n"
        "# TYPE auraio_ops_completed_total counter\n"
        "auraio_ops_completed_total %" PRId64 "\n\n",
        stats.ops_completed);

    PROM_APPEND(
        "# HELP auraio_bytes_transferred_total Total bytes transferred\n"
        "# TYPE auraio_bytes_transferred_total counter\n"
        "auraio_bytes_transferred_total %" PRId64 "\n\n",
        stats.bytes_transferred);

    PROM_APPEND(
        "# HELP auraio_throughput_bytes_per_second Current aggregate throughput\n"
        "# TYPE auraio_throughput_bytes_per_second gauge\n"
        "auraio_throughput_bytes_per_second %.1f\n\n",
        prom_finite(stats.current_throughput_bps));

    PROM_APPEND(
        "# HELP auraio_p99_latency_seconds Aggregate P99 latency\n"
        "# TYPE auraio_p99_latency_seconds gauge\n"
        "auraio_p99_latency_seconds %.6f\n\n",
        prom_finite(stats.p99_latency_ms / 1000.0));

    PROM_APPEND(
        "# HELP auraio_in_flight Current total in-flight operations\n"
        "# TYPE auraio_in_flight gauge\n"
        "auraio_in_flight %d\n\n",
        stats.current_in_flight);

    PROM_APPEND(
        "# HELP auraio_optimal_in_flight Aggregate AIMD-tuned in-flight limit\n"
        "# TYPE auraio_optimal_in_flight gauge\n"
        "auraio_optimal_in_flight %d\n\n",
        stats.optimal_in_flight);

    PROM_APPEND(
        "# HELP auraio_optimal_batch_size Aggregate AIMD-tuned batch size\n"
        "# TYPE auraio_optimal_batch_size gauge\n"
        "auraio_optimal_batch_size %d\n\n",
        stats.optimal_batch_size);

    PROM_APPEND(
        "# HELP auraio_ring_count Number of io_uring rings\n"
        "# TYPE auraio_ring_count gauge\n"
        "auraio_ring_count %d\n\n",
        ring_count);

    /* --- Per-ring stats (single pass per ring to minimize mutex traffic) --- */
    if (ring_count > 0) {
        rs = malloc((size_t)ring_count * sizeof(*rs));
        if (!rs) goto overflow;

        for (int i = 0; i < ring_count; i++) {
            auraio_get_ring_stats(engine, i, &rs[i]);
        }

        PROM_APPEND(
            "# HELP auraio_ring_ops_completed_total Operations completed per ring\n"
            "# TYPE auraio_ring_ops_completed_total counter\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_ops_completed_total{ring=\"%d\"} %" PRId64 "\n",
                         i, rs[i].ops_completed);
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_bytes_transferred_total Bytes transferred per ring\n"
            "# TYPE auraio_ring_bytes_transferred_total counter\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_bytes_transferred_total{ring=\"%d\"} %" PRId64 "\n",
                         i, rs[i].bytes_transferred);
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_in_flight Current in-flight operations per ring\n"
            "# TYPE auraio_ring_in_flight gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_in_flight{ring=\"%d\"} %d\n",
                         i, rs[i].pending_count);
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_in_flight_limit AIMD-tuned in-flight limit per ring\n"
            "# TYPE auraio_ring_in_flight_limit gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_in_flight_limit{ring=\"%d\"} %d\n",
                         i, rs[i].in_flight_limit);
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_batch_threshold AIMD-tuned batch threshold per ring\n"
            "# TYPE auraio_ring_batch_threshold gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_batch_threshold{ring=\"%d\"} %d\n",
                         i, rs[i].batch_threshold);
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_queue_depth Kernel queue depth per ring\n"
            "# TYPE auraio_ring_queue_depth gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_queue_depth{ring=\"%d\"} %d\n",
                         i, rs[i].queue_depth);
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_p99_latency_seconds P99 latency per ring\n"
            "# TYPE auraio_ring_p99_latency_seconds gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_p99_latency_seconds{ring=\"%d\"} %.6f\n",
                         i, prom_finite(rs[i].p99_latency_ms / 1000.0));
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_throughput_bytes_per_second Throughput per ring\n"
            "# TYPE auraio_ring_throughput_bytes_per_second gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_throughput_bytes_per_second{ring=\"%d\"} %.1f\n",
                         i, prom_finite(rs[i].throughput_bps));
        }
        PROM_APPEND("\n");

        PROM_APPEND(
            "# HELP auraio_ring_aimd_phase Current AIMD phase per ring "
            "(0=baseline,1=probing,2=steady,3=backoff,4=settling,5=converged)\n"
            "# TYPE auraio_ring_aimd_phase gauge\n");
        for (int i = 0; i < ring_count; i++) {
            PROM_APPEND("auraio_ring_aimd_phase{ring=\"%d\"} %d\n",
                         i, rs[i].aimd_phase);
        }
        PROM_APPEND("\n");

        /* --- Per-ring latency histogram --- */
        PROM_APPEND(
            "# HELP auraio_latency_seconds Latency distribution per ring "
            "(sum estimated from bucket midpoints)\n"
            "# TYPE auraio_latency_seconds histogram\n");

        for (int i = 0; i < ring_count; i++) {
            auraio_histogram_t hist;
            auraio_get_histogram(engine, i, &hist);

            /* Prometheus histograms require cumulative buckets */
            uint64_t cumulative = 0;
            double sum_estimate = 0.0;

            for (int b = 0; b < AURAIO_HISTOGRAM_BUCKETS; b++) {
                cumulative += hist.buckets[b];
                /* Estimate sum using bucket midpoint */
                double midpoint_s = ((double)b + 0.5) * hist.bucket_width_us / 1e6;
                sum_estimate += hist.buckets[b] * midpoint_s;

                /* Only emit non-zero cumulative buckets to keep output compact */
                if (cumulative > 0) {
                    double le = (double)(b + 1) * hist.bucket_width_us / 1e6;
                    PROM_APPEND(
                        "auraio_latency_seconds_bucket{ring=\"%d\",le=\"%.6f\"} %llu\n",
                        i, le, (unsigned long long)cumulative);
                }
            }

            /* +Inf bucket includes overflow */
            cumulative += hist.overflow;
            sum_estimate += hist.overflow * (hist.max_tracked_us / 1e6);

            PROM_APPEND(
                "auraio_latency_seconds_bucket{ring=\"%d\",le=\"+Inf\"} %llu\n",
                i, (unsigned long long)cumulative);
            PROM_APPEND(
                "auraio_latency_seconds_sum{ring=\"%d\"} %.6f\n",
                i, sum_estimate);
            /* Use cumulative (computed from buckets + overflow) as _count
             * to guarantee _count == le="+Inf" per Prometheus spec. */
            PROM_APPEND(
                "auraio_latency_seconds_count{ring=\"%d\"} %llu\n",
                i, (unsigned long long)cumulative);
        }
        PROM_APPEND("\n");
    }

    /* --- Buffer pool stats --- */
    auraio_buffer_stats_t bstats;
    auraio_get_buffer_stats(engine, &bstats);

    PROM_APPEND(
        "# HELP auraio_buffer_pool_allocated_bytes Total bytes allocated from buffer pool\n"
        "# TYPE auraio_buffer_pool_allocated_bytes gauge\n"
        "auraio_buffer_pool_allocated_bytes %zu\n\n",
        bstats.total_allocated_bytes);

    PROM_APPEND(
        "# HELP auraio_buffer_pool_buffers Total buffers allocated from pool\n"
        "# TYPE auraio_buffer_pool_buffers gauge\n"
        "auraio_buffer_pool_buffers %zu\n\n",
        bstats.total_buffers);

    PROM_APPEND(
        "# HELP auraio_buffer_pool_shards Number of buffer pool shards\n"
        "# TYPE auraio_buffer_pool_shards gauge\n"
        "auraio_buffer_pool_shards %d\n",
        bstats.shard_count);

    free(rs);
    return written;

overflow:
    free(rs);
    /* Return negative of conservative estimate of bytes needed.
     * Callers should retry in a loop with abs(return value) as the new size. */
    return -(written * 2 + 4096);
}
