// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file aura_otel.c
 * @brief OpenTelemetry OTLP/JSON metrics formatter for AuraIO
 */

#define _POSIX_C_SOURCE 199309L

#include "aura_otel.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Append formatted text to the output buffer, tracking position.
 * NOTE: Jumps to 'overflow' label on buffer exhaustion. */
#define OTEL_APPEND(...)                                   \
    do {                                                   \
        int _n = snprintf(pos, remain, __VA_ARGS__);       \
        if (_n < 0 || (size_t)_n >= remain) goto overflow; \
        pos += _n;                                         \
        remain -= (size_t)_n;                              \
        written += (size_t)_n;                             \
    } while (0)

/* Clamp non-finite doubles to 0.0 */
static inline double otel_finite(double v) {
    return isfinite(v) ? v : 0.0;
}

static uint64_t get_time_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int aura_metrics_otel(aura_engine_t *engine, char *buf, size_t buf_size) {
    if (!engine || !buf || buf_size == 0) return -1;

    char *pos = buf;
    size_t remain = buf_size;
    size_t written = 0;
    int ring_count = aura_get_ring_count(engine);
    aura_ring_stats_t *rs = NULL;
    uint64_t now_ns = get_time_nanos();
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%" PRIu64, now_ns);

    /* --- Aggregate engine stats --- */
    aura_stats_t stats;
    aura_get_stats(engine, &stats, sizeof(stats));

    /* --- Buffer pool stats --- */
    aura_buffer_stats_t bstats;
    aura_get_buffer_stats(engine, &bstats, sizeof(aura_buffer_stats_t));

    /* Begin ExportMetricsServiceRequest */
    OTEL_APPEND("{\"resourceMetrics\":[{\"resource\":{\"attributes\":["
                "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"aura\"}},"
                "{\"key\":\"aura.schema.version\",\"value\":{\"stringValue\":"
                "\"" AURA_OTEL_SCHEMA_VERSION "\"}},"
                "{\"key\":\"aura.schema.stability\",\"value\":{\"stringValue\":"
                "\"" AURA_OTEL_SCHEMA_STABILITY "\"}}"
                "]},\"scopeMetrics\":[{\"scope\":{\"name\":\"aura.engine\",\"version\":\"%s\"},"
                "\"metrics\":[",
                aura_version());

    /* --- Aggregate monotonic sums --- */

    /* ops_completed */
    OTEL_APPEND("{\"name\":\"aura.ops.completed\",\"unit\":\"{operation}\","
                "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                "\"dataPoints\":[{\"asInt\":\"%" PRId64 "\","
                "\"startTimeUnixNano\":\"0\",\"timeUnixNano\":\"%s\"}]}}",
                stats.ops_completed, time_str);

    /* bytes_transferred */
    OTEL_APPEND(",{\"name\":\"aura.bytes.transferred\",\"unit\":\"By\","
                "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                "\"dataPoints\":[{\"asInt\":\"%" PRId64 "\","
                "\"startTimeUnixNano\":\"0\",\"timeUnixNano\":\"%s\"}]}}",
                stats.bytes_transferred, time_str);

    /* adaptive_spills */
    OTEL_APPEND(",{\"name\":\"aura.adaptive.spills\",\"unit\":\"{operation}\","
                "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                "\"dataPoints\":[{\"asInt\":\"%" PRIu64 "\","
                "\"startTimeUnixNano\":\"0\",\"timeUnixNano\":\"%s\"}]}}",
                stats.adaptive_spills, time_str);

    /* --- Aggregate gauges --- */

    /* throughput */
    OTEL_APPEND(",{\"name\":\"aura.throughput\",\"unit\":\"By/s\","
                "\"gauge\":{\"dataPoints\":[{\"asDouble\":%.1f,"
                "\"timeUnixNano\":\"%s\"}]}}",
                otel_finite(stats.current_throughput_bps), time_str);

    /* p99_latency (convert ms to seconds) */
    OTEL_APPEND(",{\"name\":\"aura.p99_latency\",\"unit\":\"s\","
                "\"gauge\":{\"dataPoints\":[{\"asDouble\":%.6f,"
                "\"timeUnixNano\":\"%s\"}]}}",
                otel_finite(stats.p99_latency_ms / 1000.0), time_str);

    /* in_flight */
    OTEL_APPEND(",{\"name\":\"aura.in_flight\",\"unit\":\"{operation}\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%d\","
                "\"timeUnixNano\":\"%s\"}]}}",
                stats.current_in_flight, time_str);

    /* optimal_in_flight */
    OTEL_APPEND(",{\"name\":\"aura.optimal_in_flight\",\"unit\":\"{operation}\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%d\","
                "\"timeUnixNano\":\"%s\"}]}}",
                stats.optimal_in_flight, time_str);

    /* optimal_batch_size */
    OTEL_APPEND(",{\"name\":\"aura.optimal_batch_size\",\"unit\":\"{operation}\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%d\","
                "\"timeUnixNano\":\"%s\"}]}}",
                stats.optimal_batch_size, time_str);

    /* ring_count */
    OTEL_APPEND(",{\"name\":\"aura.ring_count\",\"unit\":\"{ring}\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%d\","
                "\"timeUnixNano\":\"%s\"}]}}",
                ring_count, time_str);

    /* --- Buffer pool gauges --- */

    OTEL_APPEND(",{\"name\":\"aura.buffer_pool.allocated\",\"unit\":\"By\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%zu\","
                "\"timeUnixNano\":\"%s\"}]}}",
                bstats.total_allocated_bytes, time_str);

    OTEL_APPEND(",{\"name\":\"aura.buffer_pool.buffers\",\"unit\":\"{buffer}\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%zu\","
                "\"timeUnixNano\":\"%s\"}]}}",
                bstats.total_buffers, time_str);

    OTEL_APPEND(",{\"name\":\"aura.buffer_pool.shards\",\"unit\":\"{shard}\","
                "\"gauge\":{\"dataPoints\":[{\"asInt\":\"%d\","
                "\"timeUnixNano\":\"%s\"}]}}",
                bstats.shard_count, time_str);

    /* --- Per-ring metrics --- */
    if (ring_count > 0) {
        rs = malloc((size_t)ring_count * sizeof(*rs));
        if (!rs) {
            errno = ENOMEM;
            return -1;
        }

        for (int i = 0; i < ring_count; i++) {
            aura_get_ring_stats(engine, i, &rs[i], sizeof(rs[i]));
        }

        /* Per-ring ops_completed (sum with ring attribute) */
        OTEL_APPEND(",{\"name\":\"aura.ring.ops.completed\",\"unit\":\"{operation}\","
                    "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                    "\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%" PRId64 "\","
                        "\"startTimeUnixNano\":\"0\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].ops_completed, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring bytes_transferred */
        OTEL_APPEND(",{\"name\":\"aura.ring.bytes.transferred\",\"unit\":\"By\","
                    "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
                    "\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%" PRId64 "\","
                        "\"startTimeUnixNano\":\"0\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].bytes_transferred, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring in_flight gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.in_flight\",\"unit\":\"{operation}\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%d\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].pending_count, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring in_flight_limit gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.in_flight_limit\",\"unit\":\"{operation}\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%d\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].in_flight_limit, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring batch_threshold gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.batch_threshold\",\"unit\":\"{operation}\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%d\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].batch_threshold, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring queue_depth gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.queue_depth\",\"unit\":\"{operation}\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%d\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].queue_depth, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring p99_latency gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.p99_latency\",\"unit\":\"s\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asDouble\":%.6f,\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        otel_finite(rs[i].p99_latency_ms / 1000.0), time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring throughput gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.throughput\",\"unit\":\"By/s\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asDouble\":%.1f,\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        otel_finite(rs[i].throughput_bps), time_str, i);
        }
        OTEL_APPEND("]}}");

        /* Per-ring aimd_phase gauge */
        OTEL_APPEND(",{\"name\":\"aura.ring.aimd_phase\",\"unit\":\"{phase}\","
                    "\"gauge\":{\"dataPoints\":[");
        for (int i = 0; i < ring_count; i++) {
            if (i > 0) OTEL_APPEND(",");
            OTEL_APPEND("{\"asInt\":\"%d\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        rs[i].aimd_phase, time_str, i);
        }
        OTEL_APPEND("]}}");

        /* --- Per-ring latency histogram --- */
        OTEL_APPEND(",{\"name\":\"aura.latency\",\"unit\":\"s\","
                    "\"histogram\":{\"aggregationTemporality\":2,"
                    "\"dataPoints\":[");

        for (int i = 0; i < ring_count; i++) {
            aura_histogram_t hist;
            aura_get_histogram(engine, i, &hist, sizeof(hist));

            if (i > 0) OTEL_APPEND(",");

            /* Compute non-cumulative bucket counts, sum, min, max, count */
            uint64_t total_count = 0;
            double sum_estimate = 0.0;
            double min_val = -1.0;
            double max_val = 0.0;

            for (int b = 0; b < AURA_HISTOGRAM_BUCKETS; b++) {
                uint64_t c = hist.buckets[b];
                total_count += c;
                double midpoint_s = ((double)b + 0.5) * hist.bucket_width_us / 1e6;
                sum_estimate += c * midpoint_s;
                if (c > 0) {
                    double lo = (double)b * hist.bucket_width_us / 1e6;
                    double hi = (double)(b + 1) * hist.bucket_width_us / 1e6;
                    if (min_val < 0.0) min_val = lo;
                    max_val = hi;
                }
            }
            /* overflow bucket */
            total_count += hist.overflow;
            sum_estimate += hist.overflow * (hist.max_tracked_us / 1e6);
            if (hist.overflow > 0) {
                if (min_val < 0.0) min_val = hist.max_tracked_us / 1e6;
                max_val = hist.max_tracked_us / 1e6;
            }
            if (min_val < 0.0) min_val = 0.0;

            OTEL_APPEND("{\"count\":\"%" PRIu64 "\",\"sum\":%.6f,"
                        "\"min\":%.6f,\"max\":%.6f,",
                        total_count, sum_estimate, min_val, max_val);

            /* explicitBounds: N-1 boundaries for N+1 buckets
             * We emit boundaries for all histogram buckets + overflow.
             * Boundaries are the upper edge of each of the AURA_HISTOGRAM_BUCKETS buckets. */
            OTEL_APPEND("\"explicitBounds\":[");
            for (int b = 0; b < AURA_HISTOGRAM_BUCKETS; b++) {
                if (b > 0) OTEL_APPEND(",");
                OTEL_APPEND("%.6f", (double)(b + 1) * hist.bucket_width_us / 1e6);
            }
            OTEL_APPEND("],");

            /* bucketCounts: AURA_HISTOGRAM_BUCKETS + 1 (last is overflow) */
            OTEL_APPEND("\"bucketCounts\":[");
            for (int b = 0; b < AURA_HISTOGRAM_BUCKETS; b++) {
                if (b > 0) OTEL_APPEND(",");
                OTEL_APPEND("\"%" PRIu32 "\"", hist.buckets[b]);
            }
            OTEL_APPEND(",\"%" PRIu32 "\"],", hist.overflow);

            OTEL_APPEND("\"startTimeUnixNano\":\"0\",\"timeUnixNano\":\"%s\","
                        "\"attributes\":[{\"key\":\"ring\",\"value\":{\"intValue\":\"%d\"}}]}",
                        time_str, i);
        }

        OTEL_APPEND("]}}");
    }

    /* Close metrics array, scopeMetrics, resourceMetrics */
    OTEL_APPEND("]}]}]}");

    free(rs);
    return written > (size_t)INT_MAX ? INT_MAX : (int)written;

overflow:
    free(rs);
    errno = ENOBUFS;
    {
        size_t estimate = written * 2 + 8192;
        return estimate > (size_t)INT_MAX ? INT_MIN : -(int)estimate;
    }
}
