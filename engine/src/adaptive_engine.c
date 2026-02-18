/**
 * @file adaptive_engine.c
 * @brief AIMD congestion control implementation
 *
 * Implements dual-loop control for self-tuning I/O performance:
 * - Inner Loop: Batch size optimizer based on SQE/submit ratio
 * - Outer Loop: AIMD congestion control based on throughput efficiency ratio
 */

#define _GNU_SOURCE
#include "adaptive_engine.h"
#include "internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/**
 * Add value to circular sliding window.
 */
static void window_add(double *window, int *head, int *count, int max_size, double value) {
    window[*head] = value;
    *head = (*head + 1) % max_size;
    if (*count < max_size) {
        (*count)++;
    }
}

/**
 * Get minimum value from sliding window.
 */
static double window_min(const double *window, int count) {
    if (count == 0) return 0.0;
    double min_val = window[0];
    for (int i = 1; i < count; i++) {
        if (window[i] < min_val) {
            min_val = window[i];
        }
    }
    return min_val;
}

/**
 * Tick statistics computed from histogram swap and sample data.
 * Used to pass computed values from tick_swap_and_compute_stats() to state handlers.
 */
typedef struct {
    int sample_count;
    double p99_ms;
    double throughput_bps;
    double sqe_ratio;
    double latency_guard_ms;
    int64_t elapsed_ns;
    bool have_valid_p99;
    bool latency_rising;
    double efficiency_ratio;
    int delta_inflight;
} tick_stats_t;

/**
 * Swap histograms and compute statistics for current sample period.
 * Static inline to avoid call overhead on semi-hot path (runs every 10ms).
 * Preserves exact atomic ordering for correctness on weak-memory architectures.
 */
static inline tick_stats_t tick_swap_and_compute_stats(adaptive_controller_t *ctrl,
                                                       int64_t now_ns) {
    tick_stats_t stats = { 0 };
    stats.efficiency_ratio = NAN; /* NAN = no data; distinct from 0.0 = no change */

    /* Calculate elapsed time */
    int64_t start_ns = atomic_load_explicit(&ctrl->sample_start_ns, memory_order_acquire);
    stats.elapsed_ns = now_ns - start_ns;
    /* Guard against clock going backwards (NTP step, VM migration).
     * Clamp to 1ns to avoid division by zero or negative throughput. */
    if (stats.elapsed_ns <= 0) {
        stats.elapsed_ns = 1;
    }

    /* Swap histograms - O(1) atomic index flip.
     * Returns pointer to old histogram (now inactive) for reading stats. */
    adaptive_histogram_t *old_hist = adaptive_hist_swap(&ctrl->hist_pair);

    /* Re-read sample_count from the swapped-out histogram (now inactive).
     * This is the definitive count — no more writers after the swap. */
    stats.sample_count = atomic_load_explicit(&old_hist->total_count, memory_order_acquire);

    /* Calculate current sample statistics from the old (now inactive) histogram */
    stats.p99_ms = adaptive_hist_p99(old_hist);
    double elapsed_sec = (double)stats.elapsed_ns / 1e9;
    int64_t bytes = atomic_exchange_explicit(&ctrl->sample_bytes, 0, memory_order_acq_rel);
    stats.throughput_bps = (elapsed_sec > 0) ? (double)bytes / elapsed_sec : 0.0;

    /* Calculate SQE/submit ratio for batch optimizer */
    int calls = atomic_exchange_explicit(&ctrl->submit_calls, 0, memory_order_acq_rel);
    int sqes = atomic_exchange_explicit(&ctrl->sqes_submitted, 0, memory_order_acq_rel);
    stats.sqe_ratio = (calls > 0) ? (double)sqes / (double)calls : 0.0;

    /* Store current values (atomic for thread-safe stats access).
     * Use memory_order_release so readers with acquire see consistent values,
     * important for ARM/PowerPC with weak memory ordering. */
    atomic_store_explicit(&ctrl->current_throughput_bps, stats.throughput_bps,
                          memory_order_release);
    if (stats.p99_ms >= 0) {
        atomic_store_explicit(&ctrl->current_p99_ms, stats.p99_ms, memory_order_release);
    }

    /* Update sliding windows */
    if (stats.p99_ms >= 0) {
        window_add(ctrl->p99_window, &ctrl->p99_head, &ctrl->p99_count, ADAPTIVE_P99_WINDOW,
                   stats.p99_ms);
        /* Only add to baseline window during stable phases to prevent
         * congestion-inflated latency from raising the guard threshold. */
        adaptive_phase_t phase = atomic_load_explicit(&ctrl->phase, memory_order_relaxed);
        if (phase != ADAPTIVE_PHASE_BACKOFF &&
            !(phase == ADAPTIVE_PHASE_SETTLING && ctrl->entered_via_backoff)) {
            window_add(ctrl->baseline_window, &ctrl->baseline_head, &ctrl->baseline_count,
                       ADAPTIVE_BASELINE_WINDOW, stats.p99_ms);
        }
    }
    window_add(ctrl->throughput_window, &ctrl->throughput_head, &ctrl->throughput_count,
               ADAPTIVE_THROUGHPUT_WINDOW, stats.throughput_bps);

    /* Update baseline (sliding minimum P99) */
    if (ctrl->baseline_count > 0) {
        ctrl->baseline_p99_ms = window_min(ctrl->baseline_window, ctrl->baseline_count);
    }

    /* Determine effective latency guard threshold */
    if (ctrl->max_p99_ms > 0) {
        stats.latency_guard_ms = ctrl->max_p99_ms;
    } else if (ctrl->baseline_p99_ms > 0) {
        stats.latency_guard_ms = ctrl->baseline_p99_ms * ADAPTIVE_LATENCY_GUARD_MULT;
    } else {
        stats.latency_guard_ms = ADAPTIVE_DEFAULT_LATENCY_GUARD; /* Default if no baseline yet */
    }
    ctrl->latency_rise_threshold = stats.latency_guard_ms;

    /* Determine if latency is rising
     * With low IOPS, we accept fewer samples but require minimum threshold.
     * P99 with 20 samples is effectively P95, which is still useful.
     */
    stats.have_valid_p99 =
        (stats.sample_count >= ADAPTIVE_LOW_IOPS_MIN_SAMPLES && stats.p99_ms >= 0);
    stats.latency_rising = stats.have_valid_p99 && stats.p99_ms >= stats.latency_guard_ms;

    /* Calculate efficiency ratio: change in throughput per change in in-flight */
    int in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);
    if (ctrl->prev_in_flight_limit > 0 && in_flight_limit != ctrl->prev_in_flight_limit) {
        double delta_throughput = stats.throughput_bps - ctrl->prev_throughput_bps;
        int delta_inflight = in_flight_limit - ctrl->prev_in_flight_limit;
        if (delta_inflight != 0) {
            stats.efficiency_ratio = delta_throughput / (double)delta_inflight;
            stats.delta_inflight = delta_inflight;
        }
    }

    /* Clear the old histogram for next use using atomic stores.
     * This avoids TSAN warnings from mixing memset with atomic operations.
     *
     * Known limitation: there is a brief race window between the pointer swap
     * above and this reset where concurrent adaptive_hist_record() calls may
     * write to old_hist. Those ~1-3 samples are lost. Fixing this requires a
     * seq-lock or RCU approach, which is not worth the complexity for
     * statistical metrics that tolerate minor sample loss. */
    adaptive_hist_reset(old_hist);

    return stats;
}

/* ============================================================================
 * Histogram Operations
 * ============================================================================ */

void adaptive_hist_reset(adaptive_histogram_t *hist) {
    for (int i = 0; i < LATENCY_BUCKET_COUNT; i++) {
        atomic_store_explicit(&hist->buckets[i], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&hist->overflow, 0, memory_order_relaxed);
    atomic_store_explicit(&hist->total_count, 0, memory_order_release);
}

void adaptive_hist_record(adaptive_histogram_t *hist, int64_t latency_us) {
    if (latency_us < 0) latency_us = 0;

    int bucket = (int)(latency_us / LATENCY_BUCKET_WIDTH_US);
    if (bucket >= LATENCY_BUCKET_COUNT) {
        atomic_fetch_add_explicit(&hist->overflow, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&hist->buckets[bucket], 1, memory_order_relaxed);
    }
    /* Use release ordering on total_count so that a reader who does an
     * acquire load of total_count is guaranteed to see all preceding bucket
     * increments. On x86 (TSO) this is free; on ARM/POWER it emits a
     * store-release barrier. Without this, bucket reads in adaptive_hist_p99
     * could observe stale values on weak-memory architectures. */
    atomic_fetch_add_explicit(&hist->total_count, 1, memory_order_release);
}

double adaptive_hist_p99(adaptive_histogram_t *hist) {
    uint32_t total = atomic_load_explicit(&hist->total_count, memory_order_acquire);
    if (total == 0) {
        return -1.0; /* No data */
    }

    /* P99 = value at 99th percentile (1% from the top).
     * With fewer than 100 samples, 1% rounds to 0 — use 5% (P95) instead
     * to avoid degenerating to the single highest sample (the max), which
     * is far noisier and causes spurious backoff decisions. */
    uint32_t target;
    if (total >= 100) {
        target = (total + 99) / 100;
    } else {
        target = (total + 19) / 20; /* P95 for small sample sets */
        if (target == 0) target = 1;
    }

    /* Scan from high to low, counting down */
    uint32_t count = atomic_load_explicit(&hist->overflow, memory_order_relaxed);
    if (count >= target) {
        /* P99 is in overflow bucket (> 10ms) */
        return (double)LATENCY_MAX_US / 1000.0;
    }

    for (int i = LATENCY_BUCKET_COUNT - 1; i >= 0; i--) {
        count += atomic_load_explicit(&hist->buckets[i], memory_order_relaxed);
        if (count >= target) {
            /* P99 is in this bucket - return bucket midpoint in ms */
            double bucket_mid_us = (i + 0.5) * LATENCY_BUCKET_WIDTH_US;
            return bucket_mid_us / 1000.0;
        }
    }

    /* Bucket sum didn't reach target — likely a race between hist_reset and
     * a stale hist_record that incremented total_count after buckets were
     * zeroed.  Treat as no data rather than returning a spurious near-zero
     * value that could corrupt the baseline. */
    return -1.0;
}

/* ============================================================================
 * Double-Buffered Histogram Operations
 * ============================================================================ */

void adaptive_hist_pair_init(adaptive_histogram_pair_t *pair) {
    for (int h = 0; h < 2; h++) {
        for (int i = 0; i < LATENCY_BUCKET_COUNT; i++) {
            atomic_init(&pair->histograms[h].buckets[i], 0);
        }
        atomic_init(&pair->histograms[h].overflow, 0);
        atomic_init(&pair->histograms[h].total_count, 0);
    }
    atomic_init(&pair->active_index, 0);
}

adaptive_histogram_t *adaptive_hist_active(adaptive_histogram_pair_t *pair) {
    int idx = atomic_load_explicit(&pair->active_index, memory_order_acquire);
    return &pair->histograms[idx];
}

adaptive_histogram_t *adaptive_hist_swap(adaptive_histogram_pair_t *pair) {
    /* Atomically swap to the other histogram */
    int old_idx = atomic_fetch_xor_explicit(&pair->active_index, 1, memory_order_acq_rel);

    /* The old histogram is now inactive - it will be cleared for next use.
     * Note: There is a brief race window where concurrent adaptive_hist_record()
     * calls may still write to it between the swap and reset (see adaptive_tick). */
    adaptive_histogram_t *old_hist = &pair->histograms[old_idx];

    /* Return pointer to the old histogram (caller can read P99 from it) */
    return old_hist;
}

/* ============================================================================
 * Controller Lifecycle
 * ============================================================================ */

int adaptive_init(adaptive_controller_t *ctrl, int max_queue_depth, int initial_inflight) {
    if (!ctrl || max_queue_depth < 1 || initial_inflight < 1) {
        return -1;
    }

    if (initial_inflight > max_queue_depth) initial_inflight = max_queue_depth;

    /* Initialize atomics BEFORE zeroing non-atomic fields to avoid UB.
     * On platforms where _Atomic types use internal locks, memset would
     * destroy the lock state. Instead, init atomics first, then zero
     * the non-atomic fields individually. */
    atomic_init(&ctrl->current_in_flight_limit, initial_inflight);
    atomic_init(&ctrl->current_batch_threshold, ADAPTIVE_MIN_BATCH);
    atomic_init(&ctrl->current_p99_ms, 0.0);
    atomic_init(&ctrl->current_throughput_bps, 0.0);
    atomic_init(&ctrl->phase, ADAPTIVE_PHASE_BASELINE);
    atomic_init(&ctrl->submit_calls, 0);
    atomic_init(&ctrl->sqes_submitted, 0);
    atomic_init(&ctrl->sample_start_ns, get_time_ns());
    atomic_init(&ctrl->sample_bytes, 0);

    /* Zero all non-atomic fields */
    ctrl->max_queue_depth = max_queue_depth;
    ctrl->min_in_flight = 4; /* Never go below 4 */
    ctrl->baseline_p99_ms = 0.0;
    ctrl->latency_rise_threshold = 0.0;
    ctrl->max_p99_ms = 0.0;
    ctrl->prev_throughput_bps = 0.0;
    ctrl->warmup_count = 0;
    ctrl->plateau_count = 0;
    ctrl->steady_count = 0;
    ctrl->spike_count = 0;
    ctrl->settling_timer = 0;
    ctrl->entered_via_backoff = false;
    ctrl->prev_in_flight_limit = 0;
    ctrl->p99_head = 0;
    ctrl->p99_count = 0;
    ctrl->throughput_head = 0;
    ctrl->throughput_count = 0;
    ctrl->baseline_head = 0;
    ctrl->baseline_count = 0;
    memset(ctrl->p99_window, 0, sizeof(ctrl->p99_window));
    memset(ctrl->throughput_window, 0, sizeof(ctrl->throughput_window));
    memset(ctrl->baseline_window, 0, sizeof(ctrl->baseline_window));

    /* Initialize double-buffered histogram */
    adaptive_hist_pair_init(&ctrl->hist_pair);

#ifndef NDEBUG
    atomic_init(&ctrl->tick_entered, 0);
#endif

    return 0;
}

void adaptive_destroy(adaptive_controller_t *ctrl) {
    /* Nothing to free - all embedded */
    (void)ctrl;
}

/* ============================================================================
 * Hot Path Operations
 * ============================================================================ */

void adaptive_record_completion(adaptive_controller_t *ctrl, int64_t latency_ns, size_t bytes) {
    int64_t latency_us = latency_ns / 1000;
    adaptive_histogram_t *hist = adaptive_hist_active(&ctrl->hist_pair);
    adaptive_hist_record(hist, latency_us);
    atomic_fetch_add_explicit(&ctrl->sample_bytes, (int64_t)bytes, memory_order_relaxed);
}

void adaptive_record_submit(adaptive_controller_t *ctrl, int sqe_count) {
    atomic_fetch_add_explicit(&ctrl->submit_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctrl->sqes_submitted, sqe_count, memory_order_relaxed);
}

/* adaptive_get_inflight_limit() and adaptive_get_batch_threshold() are
 * static inline in adaptive_engine.h for zero-overhead access on the
 * per-submission hot path (ring_can_submit / ring_should_flush). */

const char *adaptive_phase_name(adaptive_phase_t phase) {
    switch (phase) {
    case ADAPTIVE_PHASE_BASELINE:
        return "BASELINE";
    case ADAPTIVE_PHASE_PROBING:
        return "PROBING";
    case ADAPTIVE_PHASE_STEADY:
        return "STEADY";
    case ADAPTIVE_PHASE_BACKOFF:
        return "BACKOFF";
    case ADAPTIVE_PHASE_SETTLING:
        return "SETTLING";
    case ADAPTIVE_PHASE_CONVERGED:
        return "CONVERGED";
    default:
        return "UNKNOWN";
    }
}

/* ============================================================================
 * AIMD State Machine Handlers
 * ============================================================================ */

/**
 * Handle BASELINE phase: Collect baseline metrics during warmup.
 */
static inline bool handle_baseline_phase(adaptive_controller_t *ctrl, const tick_stats_t *stats) {
    ctrl->warmup_count++;
    if (ctrl->warmup_count >= ADAPTIVE_WARMUP_SAMPLES) {
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_PROBING, memory_order_release);
        ctrl->prev_throughput_bps = stats->throughput_bps;
        ctrl->prev_in_flight_limit =
            atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);
    }
    return false; /* No params changed */
}

/**
 * Handle PROBING phase: Additive increase while throughput improves.
 */
static inline bool handle_probing_phase(adaptive_controller_t *ctrl, const tick_stats_t *stats) {
    bool params_changed = false;
    int in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);

    if (stats->latency_rising) {
        ctrl->spike_count++;
        if (ctrl->spike_count >= 2) {
            /* Latency spike - back off */
            atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_BACKOFF, memory_order_release);
            ctrl->spike_count = 0;
        }
    } else {
        ctrl->spike_count = 0;

        /* Check if we're still gaining throughput.
         * Use relative threshold: throughput must increase by at least
         * ADAPTIVE_ER_EPSILON_RATIO (1%) per unit of in-flight increase
         * relative to current throughput to be considered improvement. */
        double er_threshold = (stats->throughput_bps > 0 && stats->delta_inflight != 0)
                                  ? stats->throughput_bps * ADAPTIVE_ER_EPSILON_RATIO /
                                        fabs((double)stats->delta_inflight)
                                  : 0.0;
        if (stats->efficiency_ratio > er_threshold) {
            /* Still improving - increase (clamped to max) */
            if (in_flight_limit < ctrl->max_queue_depth) {
                in_flight_limit += ADAPTIVE_AIMD_INCREASE;
                if (in_flight_limit > ctrl->max_queue_depth)
                    in_flight_limit = ctrl->max_queue_depth;
                atomic_store_explicit(&ctrl->current_in_flight_limit, in_flight_limit,
                                      memory_order_release);
                params_changed = true;
            }
            ctrl->plateau_count = 0;
        } else if (ctrl->max_p99_ms > 0 && stats->have_valid_p99 &&
                   stats->p99_ms < ctrl->max_p99_ms) {
            /* Target-p99 mode: latency headroom remains, keep probing
             * even though throughput has plateaued. Push depth until
             * we approach the user's latency ceiling. */
            if (in_flight_limit < ctrl->max_queue_depth) {
                in_flight_limit += ADAPTIVE_AIMD_INCREASE;
                if (in_flight_limit > ctrl->max_queue_depth)
                    in_flight_limit = ctrl->max_queue_depth;
                atomic_store_explicit(&ctrl->current_in_flight_limit, in_flight_limit,
                                      memory_order_release);
                params_changed = true;
            }
            ctrl->plateau_count = 0;
        } else {
            /* Plateau detected */
            ctrl->plateau_count++;
            if (ctrl->plateau_count >= 3) {
                /* Confirmed plateau - enter steady */
                ctrl->entered_via_backoff = false;
                atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_SETTLING, memory_order_release);
                ctrl->settling_timer = 0;
            }
        }
    }

    return params_changed;
}

/**
 * Handle SETTLING phase: Wait for metrics to stabilize.
 */
static inline bool handle_settling_phase(adaptive_controller_t *ctrl, const tick_stats_t *stats) {
    /* If latency is still elevated during settling, re-enter BACKOFF immediately
     * rather than waiting for the settling timer to expire. Without this check,
     * sustained congestion causes a BACKOFF->SETTLING->STEADY->BACKOFF thrash
     * loop where each SETTLING window (100ms) is wasted ignoring the problem. */
    if (stats->latency_rising) {
        ctrl->entered_via_backoff = true;
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_BACKOFF, memory_order_release);
        ctrl->settling_timer = 0;
        return false;
    }
    ctrl->settling_timer++;
    if (ctrl->settling_timer >= ADAPTIVE_SETTLING_TICKS) {
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_STEADY, memory_order_release);
        ctrl->steady_count = 0;
    }
    return false; /* No params changed */
}

/**
 * Handle STEADY phase: Maintain current config, monitor for changes.
 */
static inline bool handle_steady_phase(adaptive_controller_t *ctrl, const tick_stats_t *stats) {
    ctrl->steady_count++;

    /* Check for latency spike */
    if (stats->latency_rising) {
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_BACKOFF, memory_order_release);
        ctrl->steady_count = 0;
    }
    /* Re-probe after backoff: the transient spike may have passed, so
     * try increasing in-flight again rather than staying at a reduced level. */
    else if (ctrl->entered_via_backoff && ctrl->steady_count >= ADAPTIVE_REPROBE_INTERVAL) {
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_PROBING, memory_order_release);
        ctrl->entered_via_backoff = false;
        ctrl->plateau_count = 0;
        ctrl->spike_count = 0;
    }
    /* Check for sustained steady state (only from plateau, not backoff) */
    else if (!ctrl->entered_via_backoff && ctrl->steady_count >= ADAPTIVE_STEADY_THRESHOLD) {
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_CONVERGED, memory_order_release);
    }

    return false; /* No params changed */
}

/**
 * Handle BACKOFF phase: Multiplicative decrease.
 */
static inline bool handle_backoff_phase(adaptive_controller_t *ctrl, const tick_stats_t *stats) {
    (void)stats; /* Unused in backoff phase */

    /* Multiplicative decrease: reduce by (1 - AIMD_DECREASE).
     * Use double multiplication + floor so the limit always strictly decreases
     * (ceil would leave small values unchanged, e.g. ceil(4 * 0.80) = 4).
     * Clamp to min_in_flight (>= 1). */
    int in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);
    int reduced = (int)floor((double)in_flight_limit * ADAPTIVE_AIMD_DECREASE);
    in_flight_limit = reduced > ctrl->min_in_flight ? reduced : ctrl->min_in_flight;
    atomic_store_explicit(&ctrl->current_in_flight_limit, in_flight_limit, memory_order_release);

    ctrl->plateau_count = 0;
    ctrl->entered_via_backoff = true;
    atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_SETTLING, memory_order_release);
    ctrl->settling_timer = 0;

    return true; /* Params changed */
}

/**
 * Handle CONVERGED phase: Monitor for workload changes.
 *
 * CONVERGED means tuning is complete for the current workload. However, if the
 * workload changes (sustained latency spike), we must re-enter PROBING to adapt.
 * Without this, a workload shift after convergence would cause permanent latency
 * degradation with no corrective action.
 */
static inline bool handle_converged_phase(adaptive_controller_t *ctrl, const tick_stats_t *stats) {
    if (stats->latency_rising) {
        ctrl->spike_count++;
        if (ctrl->spike_count >= 2) {
            /* Sustained latency spike — workload has changed, back off and re-tune */
            atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_BACKOFF, memory_order_release);
            ctrl->spike_count = 0;
            ctrl->steady_count = 0;
        }
    } else {
        ctrl->spike_count = 0;
    }
    return false; /* No params changed (BACKOFF will change them on next tick) */
}

/* ============================================================================
 * Control Loop
 * ============================================================================ */

bool adaptive_tick(adaptive_controller_t *ctrl) {
    /* NOT thread-safe: must be called from a single thread only.
     * Concurrent calls would double-swap the histogram (fetch_xor twice),
     * causing both callers to read the active histogram and corrupt state. */
#ifndef NDEBUG
    int prev = atomic_fetch_add_explicit(&ctrl->tick_entered, 1, memory_order_relaxed);
    assert(prev == 0 && "adaptive_tick called concurrently — not thread-safe");
#endif

    int64_t now_ns = get_time_ns();
    int64_t start_ns = atomic_load_explicit(&ctrl->sample_start_ns, memory_order_acquire);
    int64_t elapsed_ns = now_ns - start_ns;
    int64_t elapsed_ms = elapsed_ns / 1000000LL;
    bool params_changed = false;

    /*
     * Low-IOPS handling: Extend sample window when we don't have enough data.
     * We need EITHER enough samples OR enough time before making decisions.
     * This prevents noisy/invalid statistics with slow storage.
     *
     * Peek at active histogram's count to decide whether to skip this tick.
     */
    adaptive_histogram_t *active_hist = adaptive_hist_active(&ctrl->hist_pair);
    int peek_count = atomic_load_explicit(&active_hist->total_count, memory_order_acquire);
    bool have_min_samples = (peek_count >= ADAPTIVE_LOW_IOPS_MIN_SAMPLES);
    bool have_min_time = (elapsed_ms >= ADAPTIVE_MIN_SAMPLE_WINDOW_MS);
    bool hit_max_time = (elapsed_ms >= ADAPTIVE_MAX_SAMPLE_WINDOW_MS);

    /* If we don't have enough data and haven't hit max window, accumulate more */
    if (!have_min_samples && !have_min_time && !hit_max_time) {
#ifndef NDEBUG
        atomic_fetch_sub_explicit(&ctrl->tick_entered, 1, memory_order_relaxed);
#endif
        return false; /* Skip this tick, keep accumulating */
    }

    /* Swap histograms and compute statistics */
    tick_stats_t stats = tick_swap_and_compute_stats(ctrl, now_ns);

    /* =========== INNER LOOP: Batch Optimizer =========== */
    int batch_threshold =
        atomic_load_explicit(&ctrl->current_batch_threshold, memory_order_relaxed);
    if (stats.sqe_ratio > 0) {
        if (stats.sqe_ratio < ADAPTIVE_TARGET_SQE_RATIO &&
            batch_threshold < ADAPTIVE_MAX_BATCH_THRESHOLD) {
            batch_threshold++;
            atomic_store_explicit(&ctrl->current_batch_threshold, batch_threshold,
                                  memory_order_relaxed);
            params_changed = true;
        } else if (stats.sqe_ratio > ADAPTIVE_TARGET_SQE_RATIO * 1.5 &&
                   batch_threshold > ADAPTIVE_MIN_BATCH) {
            batch_threshold--;
            atomic_store_explicit(&ctrl->current_batch_threshold, batch_threshold,
                                  memory_order_relaxed);
            params_changed = true;
        }
    }

    /* Save prev_* BEFORE the state machine runs.  The efficiency ratio
     * on the NEXT tick needs to measure the effect of THIS tick's state
     * machine decision: delta_throughput = next_throughput - this_throughput,
     * delta_inflight = next_limit - this_limit.  If we saved after the
     * state machine, prev_in_flight_limit would already include this tick's
     * change, making the delta always zero on the next tick. */
    ctrl->prev_throughput_bps = stats.throughput_bps;
    ctrl->prev_in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);
    /* Compiler barrier: prevent reordering of the prev_* writes past the
     * state machine switch below.  These are non-atomic writes that the
     * compiler is otherwise free to reorder or defer. */
    __asm__ volatile("" ::: "memory");

    /* =========== OUTER LOOP: AIMD State Machine =========== */
    bool state_changed_params = false;
    switch (atomic_load_explicit(&ctrl->phase, memory_order_relaxed)) {
    case ADAPTIVE_PHASE_BASELINE:
        state_changed_params = handle_baseline_phase(ctrl, &stats);
        break;
    case ADAPTIVE_PHASE_PROBING:
        state_changed_params = handle_probing_phase(ctrl, &stats);
        break;
    case ADAPTIVE_PHASE_SETTLING:
        state_changed_params = handle_settling_phase(ctrl, &stats);
        break;
    case ADAPTIVE_PHASE_STEADY:
        state_changed_params = handle_steady_phase(ctrl, &stats);
        break;
    case ADAPTIVE_PHASE_BACKOFF:
        state_changed_params = handle_backoff_phase(ctrl, &stats);
        break;
    case ADAPTIVE_PHASE_CONVERGED:
        state_changed_params = handle_converged_phase(ctrl, &stats);
        break;
    }
    params_changed = params_changed || state_changed_params;

    /* Reset sample start for next period.
     * submit_calls, sqes_submitted, sample_bytes, and histogram are already reset
     * by tick_swap_and_compute_stats(). */
    atomic_store_explicit(&ctrl->sample_start_ns, now_ns, memory_order_release);

#ifndef NDEBUG
    atomic_fetch_sub_explicit(&ctrl->tick_entered, 1, memory_order_relaxed);
#endif
    return params_changed;
}
