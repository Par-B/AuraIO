/**
 * @file adaptive_engine.h
 * @brief AIMD congestion control for I/O tuning
 *
 * Internal header - not part of public API.
 *
 * Implements Additive Increase Multiplicative Decrease (AIMD) control
 * to automatically tune in-flight limit and batch size based on observed
 * throughput and latency.
 */

#ifndef ADAPTIVE_ENGINE_H
#define ADAPTIVE_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

/* ============================================================================
 * Constants
 * ============================================================================
 *
 * AIMD (Additive Increase Multiplicative Decrease) tuning parameters.
 *
 * These constants control the adaptive algorithm's behavior. They have been
 * tuned for modern NVMe SSDs and high-performance storage systems.
 *
 * Key design decisions:
 *
 * 1. 10ms Latency Guard (LATENCY_MAX_US, ADAPTIVE_DEFAULT_LATENCY_GUARD):
 *    Any I/O operation taking longer than 10ms indicates either a heavily
 *    saturated device or a failing storage system. Modern NVMe SSDs complete
 *    random reads in 50-100µs; even QD256 should stay under 5ms. If your
 *    workload consistently sees >10ms latencies, either the device is
 *    fundamentally too slow for performance-critical work, or something is
 *    wrong (thermal throttling, firmware bugs, etc.). The library backs off
 *    aggressively when hitting this ceiling.
 *
 * 2. 20% Multiplicative Decrease (ADAPTIVE_AIMD_DECREASE = 0.80):
 *    Standard AIMD uses 50% decrease (TCP Reno). We use a gentler 20% cut
 *    because storage latency is more predictable than network RTT, and
 *    aggressive cuts cause unnecessary throughput oscillation. The 20%
 *    reduction provides sufficient backoff while maintaining stability.
 *
 * 3. 10ms Sample Interval (ADAPTIVE_SAMPLE_INTERVAL_MS):
 *    Matches io_uring's typical completion polling granularity and provides
 *    enough samples per tick for statistically significant P99 calculation
 *    at typical SSD throughput levels (>100K IOPS).
 *
 * 4. 10x Latency Guard Multiplier (ADAPTIVE_LATENCY_GUARD_MULT):
 *    Triggers backoff when P99 exceeds 10x the baseline. This allows for
 *    workload variability while catching genuine saturation. A 10x increase
 *    from baseline (e.g., 100µs to 1ms) indicates approaching device limits.
 */

/** Tick interval in milliseconds. The adaptive controller evaluates metrics
 *  and potentially adjusts parameters every 10ms. */
#define ADAPTIVE_SAMPLE_INTERVAL_MS 10

/** Warmup samples before establishing baseline. Collect 10 ticks (100ms) of
 *  latency data to establish a reliable minimum P99 baseline. */
#define ADAPTIVE_WARMUP_SAMPLES 10

/** P99 sliding window size. Track the last 15 P99 samples (150ms) to smooth
 *  out transient spikes and detect sustained latency changes. */
#define ADAPTIVE_P99_WINDOW 15

/** Throughput sliding window size. Track 25 samples (250ms) for throughput
 *  efficiency ratio calculation. Longer than P99 window to reduce noise. */
#define ADAPTIVE_THROUGHPUT_WINDOW 25

/** Baseline tracking window size. Track 50 samples (500ms) of minimum P99
 *  values to maintain a stable baseline even as workload varies. */
#define ADAPTIVE_BASELINE_WINDOW 50

/** Additive increase per tick. Add 1 to in-flight limit each tick during
 *  PROBING phase. Conservative increase avoids overshooting optimal point. */
#define ADAPTIVE_AIMD_INCREASE 1

/** Multiplicative decrease factor. Cut in-flight limit by 20% on backoff.
 *  Gentler than TCP's 50% because storage latency is more predictable. */
#define ADAPTIVE_AIMD_DECREASE 0.80

/** Near-zero efficiency ratio threshold. If efficiency ratio is below 1%,
 *  consider it a plateau (throughput not increasing with more in-flight). */
#define ADAPTIVE_ER_EPSILON 0.01

/** Latency guard multiplier. Trigger backoff when P99 exceeds baseline by
 *  this factor. 10x allows normal variation while catching saturation. */
#define ADAPTIVE_LATENCY_GUARD_MULT 10.0

/** Ideal completions for valid P99. Need ~100 samples for statistically
 *  meaningful 99th percentile. At low IOPS, use MIN_SAMPLES or time window. */
#define ADAPTIVE_MIN_SAMPLES_FOR_P99 100

/** Minimum samples before making any AIMD decision. Even at low IOPS, need
 *  at least 20 completions to make a reasonable judgment call. */
#define ADAPTIVE_LOW_IOPS_MIN_SAMPLES 20

/** Minimum sample window in milliseconds. At low IOPS, wait at least 100ms
 *  before making decisions to accumulate enough samples. */
#define ADAPTIVE_MIN_SAMPLE_WINDOW_MS 100

/** Maximum sample window in milliseconds. Don't wait longer than 1 second
 *  even at very low IOPS; make a decision with available data. */
#define ADAPTIVE_MAX_SAMPLE_WINDOW_MS 1000

/** Minimum batch threshold. Never batch fewer than 2 SQEs per submit to
 *  maintain syscall efficiency. */
#define ADAPTIVE_MIN_BATCH 2

/** Target SQEs per submit syscall. Aim for 8 SQEs per io_uring_enter()
 *  to amortize syscall overhead while maintaining responsiveness. */
#define ADAPTIVE_TARGET_SQE_RATIO 8.0

/** Maximum batch threshold. Cap batching at 64 SQEs to bound worst-case
 *  latency from batch accumulation. */
#define ADAPTIVE_MAX_BATCH_THRESHOLD 64

/** Default latency guard in milliseconds. Hard ceiling on acceptable P99.
 *
 *  Rationale: No modern storage device should take >10ms for an I/O operation
 *  under normal conditions. NVMe SSDs: 50-200µs typical, <5ms at saturation.
 *  SATA SSDs: 100-500µs typical, <8ms at saturation. If operations consistently
 *  exceed 10ms, the device is either unsuitable for performance-critical
 *  workloads or experiencing problems. Users of AuraIO expect performance;
 *  this guard triggers aggressive backoff to prevent latency blowup. */
#define ADAPTIVE_DEFAULT_LATENCY_GUARD 10.0

/** Settling phase duration in ticks. After backoff, wait 10 ticks (100ms)
 *  for metrics to stabilize before resuming PROBING. */
#define ADAPTIVE_SETTLING_TICKS 10

/** Ticks in STEADY before CONVERGED. Stay in STEADY for 500 ticks (5 seconds)
 *  with stable metrics before declaring tuning complete. */
#define ADAPTIVE_STEADY_THRESHOLD 500

/* Histogram configuration:
 *
 * The histogram tracks latencies from 0 to 10ms in 50µs buckets (200 buckets).
 * This provides sufficient resolution for P99 calculation while keeping the
 * histogram compact. Operations exceeding 10ms go into the overflow bucket
 * and are counted against the latency guard.
 */

/** Latency bucket width in microseconds. 50µs granularity is sufficient for
 *  P99 calculation and matches NVMe command timing precision. */
#define LATENCY_BUCKET_WIDTH_US 50

/** Maximum tracked latency in microseconds. Operations taking longer than
 *  10ms are counted in overflow and trigger latency guard. See rationale
 *  for ADAPTIVE_DEFAULT_LATENCY_GUARD above. */
#define LATENCY_MAX_US 10000

/** Number of histogram buckets. Derived from max latency and bucket width. */
#define LATENCY_BUCKET_COUNT (LATENCY_MAX_US / LATENCY_BUCKET_WIDTH_US)

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Adaptive phase
 */
typedef enum {
    ADAPTIVE_PHASE_BASELINE, /**< Collecting baseline latency */
    ADAPTIVE_PHASE_PROBING,  /**< Increasing in-flight limit */
    ADAPTIVE_PHASE_STEADY,   /**< Maintaining optimal config */
    ADAPTIVE_PHASE_BACKOFF,  /**< Reducing due to latency spike */
    ADAPTIVE_PHASE_SETTLING, /**< Waiting for metrics to stabilize */
    ADAPTIVE_PHASE_CONVERGED /**< Tuning complete */
} adaptive_phase_t;

/**
 * Per-sample latency histogram
 *
 * Lightweight histogram for fast P99 calculation.
 * Uses atomic types for thread-safe updates from completion threads.
 */
typedef struct {
    _Atomic uint32_t buckets[LATENCY_BUCKET_COUNT]; /**< Latency buckets */
    _Atomic uint32_t overflow;                      /**< Count > max tracked */
    _Atomic uint32_t total_count;                   /**< Total samples */
} adaptive_histogram_t;

/**
 * Double-buffered histogram pair
 *
 * Two histograms with atomic index swap for O(1) reset.
 * Recording writes to active histogram, tick swaps and clears inactive.
 */
typedef struct {
    adaptive_histogram_t histograms[2]; /**< Double buffer */
    _Atomic int active_index;           /**< Active histogram (0 or 1) */
} adaptive_histogram_pair_t;

/**
 * Adaptive controller state
 *
 * AIMD state machine for tuning I/O parameters.
 */
typedef struct {
    /* === Cache line 0: Hot atomics for submit + completion paths ===
     * All fields accessed per-op are packed here so both the submission
     * thread (reads limits) and completion thread (updates counters)
     * only need to fetch a single cache line. */
    _Atomic int current_in_flight_limit;   /**< Current max concurrent ops */
    _Atomic int current_batch_threshold;   /**< Current batch size before submit */
    _Atomic int submit_calls;              /**< Submit syscalls this period */
    _Atomic int sqes_submitted;            /**< SQEs submitted this period */
    _Atomic int64_t sample_start_ns;       /**< Current sample start time */
    _Atomic int64_t sample_bytes;          /**< Bytes completed this sample */
    _Atomic double current_p99_ms;         /**< Current sample P99 */
    _Atomic double current_throughput_bps; /**< Current throughput */
    _Atomic adaptive_phase_t phase;        /**< Current phase */
    int max_queue_depth;                   /**< Upper bound on in-flight */
    int min_in_flight;                     /**< Lower bound on in-flight */
    int prev_in_flight_limit;              /**< Previous limit for efficiency ratio */

    /* === Cache line 1+: Tick-only state (cold, accessed every 10ms) === */
    double baseline_p99_ms;        /**< Minimum observed P99 */
    double latency_rise_threshold; /**< Threshold for latency backoff */
    double max_p99_ms;             /**< Hard ceiling on P99 (0 = none) */
    double prev_throughput_bps;    /**< Previous throughput for efficiency ratio */
    int warmup_count;              /**< Samples collected in warmup */
    int plateau_count;             /**< Consecutive plateau samples */
    int steady_count;              /**< Time in STEADY phase */
    int spike_count;               /**< Consecutive latency spikes */
    int settling_timer;            /**< Time in SETTLING phase */
    int p99_head, p99_count;
    int throughput_head, throughput_count;
    int baseline_head, baseline_count;

    /* Sliding windows (cold, only accessed during tick) */
    double p99_window[ADAPTIVE_P99_WINDOW];
    double throughput_window[ADAPTIVE_THROUGHPUT_WINDOW];
    double baseline_window[ADAPTIVE_BASELINE_WINDOW];

    /* Double-buffered histogram for O(1) reset.
     * Aligned to cacheline boundary to prevent false sharing with
     * the sliding windows above. */
    _Alignas(64) adaptive_histogram_pair_t hist_pair;
} adaptive_controller_t;

/* ============================================================================
 * Functions
 * ============================================================================ */

/**
 * Initialize adaptive controller
 *
 * @param ctrl            Controller to initialize
 * @param max_queue_depth Maximum in-flight limit
 * @param initial_inflight Starting in-flight limit
 * @return 0 on success, -1 on error
 */
int adaptive_init(adaptive_controller_t *ctrl, int max_queue_depth, int initial_inflight);

/**
 * Destroy adaptive controller
 *
 * @param ctrl Controller to destroy
 */
void adaptive_destroy(adaptive_controller_t *ctrl);

/**
 * Record a completion
 *
 * Called for each completed operation to track latency.
 *
 * @param ctrl       Controller
 * @param latency_ns Completion latency in nanoseconds
 * @param bytes      Bytes transferred
 */
void adaptive_record_completion(adaptive_controller_t *ctrl, int64_t latency_ns, size_t bytes);

/**
 * Process a tick (every 10ms)
 *
 * Updates statistics, calculates P99, and adjusts parameters.
 *
 * @param ctrl Controller
 * @return true if parameters changed, false otherwise
 */
bool adaptive_tick(adaptive_controller_t *ctrl);

/**
 * Record a submit syscall
 *
 * Tracks submit call frequency for batch optimization.
 *
 * @param ctrl      Controller
 * @param sqe_count Number of SQEs submitted
 */
void adaptive_record_submit(adaptive_controller_t *ctrl, int sqe_count);

/**
 * Get current in-flight limit
 *
 * Inlined: called on every submission in ring_can_submit().
 *
 * @param ctrl Controller
 * @return Current limit
 */
static inline int adaptive_get_inflight_limit(adaptive_controller_t *ctrl) {
    return atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);
}

/**
 * Get current batch threshold
 *
 * Inlined: called on every submission in ring_should_flush().
 *
 * @param ctrl Controller
 * @return Current threshold
 */
static inline int adaptive_get_batch_threshold(adaptive_controller_t *ctrl) {
    return atomic_load_explicit(&ctrl->current_batch_threshold, memory_order_relaxed);
}

/**
 * Get current phase name
 *
 * @param phase Phase enum value
 * @return Phase name string
 */
const char *adaptive_phase_name(adaptive_phase_t phase);

/**
 * Reset histogram for new sample period
 *
 * @param hist Histogram to reset
 */
void adaptive_hist_reset(adaptive_histogram_t *hist);

/**
 * Initialize histogram pair
 *
 * @param pair Histogram pair to initialize
 */
void adaptive_hist_pair_init(adaptive_histogram_pair_t *pair);

/**
 * Swap active histogram and clear the old one
 *
 * O(1) operation - just swaps the active index and clears the
 * now-inactive histogram with memset (not atomic stores).
 *
 * @param pair Histogram pair
 * @return Pointer to the histogram that was just deactivated (for reading)
 */
adaptive_histogram_t *adaptive_hist_swap(adaptive_histogram_pair_t *pair);

/**
 * Get active histogram for recording
 *
 * @param pair Histogram pair
 * @return Pointer to active histogram
 */
adaptive_histogram_t *adaptive_hist_active(adaptive_histogram_pair_t *pair);

/**
 * Record latency in histogram
 *
 * @param hist       Histogram
 * @param latency_us Latency in microseconds
 */
void adaptive_hist_record(adaptive_histogram_t *hist, int64_t latency_us);

/**
 * Calculate P99 from histogram
 *
 * @param hist Histogram
 * @return P99 latency in milliseconds
 */
double adaptive_hist_p99(adaptive_histogram_t *hist);

#endif /* ADAPTIVE_ENGINE_H */
