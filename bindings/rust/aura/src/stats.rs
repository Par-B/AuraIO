//! Engine statistics

/// Engine statistics snapshot
///
/// Retrieved via `Engine::stats()` for monitoring and debugging.
#[derive(Debug, Clone, Default)]
pub struct Stats {
    inner: aura_sys::aura_stats_t,
}

impl Stats {
    /// Create a new stats snapshot (internal use)
    pub(crate) fn new(inner: aura_sys::aura_stats_t) -> Self {
        Self { inner }
    }

    /// Total operations completed
    pub fn ops_completed(&self) -> i64 {
        self.inner.ops_completed
    }

    /// Total bytes transferred (read + written)
    pub fn bytes_transferred(&self) -> i64 {
        self.inner.bytes_transferred
    }

    /// Current throughput in bytes per second
    pub fn throughput_bps(&self) -> f64 {
        self.inner.current_throughput_bps
    }

    /// 99th percentile latency in milliseconds
    pub fn p99_latency_ms(&self) -> f64 {
        self.inner.p99_latency_ms
    }

    /// Current number of in-flight operations
    pub fn current_in_flight(&self) -> i32 {
        self.inner.current_in_flight
    }

    /// Tuned optimal in-flight limit
    pub fn optimal_in_flight(&self) -> i32 {
        self.inner.optimal_in_flight
    }

    /// Tuned optimal batch size
    pub fn optimal_batch_size(&self) -> i32 {
        self.inner.optimal_batch_size
    }

    /// Number of times ADAPTIVE mode spilled to a non-local ring
    pub fn adaptive_spills(&self) -> u64 {
        self.inner.adaptive_spills
    }
}

/// Per-ring statistics snapshot
///
/// Retrieved via `Engine::ring_stats()` for per-ring monitoring.
#[derive(Debug, Clone)]
pub struct RingStats {
    inner: aura_sys::aura_ring_stats_t,
}

impl RingStats {
    /// Create a new ring stats snapshot (internal use)
    pub(crate) fn new(inner: aura_sys::aura_ring_stats_t) -> Self {
        Self { inner }
    }

    /// Total operations completed on this ring
    pub fn ops_completed(&self) -> i64 {
        self.inner.ops_completed
    }

    /// Total bytes transferred on this ring
    pub fn bytes_transferred(&self) -> i64 {
        self.inner.bytes_transferred
    }

    /// Number of pending operations
    pub fn pending_count(&self) -> i32 {
        self.inner.pending_count
    }

    /// Current in-flight limit (AIMD-tuned)
    pub fn in_flight_limit(&self) -> i32 {
        self.inner.in_flight_limit
    }

    /// Current batch threshold
    pub fn batch_threshold(&self) -> i32 {
        self.inner.batch_threshold
    }

    /// P99 latency for this ring (milliseconds)
    pub fn p99_latency_ms(&self) -> f64 {
        self.inner.p99_latency_ms
    }

    /// Throughput for this ring (bytes per second)
    pub fn throughput_bps(&self) -> f64 {
        self.inner.throughput_bps
    }

    /// Current AIMD phase (raw value)
    pub fn aimd_phase(&self) -> i32 {
        self.inner.aimd_phase
    }

    /// Current AIMD phase as string
    pub fn aimd_phase_name(&self) -> &'static str {
        unsafe {
            let ptr = aura_sys::aura_phase_name(self.inner.aimd_phase);
            if ptr.is_null() {
                "unknown"
            } else {
                std::ffi::CStr::from_ptr(ptr)
                    .to_str()
                    .unwrap_or("unknown")
            }
        }
    }

    /// Queue depth
    pub fn queue_depth(&self) -> i32 {
        self.inner.queue_depth
    }
}

/// Latency histogram snapshot
///
/// Retrieved via `Engine::histogram()` for latency distribution analysis.
#[derive(Debug, Clone)]
pub struct Histogram {
    inner: aura_sys::aura_histogram_t,
}

impl Histogram {
    /// Number of histogram buckets
    pub const BUCKET_COUNT: usize = aura_sys::AURA_HISTOGRAM_BUCKETS as usize;

    /// Bucket width in microseconds
    pub const BUCKET_WIDTH_US: i32 = aura_sys::AURA_HISTOGRAM_BUCKET_WIDTH_US as i32;

    /// Create a new histogram snapshot (internal use)
    pub(crate) fn new(inner: aura_sys::aura_histogram_t) -> Self {
        Self { inner }
    }

    /// Get sample count for a specific bucket
    pub fn bucket(&self, idx: usize) -> u32 {
        if idx < Self::BUCKET_COUNT {
            self.inner.buckets[idx]
        } else {
            0
        }
    }

    /// Number of samples that exceeded max_tracked_us
    pub fn overflow(&self) -> u32 {
        self.inner.overflow
    }

    /// Total number of samples
    pub fn total_count(&self) -> u32 {
        self.inner.total_count
    }

    /// Maximum tracked latency (microseconds)
    pub fn max_tracked_us(&self) -> i32 {
        self.inner.max_tracked_us
    }

    /// Bucket width (microseconds)
    pub fn bucket_width_us(&self) -> i32 {
        self.inner.bucket_width_us
    }

    /// Get lower bound of bucket (microseconds)
    pub fn bucket_lower_us(&self, idx: usize) -> i32 {
        if idx < Self::BUCKET_COUNT {
            (idx as i32) * self.inner.bucket_width_us
        } else {
            0
        }
    }

    /// Get upper bound of bucket (microseconds)
    pub fn bucket_upper_us(&self, idx: usize) -> i32 {
        if idx < Self::BUCKET_COUNT {
            ((idx as i32) + 1) * self.inner.bucket_width_us
        } else {
            0
        }
    }
}

/// Buffer pool statistics snapshot
///
/// Retrieved via `Engine::buffer_stats()` for buffer pool monitoring.
#[derive(Debug, Clone)]
pub struct BufferStats {
    inner: aura_sys::aura_buffer_stats_t,
}

impl BufferStats {
    /// Create a new buffer stats snapshot (internal use)
    pub(crate) fn new(inner: aura_sys::aura_buffer_stats_t) -> Self {
        Self { inner }
    }

    /// Total allocated bytes across all buffers
    pub fn total_allocated_bytes(&self) -> usize {
        self.inner.total_allocated_bytes
    }

    /// Total number of buffers
    pub fn total_buffers(&self) -> usize {
        self.inner.total_buffers
    }

    /// Number of shards in the buffer pool
    pub fn shard_count(&self) -> i32 {
        self.inner.shard_count
    }
}
