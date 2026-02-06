# AuraIO Observability Guide

## Overview

AuraIO exposes a three-tier metrics API for monitoring I/O performance in production:

| Tier | Scope | Functions |
|------|-------|-----------|
| **Aggregate** | Engine-wide totals | `auraio_get_stats()` |
| **Per-Ring** | Individual io_uring ring + AIMD controller state | `auraio_get_ring_stats()`, `auraio_get_histogram()` |
| **Buffer Pool** | Memory allocator health | `auraio_get_buffer_stats()` |

All stats functions are **thread-safe** and safe to call from any thread at any time, including during active I/O.

## C API Reference

### Aggregate Stats

```c
auraio_stats_t stats;
auraio_get_stats(engine, &stats);

printf("IOPS: %.0f, Throughput: %.1f MB/s, P99: %.2f ms\n",
       stats.ops_completed / elapsed,
       stats.current_throughput_bps / (1024 * 1024),
       stats.p99_latency_ms);
```

Fields in `auraio_stats_t`:

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed across all rings |
| `bytes_transferred` | `int64_t` | Total bytes read/written across all rings |
| `current_throughput_bps` | `double` | Current aggregate throughput (bytes/sec) |
| `p99_latency_ms` | `double` | 99th percentile latency (ms) |
| `current_in_flight` | `int` | Current total in-flight operations |
| `optimal_in_flight` | `int` | AIMD-tuned aggregate in-flight limit |
| `optimal_batch_size` | `int` | AIMD-tuned aggregate batch size |

### Per-Ring Stats

```c
int rings = auraio_get_ring_count(engine);

for (int i = 0; i < rings; i++) {
    auraio_ring_stats_t rs;
    auraio_get_ring_stats(engine, i, &rs);

    printf("Ring %d: phase=%s, in_flight=%d/%d, p99=%.2fms, %.1f MB/s\n",
           i, auraio_phase_name(rs.aimd_phase),
           rs.pending_count, rs.in_flight_limit,
           rs.p99_latency_ms, rs.throughput_bps / (1024 * 1024));
}
```

Fields in `auraio_ring_stats_t`:

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed on this ring |
| `bytes_transferred` | `int64_t` | Total bytes transferred through this ring |
| `pending_count` | `int` | Current in-flight operations |
| `in_flight_limit` | `int` | AIMD-tuned maximum in-flight |
| `batch_threshold` | `int` | AIMD-tuned batch size |
| `p99_latency_ms` | `double` | Current P99 latency estimate (ms) |
| `throughput_bps` | `double` | Current throughput (bytes/sec) |
| `aimd_phase` | `int` | Controller phase (0-5, see table below) |
| `queue_depth` | `int` | Kernel queue depth for this ring |

AIMD phases (via `auraio_phase_name()`):

| Value | Name | Meaning |
|-------|------|---------|
| 0 | BASELINE | Initial ramp-up, collecting first samples |
| 1 | PROBING | Additive increase — testing higher depth |
| 2 | STEADY | Throughput plateau — holding position |
| 3 | BACKOFF | Multiplicative decrease — P99 exceeded target |
| 4 | SETTLING | Post-backoff stabilization |
| 5 | CONVERGED | Optimal depth found, minor adjustments only |

### Latency Histogram

```c
auraio_histogram_t hist;
auraio_get_histogram(engine, ring_idx, &hist);

// 200 buckets x 50us = 0-10ms range
for (int b = 0; b < AURAIO_HISTOGRAM_BUCKETS; b++) {
    if (hist.buckets[b] > 0) {
        printf("  %d-%d us: %u ops\n",
               b * hist.bucket_width_us,
               (b + 1) * hist.bucket_width_us,
               hist.buckets[b]);
    }
}
printf("  >%d us (overflow): %u ops\n", hist.max_tracked_us, hist.overflow);
printf("  total: %u ops\n", hist.total_count);
```

The histogram is an **approximate snapshot of the active window** — AuraIO double-buffers histograms internally and swaps them periodically. Because the snapshot is read from a concurrently-written histogram, individual bucket values are atomic but `total_count` may differ slightly from the sum of all buckets + overflow. For monitoring purposes this is negligible.

When adaptive tuning is disabled (`disable_adaptive = true`), the histogram is not periodically reset and accumulates data indefinitely.

### Buffer Pool Stats

```c
auraio_buffer_stats_t bs;
auraio_get_buffer_stats(engine, &bs);

printf("Pool: %zu buffers, %zu bytes across %d shards\n",
       bs.total_buffers, bs.total_allocated_bytes, bs.shard_count);
```

## Binding Coverage

| Feature | C | C++ | Rust |
|---------|---|-----|------|
| Aggregate stats | `auraio_get_stats()` | `engine.get_stats()` | `engine.stats()` |
| Ring count | `auraio_get_ring_count()` | `engine.ring_count()` | Not yet |
| Per-ring stats | `auraio_get_ring_stats()` | `engine.get_ring_stats(i)` | Not yet |
| Latency histogram | `auraio_get_histogram()` | `engine.get_histogram(i)` | Not yet |
| Buffer pool stats | `auraio_get_buffer_stats()` | `engine.get_buffer_stats()` | Not yet |
| Phase name | `auraio_phase_name()` | `ring_stats.aimd_phase_name()` | Not yet |

The C++ bindings are header-only and fully inline — no additional link dependencies.

Rust bindings currently expose aggregate stats only. Per-ring stats, histograms, and buffer pool metrics will be added in a future release once the FFI types are generated by `bindgen` from the updated header.

## C++ Usage

```cpp
#include <auraio.hpp>

auraio::Engine engine;

// Aggregate
auto stats = engine.get_stats();
std::cout << stats.throughput_bps() << " bytes/sec\n";

// Per-ring
for (int i = 0; i < engine.ring_count(); i++) {
    auto rs = engine.get_ring_stats(i);
    std::cout << "Ring " << i << ": " << rs.aimd_phase_name()
              << " depth=" << rs.in_flight_limit() << "\n";

    auto hist = engine.get_histogram(i);
    for (int b = 0; b < auraio::Histogram::bucket_count; b++) {
        if (hist.bucket(b) > 0)
            std::cout << "  " << hist.bucket_lower_us(b) << "-"
                      << hist.bucket_upper_us(b) << "us: "
                      << hist.bucket(b) << "\n";
    }
}

// Buffer pool
auto bs = engine.get_buffer_stats();
std::cout << bs.total_buffers() << " buffers in " << bs.shard_count() << " shards\n";
```

## Prometheus Integration

AuraIO ships a standalone Prometheus exposition text formatter in `exporters/prometheus/`. It has **no external dependencies** beyond libauraio itself.

### Building

```bash
make exporters
```

### Using the Formatter in Your Application

The formatter writes Prometheus exposition text into a user-provided buffer:

```c
#include "auraio_prometheus.h"

char buf[256 * 1024];
int len = auraio_metrics_prometheus(engine, buf, sizeof(buf));
if (len < 0) {
    // Buffer too small — |len| is an estimate of needed size
}
// Write buf[0..len] as HTTP response body with
// Content-Type: text/plain; version=0.0.4; charset=utf-8
```

Integrate this into whichever HTTP server you already run — there is no built-in HTTP listener in the library.

### Exported Metrics

| Metric | Type | Labels | Description |
|--------|------|--------|-------------|
| `auraio_ops_completed_total` | counter | — | Total I/O operations completed |
| `auraio_bytes_transferred_total` | counter | — | Total bytes transferred |
| `auraio_throughput_bytes_per_second` | gauge | — | Current aggregate throughput |
| `auraio_p99_latency_seconds` | gauge | — | Aggregate P99 latency |
| `auraio_in_flight` | gauge | — | Current total in-flight operations |
| `auraio_optimal_in_flight` | gauge | — | AIMD-tuned aggregate in-flight limit |
| `auraio_optimal_batch_size` | gauge | — | AIMD-tuned aggregate batch size |
| `auraio_ring_count` | gauge | — | Number of io_uring rings |
| `auraio_ring_ops_completed_total` | counter | `ring` | Ops completed per ring |
| `auraio_ring_bytes_transferred_total` | counter | `ring` | Bytes transferred per ring |
| `auraio_ring_in_flight` | gauge | `ring` | Current in-flight per ring |
| `auraio_ring_in_flight_limit` | gauge | `ring` | AIMD in-flight limit per ring |
| `auraio_ring_batch_threshold` | gauge | `ring` | AIMD batch threshold per ring |
| `auraio_ring_queue_depth` | gauge | `ring` | Kernel queue depth per ring |
| `auraio_ring_p99_latency_seconds` | gauge | `ring` | P99 latency per ring |
| `auraio_ring_throughput_bytes_per_second` | gauge | `ring` | Throughput per ring |
| `auraio_ring_aimd_phase` | gauge | `ring` | AIMD phase (0-5) per ring |
| `auraio_latency_seconds` | histogram | `ring` | Latency distribution per ring (sum estimated from bucket midpoints) |
| `auraio_buffer_pool_allocated_bytes` | gauge | — | Buffer pool allocated bytes |
| `auraio_buffer_pool_buffers` | gauge | — | Total buffers in pool |
| `auraio_buffer_pool_shards` | gauge | — | Number of pool shards |

### Demo Server

A minimal example server is included for testing:

```bash
./exporters/prometheus/prometheus_example &
curl -s http://localhost:9091/metrics
```

This is a demo only — it runs a single-threaded accept loop and is not intended for production use. In production, call `auraio_metrics_prometheus()` from your existing HTTP/metrics infrastructure.

### Grafana Dashboard

Point Prometheus at your application's `/metrics` endpoint, then build dashboards with these queries:

```promql
# Throughput across all rings
auraio_throughput_bytes_per_second

# Per-ring P99 latency
auraio_ring_p99_latency_seconds

# AIMD phase distribution (are rings converged?)
count by (ring) (auraio_ring_aimd_phase == 5)

# Latency P99 from histogram (more accurate than gauge)
histogram_quantile(0.99, rate(auraio_latency_seconds_bucket[1m]))

# In-flight utilization ratio
auraio_ring_in_flight / auraio_ring_in_flight_limit
```

## Sampling Rate & Performance

### Cost per Sample

| Operation | Cost | Lock | Notes |
|-----------|------|------|-------|
| `auraio_get_stats()` | ~1-2 us | Per-ring mutex (iterated) | Aggregates across all rings |
| `auraio_get_ring_stats()` | ~50-100 ns | Single ring mutex | One lock/unlock pair |
| `auraio_get_histogram()` | ~200-500 ns | Single ring mutex | Copies 200 x 4-byte buckets under lock |
| `auraio_get_buffer_stats()` | ~10-30 ns | None (lockless) | Atomic reads only |
| `auraio_phase_name()` | ~5 ns | None | Pure lookup |

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

The latency histogram uses a **double-buffer swap** strategy. The AIMD controller periodically swaps the active and inactive histogram windows. `auraio_get_histogram()` copies the *active* window, so:

- You always get the most recent complete measurement window
- The copy is ~800 bytes (200 buckets x 4 bytes) and takes ~200-500 ns under lock
- The swap itself is a single pointer exchange and does not affect I/O
