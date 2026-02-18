//! Engine configuration options

/// Ring selection mode
///
/// Controls how submissions are distributed across io_uring rings.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RingSelect {
    /// CPU-local with overflow spilling (default)
    Adaptive,
    /// CPU-affinity only (best NUMA locality)
    CpuLocal,
    /// Atomic round-robin across all rings (max single-thread scaling)
    RoundRobin,
}

impl RingSelect {
    fn to_c(self) -> u32 {
        match self {
            RingSelect::Adaptive => aura_sys::aura_ring_select_t_AURA_SELECT_ADAPTIVE,
            RingSelect::CpuLocal => aura_sys::aura_ring_select_t_AURA_SELECT_CPU_LOCAL,
            RingSelect::RoundRobin => aura_sys::aura_ring_select_t_AURA_SELECT_ROUND_ROBIN,
        }
    }
}

/// Engine configuration options
///
/// Use the builder pattern to customize engine behavior:
///
/// ```no_run
/// use aura::Options;
///
/// let opts = Options::new()
///     .queue_depth(512)
///     .ring_count(4)
///     .enable_sqpoll(true);
/// ```
#[derive(Debug, Clone)]
pub struct Options {
    inner: aura_sys::aura_options_t,
}

impl Default for Options {
    fn default() -> Self {
        Self::new()
    }
}

impl Options {
    /// Create new options with default values
    pub fn new() -> Self {
        let mut inner: aura_sys::aura_options_t = unsafe { std::mem::zeroed() };
        unsafe {
            aura_sys::aura_options_init(&mut inner);
        }
        Self { inner }
    }

    /// Set the queue depth per ring (default: 256)
    pub fn queue_depth(mut self, depth: i32) -> Self {
        self.inner.queue_depth = depth;
        self
    }

    /// Set the number of rings (default: 0 = auto, one per CPU)
    pub fn ring_count(mut self, count: i32) -> Self {
        self.inner.ring_count = count;
        self
    }

    /// Set the initial in-flight limit (default: queue_depth/4)
    pub fn initial_in_flight(mut self, limit: i32) -> Self {
        self.inner.initial_in_flight = limit;
        self
    }

    /// Set the minimum in-flight limit (default: 4)
    pub fn min_in_flight(mut self, limit: i32) -> Self {
        self.inner.min_in_flight = limit;
        self
    }

    /// Set the target max P99 latency in milliseconds (default: 0 = auto)
    pub fn max_p99_latency_ms(mut self, latency: f64) -> Self {
        self.inner.max_p99_latency_ms = latency;
        self
    }

    /// Set buffer alignment (default: 4096)
    pub fn buffer_alignment(mut self, alignment: usize) -> Self {
        self.inner.buffer_alignment = alignment;
        self
    }

    /// Disable adaptive tuning
    pub fn disable_adaptive(mut self, disable: bool) -> Self {
        self.inner.disable_adaptive = disable;
        self
    }

    /// Enable SQPOLL mode (requires root or CAP_SYS_NICE)
    pub fn enable_sqpoll(mut self, enable: bool) -> Self {
        self.inner.enable_sqpoll = enable;
        self
    }

    /// Set SQPOLL idle timeout in milliseconds (default: 1000)
    pub fn sqpoll_idle_ms(mut self, timeout: i32) -> Self {
        self.inner.sqpoll_idle_ms = timeout;
        self
    }

    /// Set ring selection mode (default: Adaptive)
    pub fn ring_select(mut self, mode: RingSelect) -> Self {
        self.inner.ring_select = mode.to_c();
        self
    }

    /// Enable single-thread mode (skip ring mutexes)
    ///
    /// When enabled, the engine skips internal mutex locking on submissions,
    /// which reduces overhead.
    ///
    /// # Safety
    ///
    /// The caller must guarantee that only one thread calls submission methods
    /// at a time. Since `Engine` implements `Sync`, it can be shared across
    /// threads — using this option without single-threaded access is undefined
    /// behavior.
    pub unsafe fn single_thread(mut self, enable: bool) -> Self {
        self.inner.single_thread = enable;
        self
    }

    /// Get a reference to the underlying C options struct
    pub(crate) fn as_ptr(&self) -> *const aura_sys::aura_options_t {
        &self.inner
    }
}
