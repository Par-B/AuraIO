// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

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
#include <string.h> /* memcpy */

/* ============================================================================
 * Lock-free double atomics via uint64_t type-punning
 *
 * C11 does not guarantee _Atomic double is lock-free on all platforms.
 * We store doubles as _Atomic uint64_t and use memcpy-based punning
 * (well-defined in C11) for loads and stores.
 * ============================================================================ */

static inline void atomic_store_double(_Atomic uint64_t *a, double v, memory_order order) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    atomic_store_explicit(a, bits, order);
}

static inline double atomic_load_double(const _Atomic uint64_t *a, memory_order order) {
    uint64_t bits = atomic_load_explicit((_Atomic uint64_t *)a, order);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

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
 * 1. 50ms Latency Guard (ADAPTIVE_DEFAULT_LATENCY_GUARD):
 *    The default guard is 50ms to accommodate NFS and network storage where
 *    latencies routinely exceed 10ms. For NVMe SSDs (50-200µs typical), the
 *    dynamic guard (baseline_p99 * LATENCY_GUARD_MULT) converges to a much
 *    lower threshold automatically. The histogram tracks up to 100ms across
 *    4 tiers with variable bucket widths for both NVMe and NFS workloads.
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

/** Minimum relative throughput gain to consider improvement. If throughput
 *  increases by less than 1% relative to current throughput per unit of
 *  in-flight increase, consider it a plateau. */
#define ADAPTIVE_ER_EPSILON_RATIO 0.01

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
 *  even at very low IOPS; make a decision with available data.
 *  Not referenced directly; the tick proceeds when MIN is satisfied and
 *  MAX >= MIN guarantees the window is bounded. */
#define ADAPTIVE_MAX_SAMPLE_WINDOW_MS 1000
_Static_assert(ADAPTIVE_MAX_SAMPLE_WINDOW_MS >= ADAPTIVE_MIN_SAMPLE_WINDOW_MS,
               "MAX sample window must be >= MIN sample window");

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
 *  50ms covers NFS/network storage where latencies routinely exceed 10ms.
 *  NVMe SSDs (50-200µs typical) will converge to a much lower dynamic guard
 *  via the baseline_p99 * LATENCY_GUARD_MULT mechanism. This default only
 *  applies before baseline is established. */
#define ADAPTIVE_DEFAULT_LATENCY_GUARD 50.0

/** Settling phase duration in ticks. After backoff, wait 10 ticks (100ms)
 *  for metrics to stabilize before resuming PROBING. */
#define ADAPTIVE_SETTLING_TICKS 10

/** Ticks in STEADY before CONVERGED. Stay in STEADY for 500 ticks (5 seconds)
 *  with stable metrics before declaring tuning complete. */
#define ADAPTIVE_STEADY_THRESHOLD 500

/** Ticks in STEADY before re-probing. After backoff, re-enter PROBING after
 *  100 ticks (1 second) of stable metrics to recover from transient spikes.
 *  Only applies when the controller entered STEADY via BACKOFF, not plateau. */
#define ADAPTIVE_REPROBE_INTERVAL 100

/* Histogram configuration:
 *
 * Tiered histogram covering 0-100ms with variable bucket widths:
 *   Tier 0: 0-1ms    @ 10µs  (100 buckets)  — NVMe-class latencies
 *   Tier 1: 1-5ms    @ 50µs  (80 buckets)   — SSD saturation
 *   Tier 2: 5-20ms   @ 250µs (60 buckets)   — HDD / slow SSD
 *   Tier 3: 20-100ms @ 1ms   (80 buckets)   — NFS / network storage
 *
 * Total: 320 buckets (~1.3KB per histogram).
 * Operations exceeding 100ms go into the overflow bucket.
 */

/** Number of tiers in the latency histogram. */
#define LATENCY_TIER_COUNT 4

/** Total number of histogram buckets across all tiers. */
#define LATENCY_TIERED_BUCKET_COUNT 320

/** Maximum tracked latency in microseconds (100ms). */
#define LATENCY_TIERED_MAX_US 100000

/* Legacy aliases used by some internal code */
#define LATENCY_BUCKET_COUNT LATENCY_TIERED_BUCKET_COUNT
#define LATENCY_MAX_US LATENCY_TIERED_MAX_US

/**
 * Tier descriptor: defines a contiguous range of histogram buckets
 * with uniform width within the tier.
 */
typedef struct {
    int start_us;     /**< Lower bound of tier in microseconds */
    int width_us;     /**< Bucket width within this tier */
    int bucket_count; /**< Number of buckets in this tier */
    int base_bucket;  /**< Index of first bucket in this tier */
} latency_tier_t;

/** Tier definitions (compile-time constant). */
static const latency_tier_t LATENCY_TIERS[LATENCY_TIER_COUNT] = {
    {.start_us = 0, .width_us = 10, .bucket_count = 100, .base_bucket = 0},
    {.start_us = 1000, .width_us = 50, .bucket_count = 80, .base_bucket = 100},
    {.start_us = 5000, .width_us = 250, .bucket_count = 60, .base_bucket = 180},
    {.start_us = 20000, .width_us = 1000, .bucket_count = 80, .base_bucket = 240},
};

/**
 * Map a latency in microseconds to a histogram bucket index.
 *
 * Returns -1 if the latency exceeds the maximum tracked range (overflow).
 * Negative latencies are clamped to 0.
 */
static inline int latency_us_to_bucket(int64_t latency_us) {
    if (latency_us < 0) latency_us = 0;
    /* Reverse-scan tiers (most NVMe ops land in tier 0, but checking
     * from high to low lets us use a single comparison per tier). */
    for (int t = LATENCY_TIER_COUNT - 1; t >= 0; t--) {
        const latency_tier_t *tier = &LATENCY_TIERS[t];
        if (latency_us >= tier->start_us) {
            int offset = (int)(latency_us - tier->start_us) / tier->width_us;
            if (offset >= tier->bucket_count)
                return -1; /* overflow (only possible for last tier) */
            return tier->base_bucket + offset;
        }
    }
    return 0; /* should not reach here */
}

/**
 * Map a histogram bucket index back to the bucket midpoint in microseconds.
 *
 * Used for P99 reverse mapping: the midpoint is the best single-value
 * estimate of the latencies that fell into this bucket.
 */
static inline double bucket_to_midpoint_us(int bucket) {
    for (int t = LATENCY_TIER_COUNT - 1; t >= 0; t--) {
        const latency_tier_t *tier = &LATENCY_TIERS[t];
        if (bucket >= tier->base_bucket) {
            int offset = bucket - tier->base_bucket;
            return tier->start_us + (offset + 0.5) * tier->width_us;
        }
    }
    return 0.0;
}

/**
 * Get the upper bound of a histogram bucket in microseconds.
 */
static inline int bucket_upper_bound_us(int bucket) {
    for (int t = LATENCY_TIER_COUNT - 1; t >= 0; t--) {
        const latency_tier_t *tier = &LATENCY_TIERS[t];
        if (bucket >= tier->base_bucket) {
            int offset = bucket - tier->base_bucket;
            return tier->start_us + (offset + 1) * tier->width_us;
        }
    }
    return 0;
}

/* ============================================================================
 * Passthrough Mode Constants
 * ============================================================================
 *
 * Passthrough mode skips AIMD gating on the hot path when there's no I/O
 * pressure, eliminating overhead on page-cache workloads.
 */

/** Consecutive ticks of growing pending before engaging AIMD (80ms) */
#define AIMD_ENGAGE_TICKS 8

/** Minimum pending growth per tick to count as "growing" */
#define AIMD_ENGAGE_PENDING_DELTA 16

/** Consecutive flat-pending ticks after CONVERGED to re-enter passthrough (100ms) */
#define PASSTHROUGH_REENTER_TICKS 10

/** Maximum |pending delta| per tick to count as "flat" for re-entry */
#define PASSTHROUGH_REENTER_DELTA_MAX 2

/** Latency sample mask in passthrough mode: 1-in-64 */
#define PASSTHROUGH_SAMPLE_MASK 0x3F

/* ============================================================================
 * Types
 * ============================================================================ */

/**
 * Adaptive phase
 */
typedef enum {
    ADAPTIVE_PHASE_BASELINE,   /**< Collecting baseline latency */
    ADAPTIVE_PHASE_PROBING,    /**< Increasing in-flight limit */
    ADAPTIVE_PHASE_STEADY,     /**< Maintaining optimal config */
    ADAPTIVE_PHASE_BACKOFF,    /**< Reducing due to latency spike */
    ADAPTIVE_PHASE_SETTLING,   /**< Waiting for metrics to stabilize */
    ADAPTIVE_PHASE_CONVERGED,  /**< Tuning complete */
    ADAPTIVE_PHASE_PASSTHROUGH /**< No AIMD gating (default start state) */
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
    _Atomic uint64_t total_count;                   /**< Total samples */
} adaptive_histogram_t;

/**
 * Double-buffered histogram pair
 *
 * Two histograms with atomic index swap for O(1) reset.
 * Recording writes to active histogram, tick swaps and clears inactive.
 *
 * active_index is placed first so that reading it brings the first ~15
 * buckets of histogram[0] into the same cache line.  For NVMe workloads
 * most latencies fall in these low buckets, giving a single cache line
 * fetch for the index lookup + most common recording path.
 */
typedef struct {
    _Atomic int active_index;            /**< Active histogram (0 or 1) */
    adaptive_histogram_t histograms[2];  /**< Double buffer */
    adaptive_histogram_t *pending_reset; /**< Histogram to reset on next tick (tick-thread only) */
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
    _Atomic int current_in_flight_limit; /**< Current max concurrent ops */
    _Atomic int current_batch_threshold; /**< Current batch size before submit */
    _Atomic unsigned int submit_calls;   /**< Submit syscalls this period */
    _Atomic unsigned int sqes_submitted; /**< SQEs submitted this period */
    _Atomic int64_t sample_start_ns;     /**< Current sample start time */
    _Atomic uint64_t
        sample_bytes; /**< Bytes completed this sample (unsigned to avoid signed overflow) */
    _Atomic uint64_t current_p99_ms;         /**< Current sample P99 (double stored as uint64_t) */
    _Atomic uint64_t current_throughput_bps; /**< Current throughput (double stored as uint64_t) */
    _Atomic adaptive_phase_t phase;          /**< Current phase */
    _Atomic bool passthrough_mode;           /**< True = skip AIMD gating (hot read per submit) */
    int max_queue_depth;                     /**< Upper bound on in-flight */
    int min_in_flight;                       /**< Lower bound on in-flight */
    int prev_in_flight_limit;                /**< Previous limit for efficiency ratio */

    /* === Cache line 1+: Tick-only state (cold, accessed every 10ms) === */
    double baseline_p99_ms;             /**< Minimum observed P99 */
    double latency_rise_threshold;      /**< Threshold for latency backoff */
    double max_p99_ms;                  /**< Hard ceiling on P99 (0 = none) */
    double prev_throughput_bps;         /**< Previous throughput for efficiency ratio */
    int warmup_count;                   /**< Samples collected in warmup */
    int plateau_count;                  /**< Consecutive plateau samples */
    int steady_count;                   /**< Time in STEADY phase */
    int spike_count;                    /**< Consecutive latency spikes */
    int settling_timer;                 /**< Time in SETTLING phase */
    bool entered_via_backoff;           /**< STEADY was reached via BACKOFF (not plateau) */
    int passthrough_qualify_count;      /**< Consecutive flat-pending ticks toward re-entry */
    int pressure_qualify_count;         /**< Consecutive pressure ticks toward AIMD engage */
    int prev_pending_snapshot;          /**< Previous tick's pending_count for delta */
    _Atomic bool batch_threshold_fixed; /**< True = skip AIMD batch optimizer (write-once at init,
                                           read by tick thread) */
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

#ifndef NDEBUG
    _Atomic int tick_entered; /**< Debug: detect concurrent adaptive_tick calls */
#endif
} adaptive_controller_t;

_Static_assert(sizeof(double) == sizeof(uint64_t),
               "double-as-uint64_t punning requires matching width");
#if defined(__STDC_NO_ATOMICS__)
#    error "C11 atomics are required"
#elif defined(ATOMIC_LLONG_LOCK_FREE)
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,
               "Lock-free 64-bit atomics required for throughput/latency counters");
#endif

/* ============================================================================
 * Functions
 * ============================================================================ */

/**
 * Initialize adaptive controller
 *
 * @param ctrl            Controller to initialize
 * @param max_queue_depth Maximum in-flight limit
 * @param initial_inflight Starting in-flight limit
 * @param min_inflight    Minimum in-flight limit (floor for AIMD backoff)
 * @return 0 on success, -1 on error
 */
int adaptive_init(adaptive_controller_t *ctrl, int max_queue_depth, int initial_inflight,
                  int min_inflight);

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
 * NOT thread-safe: must be called from a single thread only (the tick thread).
 * Concurrent calls will corrupt internal state (sliding windows, counters).
 *
 * @param ctrl          Controller
 * @param pending_count Current number of in-flight operations
 * @return true if parameters changed, false otherwise
 */
bool adaptive_tick(adaptive_controller_t *ctrl, int pending_count);

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
 * Check if controller is in passthrough mode
 *
 * Inlined: called on every submission in ring_can_submit() and data_finish().
 *
 * @param ctrl Controller
 * @return true if passthrough (no AIMD gating)
 */
static inline bool adaptive_is_passthrough(const adaptive_controller_t *ctrl) {
    return atomic_load_explicit((_Atomic bool *)&ctrl->passthrough_mode, memory_order_relaxed);
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
 * O(1) operation - just swaps the active index.
 * The caller is responsible for resetting the returned (now-inactive)
 * histogram via adaptive_hist_reset().
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
