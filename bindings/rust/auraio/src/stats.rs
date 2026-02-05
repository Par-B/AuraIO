//! Engine statistics

use auraio_sys;

/// Engine statistics snapshot
///
/// Retrieved via `Engine::stats()` for monitoring and debugging.
#[derive(Debug, Clone, Default)]
pub struct Stats {
    inner: auraio_sys::auraio_stats_t,
}

impl Stats {
    /// Create a new stats snapshot (internal use)
    pub(crate) fn new(inner: auraio_sys::auraio_stats_t) -> Self {
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
}
