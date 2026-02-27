# AuraIO Observability Guide

## Overview

AuraIO exposes a three-tier metrics API for monitoring I/O performance in production:

| Tier | Scope | Functions |
|------|-------|-----------|
| **Aggregate** | Engine-wide totals | `aura_get_stats()` |
| **Per-Ring** | Individual io_uring ring + AIMD controller state | `aura_get_ring_stats()`, `aura_get_histogram()` |
| **Buffer Pool** | Memory allocator health | `aura_get_buffer_stats()` |

All stats functions are **thread-safe** and safe to call from any thread at any time, including during active I/O.

## C API Reference

### Aggregate Stats

```c
aura_stats_t stats;
aura_get_stats(engine, &stats, sizeof(stats));

printf("IOPS: %.0f, Throughput: %.1f MB/s, P99: %.2f ms\n",
       stats.ops_completed / elapsed,
       stats.current_throughput_bps / (1024 * 1024),
       stats.p99_latency_ms);
```

Fields in `aura_stats_t`:

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed across all rings |
| `bytes_transferred` | `int64_t` | Total bytes read/written across all rings |
| `current_throughput_bps` | `double` | Current aggregate throughput (bytes/sec) |
| `p99_latency_ms` | `double` | 99th percentile latency (ms) |
| `current_in_flight` | `int` | Current total in-flight operations |
| `optimal_in_flight` | `int` | AIMD-tuned aggregate in-flight limit |
| `peak_in_flight` | `int` | High-water mark of in-flight operations across all rings |
| `optimal_batch_size` | `int` | AIMD-tuned aggregate batch size |
| `adaptive_spills` | `uint64_t` | ADAPTIVE mode: count of submissions that spilled to a non-local ring |

### Per-Ring Stats

```c
int rings = aura_get_ring_count(engine);

for (int i = 0; i < rings; i++) {
    aura_ring_stats_t rs;
    aura_get_ring_stats(engine, i, &rs, sizeof(rs));

    printf("Ring %d: phase=%s, in_flight=%d/%d, p99=%.2fms, %.1f MB/s\n",
           i, aura_phase_name(rs.aimd_phase),
           rs.pending_count, rs.in_flight_limit,
           rs.p99_latency_ms, rs.throughput_bps / (1024 * 1024));
}
```

Fields in `aura_ring_stats_t`:

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed on this ring |
| `bytes_transferred` | `int64_t` | Total bytes transferred through this ring |
| `pending_count` | `int` | Current in-flight operations |
| `peak_in_flight` | `int` | High-water mark of in-flight operations for this ring |
| `in_flight_limit` | `int` | AIMD-tuned maximum in-flight |
| `batch_threshold` | `int` | AIMD-tuned batch size |
| `p99_latency_ms` | `double` | Current P99 latency estimate (ms) |
| `throughput_bps` | `double` | Current throughput (bytes/sec) |
| `aimd_phase` | `int` | Controller phase (0-6, see table below) |
| `queue_depth` | `int` | Maximum queue depth for this ring |

AIMD phases (via `aura_phase_name()`):

| Value | Name | Meaning |
|-------|------|---------|
| 0 | BASELINE | Initial ramp-up, collecting first samples |
| 1 | PROBING | Additive increase — testing higher depth |
| 2 | STEADY | Throughput plateau — holding position |
| 3 | BACKOFF | Multiplicative decrease — P99 exceeded target |
| 4 | SETTLING | Post-backoff stabilization |
| 5 | CONVERGED | Optimal depth found, minor adjustments only |
| 6 | PASSTHROUGH | No AIMD gating — default start state, near-zero overhead |

### Latency Histogram

```c
aura_histogram_t hist;
aura_get_histogram(engine, ring_idx, &hist, sizeof(hist));

// Recommended: use aura_histogram_percentile() for accurate results
double p50 = aura_histogram_percentile(&hist, 50.0);
double p99 = aura_histogram_percentile(&hist, 99.0);
double p999 = aura_histogram_percentile(&hist, 99.9);
printf("  P50: %.2f ms, P99: %.2f ms, P99.9: %.2f ms\n", p50, p99, p999);

// Optional: iterate buckets for detailed histogram view (advanced use)
for (int b = 0; b < AURA_HISTOGRAM_BUCKETS; b++) {
    if (hist.buckets[b] > 0) {
        int upper = aura_histogram_bucket_upper_bound_us(&hist, b);
        int lower = upper - hist.tier_width_us[hist.tier_count - 1];  // approx for display
        printf("  0-%d us: %u ops\n", upper, hist.buckets[b]);
    }
}
printf("  >%d us (overflow): %u ops\n", hist.max_tracked_us, hist.overflow);
printf("  total: %u ops\n", hist.total_count);
```

#### `aura_histogram_percentile()` -- Recommended Percentile API

```c
double aura_histogram_percentile(const aura_histogram_t *hist, double percentile);
```

Returns the estimated latency in **milliseconds** for the given percentile (0-100). This is the **preferred API** for computing percentiles from histograms. Returns -1.0 if the histogram is empty or the percentile is out of range.

The histogram is an **approximate snapshot of the active window** — AuraIO double-buffers histograms internally and swaps them periodically. Because the snapshot is read from a concurrently-written histogram, individual bucket values are atomic but `total_count` may differ slightly from the sum of all buckets + overflow. For monitoring purposes this is negligible.

When adaptive tuning is disabled (`disable_adaptive = true`), the histogram is not periodically reset and accumulates data indefinitely.

**Histogram Coverage:** 0–100 milliseconds in 4 tiers (10 µs, 50 µs, 250 µs, 1 ms bucket widths). Operations exceeding 100 ms are counted in `overflow`.

### Buffer Pool Stats

```c
aura_buffer_stats_t bs;
aura_get_buffer_stats(engine, &bs, sizeof(aura_buffer_stats_t));

printf("Pool: %zu buffers, %zu bytes across %d shards\n",
       bs.total_buffers, bs.total_allocated_bytes, bs.shard_count);
```

## Binding Coverage

| Feature | C | C++ | Rust |
|---------|---|-----|------|
| Aggregate stats | `aura_get_stats()` | `engine.get_stats()` | `engine.stats()` |
| Ring count | `aura_get_ring_count()` | `engine.ring_count()` | `engine.ring_count()` |
| Per-ring stats | `aura_get_ring_stats()` | `engine.get_ring_stats(i)` | `engine.ring_stats(i)` |
| Latency histogram | `aura_get_histogram()` | `engine.get_histogram(i)` | `engine.histogram(i)` |
| Buffer pool stats | `aura_get_buffer_stats()` | `engine.get_buffer_stats()` | `engine.buffer_stats()` |
| Phase name | `aura_phase_name()` | `ring_stats.aimd_phase_name()` | `ring_stats.aimd_phase_name()` |

The C++ bindings are header-only and fully inline — no additional link dependencies.

## C++ Usage

```cpp
#include <aura.hpp>

aura::Engine engine;

// Aggregate
auto stats = engine.get_stats();
std::cout << stats.throughput_bps() << " bytes/sec\n";

// Per-ring
for (int i = 0; i < engine.ring_count(); i++) {
    auto rs = engine.get_ring_stats(i);
    std::cout << "Ring " << i << ": " << rs.aimd_phase_name()
              << " depth=" << rs.in_flight_limit() << "\n";

    auto hist = engine.get_histogram(i);

    // Recommended: use percentile() for accurate latency metrics
    auto p50 = hist.percentile(50.0);
    auto p99 = hist.percentile(99.0);
    std::cout << "  P50: " << p50.value_or(-1.0) << "ms, "
              << "P99: " << p99.value_or(-1.0) << "ms\n";

    // Optional: iterate buckets for detailed histogram (advanced use)
    for (int b = 0; b < aura::Histogram::bucket_count && b < 20; b++) {
        if (hist.bucket(b) > 0)
            std::cout << "    0-" << hist.bucket_upper_us(b) << "us: "
                      << hist.bucket(b) << "\n";
    }
}

// Buffer pool
auto bs = engine.get_buffer_stats();
std::cout << bs.total_buffers() << " buffers in " << bs.shard_count() << " shards\n";
```

## Prometheus Integration

AuraIO ships a standalone Prometheus exposition text formatter in `integrations/prometheus/C/`. It has **no external dependencies** beyond libc.

Before `1.0`, the exporter schema is explicitly versioned and marked experimental.

**Schema versioning:** A schema info metric is emitted (`aura_metrics_schema_info{schema="v0",stability="experimental"} 1`). Before `1.0`, new metrics may be added and labels may change. At `1.0`, the schema transitions to stable and breaking metric changes require a major-version bump.

**Naming rules:** All metric names use the `aura_` prefix. Labels are kept bounded and operationally meaningful — no high-cardinality labels (`req_id`, raw fd/offset, `user_data`).

**Data consistency:** Observability reads are intentionally lock-free best-effort snapshots. Values may be updated while being read, and minor cross-field skew is expected. Metrics are intended for trends and alerting, not strict accounting.

**Change process:** Any change to exporter metric names or labels requires a `CHANGELOG.md` entry and a documentation update.

### Building

```bash
make exporters
```

### Using the Formatter in Your Application

The formatter writes Prometheus exposition text into a user-provided buffer:

```c
#include "aura_prometheus.h"

char buf[256 * 1024];
int len = aura_metrics_prometheus(engine, buf, sizeof(buf));
if (len < 0) {
    // Check errno: ENOBUFS means buffer too small (abs(len) is a minimum size estimate);
    // ENOMEM means a hard allocation failure occurred.
}
// Write buf[0..len] as HTTP response body with
// Content-Type: text/plain; version=0.0.4; charset=utf-8
```

Integrate this into whichever HTTP server you already run — there is no built-in HTTP listener in the library.

### Exported Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `aura_ops_completed_total` | counter | — | Total I/O operations completed |
| `aura_metrics_schema_info` | gauge | `schema`, `stability` | Exporter schema version/status (`v0`, `experimental` before 1.0) |
| `aura_bytes_transferred_total` | counter | — | Total bytes transferred |
| `aura_throughput_bytes_per_second` | gauge | — | Current aggregate throughput |
| `aura_p99_latency_seconds` | gauge | — | Aggregate P99 latency |
| `aura_in_flight` | gauge | — | Current total in-flight operations |
| `aura_optimal_in_flight` | gauge | — | AIMD-tuned aggregate in-flight limit |
| `aura_optimal_batch_size` | gauge | — | AIMD-tuned aggregate batch size |
| `aura_ring_count` | gauge | — | Number of io_uring rings |
| `aura_ring_ops_completed_total` | counter | `ring` | Ops completed per ring |
| `aura_ring_bytes_transferred_total` | counter | `ring` | Bytes transferred per ring |
| `aura_ring_in_flight` | gauge | `ring` | Current in-flight per ring |
| `aura_ring_in_flight_limit` | gauge | `ring` | AIMD in-flight limit per ring |
| `aura_ring_batch_threshold` | gauge | `ring` | AIMD batch threshold per ring |
| `aura_ring_queue_depth` | gauge | `ring` | Kernel queue depth per ring |
| `aura_ring_p99_latency_seconds` | gauge | `ring` | P99 latency per ring |
| `aura_ring_throughput_bytes_per_second` | gauge | `ring` | Throughput per ring |
| `aura_ring_aimd_phase` | gauge | `ring` | AIMD phase (0-6) per ring |
| `aura_latency_seconds` | histogram | `ring` | Latency distribution per ring (sum estimated from bucket midpoints) |
| `aura_adaptive_spills_total` | counter | — | ADAPTIVE mode: submissions spilled to non-local ring |
| `aura_buffer_pool_allocated_bytes` | gauge | — | Buffer pool allocated bytes |
| `aura_buffer_pool_buffers` | gauge | — | Total buffers in pool |
| `aura_buffer_pool_shards` | gauge | — | Number of pool shards |

> **Note:** `peak_in_flight` from `aura_ring_stats_t` is intentionally not exported by the Prometheus formatter. It is available via `aura_get_ring_stats()` for application-level introspection but is omitted from the exporter schema to keep cardinality bounded.

### Demo Server

A minimal example server is included for testing:

```bash
./integrations/prometheus/C/prometheus_example &
curl -s http://localhost:9091/metrics
```

This is a demo only — it runs a single-threaded accept loop and is not intended for production use. In production, call `aura_metrics_prometheus()` from your existing HTTP/metrics infrastructure.

## OpenTelemetry Integration

### `aura_metrics_otel()`

```c
#include "integrations/opentelemetry/C/aura_otel.h"

int aura_metrics_otel(aura_engine_t *engine, char *buf, size_t buf_size);
```

Formats all AuraIO metrics as an OTLP/JSON `ExportMetricsServiceRequest`. The output can be POSTed directly to an OpenTelemetry collector's `/v1/metrics` endpoint.

Recommended buffer size: **32KB + 24KB per ring**.

On success, returns the number of bytes written (excluding null terminator). On failure, returns a negative value: if `errno == ENOBUFS`, the absolute value is an estimate of the required buffer size; if `errno == ENOMEM`, a hard allocation failure occurred.

```c
#include "integrations/opentelemetry/C/aura_otel.h"

char buf[131072];
int len = aura_metrics_otel(engine, buf, sizeof(buf));
if (len < 0) {
    // Buffer too small or allocation failure
}
// POST buf[0..len] to OTel collector /v1/metrics endpoint
// Content-Type: application/json
```

## Syslog Integration

### `aura_syslog_install()` / `aura_syslog_remove()`

```c
#include "integrations/syslog/C/aura_syslog.h"

void aura_syslog_install(const aura_syslog_options_t *options);
void aura_syslog_remove(void);
```

Forwards AuraIO library log messages to `syslog(3)`. AuraIO log levels map directly to syslog priorities:

| AuraIO level | syslog priority |
|--------------|----------------|
| `AURA_LOG_ERR` | `LOG_ERR` |
| `AURA_LOG_WARN` | `LOG_WARNING` |
| `AURA_LOG_NOTICE` | `LOG_NOTICE` |
| `AURA_LOG_INFO` | `LOG_INFO` |
| `AURA_LOG_DEBUG` | `LOG_DEBUG` |

Pass `NULL` to `aura_syslog_install()` for sensible defaults (`ident="aura"`, `facility=LOG_USER`, `LOG_PID | LOG_NDELAY`). To customize, populate an `aura_syslog_options_t`:

```c
typedef struct {
    const char *ident;   // openlog ident (default: "aura")
    int facility;        // syslog facility (default: LOG_USER, -1 for default)
    int log_options;     // openlog() flags (default: LOG_PID | LOG_NDELAY, -1 for default)
} aura_syslog_options_t;
// Note: `ident` must remain valid until `aura_syslog_remove()` — POSIX `openlog()` retains the pointer.
```

```c
#include "integrations/syslog/C/aura_syslog.h"

aura_syslog_install(NULL);  // use defaults
aura_engine_t *e = aura_create();
// ... library and app logs forwarded to syslog ...
aura_syslog_remove();
aura_destroy(e);
```

### Grafana Dashboard

Point Prometheus at your application's `/metrics` endpoint, then build dashboards with these queries:

```promql
# Throughput across all rings
aura_throughput_bytes_per_second

# Per-ring P99 latency
aura_ring_p99_latency_seconds

# AIMD phase distribution (are rings in passthrough or converged?)
count by (ring) (aura_ring_aimd_phase == 6 or aura_ring_aimd_phase == 5)

# Latency P99 from histogram (more accurate than gauge)
histogram_quantile(0.99, rate(aura_latency_seconds_bucket[1m]))

# In-flight utilization ratio
aura_ring_in_flight / aura_ring_in_flight_limit
```

## Sampling Rate & Performance

### Cost per Sample

| Operation | Cost | Lock | Notes |
|-----------|------|------|-------|
| `aura_get_stats()` | ~1-2 us | Per-ring mutex (iterated) | Aggregates across all rings |
| `aura_get_ring_stats()` | ~50-100 ns | Single ring mutex | One lock/unlock pair |
| `aura_get_histogram()` | ~200-500 ns | Single ring mutex | Copies 320 x 4-byte buckets under lock |
| `aura_get_buffer_stats()` | ~10-30 ns | None (lockless) | Atomic reads only |
| `aura_phase_name()` | ~5 ns | None | Pure lookup |

### Total Cost at Various Polling Rates

Assuming 16 rings, polling all stats + histogram per ring:

| Rate | Calls/sec | Mutex Acquires/sec | Total CPU/sec | Impact |
|------|-----------|-------------------|---------------|--------|
| 1 Hz | 1 | 48 | ~5-10 us | Negligible |
| 10 Hz | 10 | 480 | ~50-100 us | Negligible |
| 100 Hz | 100 | 4,800 | ~500 us - 1 ms | Minimal |
| 1000 Hz | 1,000 | 48,000 | ~5-10 ms | Measurable — avoid unless needed |

### Recommendations

- **1 Hz** is the standard for Prometheus scraping and introduces effectively zero overhead.
- **10 Hz** is suitable for real-time dashboards or adaptive application logic.
- **100 Hz** is the practical upper bound for general monitoring. Beyond this, you may observe occasional lock contention spikes if a ring is under heavy I/O.
- **Avoid polling faster than 100 Hz per ring** unless you have a specific latency-sensitive feedback loop that requires it.

### Locking Details

Stats reads use the same per-ring `pthread_mutex` that protects submission. This means a stats read can briefly delay an I/O submission (and vice versa), but since the critical section is extremely short (~50-500 ns), the added tail latency is unmeasurable at polling rates below 100 Hz.

Buffer pool stats are fully lockless — they read atomics that are updated during normal allocation/free operations with relaxed memory ordering.

### Histogram Double-Buffering

The latency histogram uses a **double-buffer swap** strategy. The AIMD controller periodically swaps the active and inactive histogram windows. `aura_get_histogram()` copies the *active* window, so:

- You always get the most recent complete measurement window
- The copy is ~1,280 bytes (320 buckets x 4 bytes) and takes ~200-500 ns under lock
- The swap itself is a single pointer exchange and does not affect I/O

---

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for details.
