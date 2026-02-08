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
    atomic_fetch_add_explicit(&hist->total_count, 1, memory_order_relaxed);
}

double adaptive_hist_p99(adaptive_histogram_t *hist) {
    uint32_t total = atomic_load_explicit(&hist->total_count, memory_order_acquire);
    if (total == 0) {
        return -1.0; /* No data */
    }

    /* P99 = value at 99th percentile (1% from the top) */
    uint32_t target = total / 100; /* 1% of samples */
    if (target == 0) target = 1;

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

    /* Should not reach here, but return lowest bucket */
    return (double)LATENCY_BUCKET_WIDTH_US / 2000.0;
}

/* ============================================================================
 * Double-Buffered Histogram Operations
 * ============================================================================ */

void adaptive_hist_pair_init(adaptive_histogram_pair_t *pair) {
    memset(pair->histograms, 0, sizeof(pair->histograms));
    atomic_init(&pair->active_index, 0);
}

adaptive_histogram_t *adaptive_hist_active(adaptive_histogram_pair_t *pair) {
    int idx = atomic_load_explicit(&pair->active_index, memory_order_acquire);
    return &pair->histograms[idx];
}

adaptive_histogram_t *adaptive_hist_swap(adaptive_histogram_pair_t *pair) {
    /* Atomically swap to the other histogram */
    int old_idx = atomic_fetch_xor_explicit(&pair->active_index, 1, memory_order_acq_rel);

    /* The old histogram is now inactive - clear it for next use.
     * No atomic stores needed since no one is writing to it anymore. */
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

    memset(ctrl, 0, sizeof(*ctrl));

    ctrl->max_queue_depth = max_queue_depth;
    ctrl->min_in_flight = 4; /* Never go below 4 */
    atomic_init(&ctrl->current_in_flight_limit, initial_inflight);
    atomic_init(&ctrl->current_batch_threshold, ADAPTIVE_MIN_BATCH);

    atomic_init(&ctrl->current_p99_ms, 0.0);
    atomic_init(&ctrl->current_throughput_bps, 0.0);
    atomic_init(&ctrl->phase, ADAPTIVE_PHASE_BASELINE);
    atomic_store_explicit(&ctrl->sample_start_ns, get_time_ns(), memory_order_release);
    atomic_store_explicit(&ctrl->sample_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->submit_calls, 0, memory_order_relaxed);
    atomic_store_explicit(&ctrl->sqes_submitted, 0, memory_order_relaxed);

    /* Initialize double-buffered histogram */
    adaptive_hist_pair_init(&ctrl->hist_pair);

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
 * Control Loop
 * ============================================================================ */

bool adaptive_tick(adaptive_controller_t *ctrl) {
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
        return false; /* Skip this tick, keep accumulating */
    }

    /* Swap histograms - O(1) atomic index flip.
     * Returns pointer to old histogram (now inactive) for reading stats. */
    adaptive_histogram_t *old_hist = adaptive_hist_swap(&ctrl->hist_pair);

    /* Re-read sample_count from the swapped-out histogram (now inactive).
     * This is the definitive count — no more writers after the swap. */
    int sample_count = atomic_load_explicit(&old_hist->total_count, memory_order_acquire);

    /* Calculate current sample statistics from the old (now inactive) histogram */
    double p99_ms = adaptive_hist_p99(old_hist);
    double elapsed_sec = (double)elapsed_ns / 1e9;
    int64_t bytes = atomic_exchange_explicit(&ctrl->sample_bytes, 0, memory_order_relaxed);
    double throughput_bps = (elapsed_sec > 0) ? (double)bytes / elapsed_sec : 0.0;

    /* Calculate SQE/submit ratio for batch optimizer */
    double sqe_ratio = 0.0;
    int calls = atomic_exchange_explicit(&ctrl->submit_calls, 0, memory_order_relaxed);
    int sqes = atomic_exchange_explicit(&ctrl->sqes_submitted, 0, memory_order_relaxed);
    if (calls > 0) {
        sqe_ratio = (double)sqes / (double)calls;
    }

    /* Store current values (atomic for thread-safe stats access).
     * Use memory_order_release so readers with acquire see consistent values,
     * important for ARM/PowerPC with weak memory ordering. */
    atomic_store_explicit(&ctrl->current_throughput_bps, throughput_bps, memory_order_release);
    if (p99_ms >= 0) {
        atomic_store_explicit(&ctrl->current_p99_ms, p99_ms, memory_order_release);
    }

    /* Update sliding windows */
    if (p99_ms >= 0) {
        window_add(ctrl->p99_window, &ctrl->p99_head, &ctrl->p99_count, ADAPTIVE_P99_WINDOW,
                   p99_ms);
        window_add(ctrl->baseline_window, &ctrl->baseline_head, &ctrl->baseline_count,
                   ADAPTIVE_BASELINE_WINDOW, p99_ms);
    }
    window_add(ctrl->throughput_window, &ctrl->throughput_head, &ctrl->throughput_count,
               ADAPTIVE_THROUGHPUT_WINDOW, throughput_bps);

    /* Update baseline (sliding minimum P99) */
    if (ctrl->baseline_count > 0) {
        ctrl->baseline_p99_ms = window_min(ctrl->baseline_window, ctrl->baseline_count);
    }

    /* Determine effective latency guard threshold */
    double latency_guard_ms;
    if (ctrl->max_p99_ms > 0) {
        latency_guard_ms = ctrl->max_p99_ms;
    } else if (ctrl->baseline_p99_ms > 0) {
        latency_guard_ms = ctrl->baseline_p99_ms * ADAPTIVE_LATENCY_GUARD_MULT;
    } else {
        latency_guard_ms = ADAPTIVE_DEFAULT_LATENCY_GUARD; /* Default if no baseline yet */
    }
    ctrl->latency_rise_threshold = latency_guard_ms;

    /* =========== INNER LOOP: Batch Optimizer =========== */
    int batch_threshold =
        atomic_load_explicit(&ctrl->current_batch_threshold, memory_order_relaxed);
    if (sqe_ratio > 0) {
        if (sqe_ratio < ADAPTIVE_TARGET_SQE_RATIO &&
            batch_threshold < ADAPTIVE_MAX_BATCH_THRESHOLD) {
            batch_threshold++;
            atomic_store_explicit(&ctrl->current_batch_threshold, batch_threshold,
                                  memory_order_relaxed);
            params_changed = true;
        } else if (sqe_ratio > ADAPTIVE_TARGET_SQE_RATIO * 1.5 &&
                   batch_threshold > ADAPTIVE_MIN_BATCH) {
            batch_threshold--;
            atomic_store_explicit(&ctrl->current_batch_threshold, batch_threshold,
                                  memory_order_relaxed);
            params_changed = true;
        }
    }

    /* =========== OUTER LOOP: AIMD State Machine =========== */

    /* Determine if latency is rising
     * With low IOPS, we accept fewer samples but require minimum threshold.
     * P99 with 20 samples is effectively P95, which is still useful.
     */
    bool latency_rising = false;
    bool have_valid_p99 = (sample_count >= ADAPTIVE_LOW_IOPS_MIN_SAMPLES && p99_ms >= 0);
    if (have_valid_p99) {
        if (p99_ms > latency_guard_ms) {
            latency_rising = true;
        }
    }

    /* Load current in-flight limit for use in state machine */
    int in_flight_limit =
        atomic_load_explicit(&ctrl->current_in_flight_limit, memory_order_relaxed);

    /* Calculate efficiency ratio: change in throughput per change in in-flight */
    double efficiency_ratio = 0.0;
    if (ctrl->prev_in_flight_limit > 0 && in_flight_limit != ctrl->prev_in_flight_limit) {
        double delta_throughput = throughput_bps - ctrl->prev_throughput_bps;
        int delta_inflight = in_flight_limit - ctrl->prev_in_flight_limit;
        if (delta_inflight != 0) {
            efficiency_ratio = delta_throughput / (double)delta_inflight;
        }
    }

    switch (ctrl->phase) {
    case ADAPTIVE_PHASE_BASELINE:
        /* Collect baseline metrics */
        ctrl->warmup_count++;
        if (ctrl->warmup_count >= ADAPTIVE_WARMUP_SAMPLES) {
            atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_PROBING, memory_order_release);
            ctrl->prev_throughput_bps = throughput_bps;
            ctrl->prev_in_flight_limit = in_flight_limit;
        }
        break;

    case ADAPTIVE_PHASE_PROBING:
        /* Additive increase while throughput improves */
        if (latency_rising) {
            ctrl->spike_count++;
            if (ctrl->spike_count >= 2) {
                /* Latency spike - back off */
                atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_BACKOFF, memory_order_release);
                ctrl->spike_count = 0;
            }
        } else {
            ctrl->spike_count = 0;

            /* Check if we're still gaining throughput */
            if (efficiency_ratio > ADAPTIVE_ER_EPSILON) {
                /* Still improving - increase */
                if (in_flight_limit < ctrl->max_queue_depth) {
                    in_flight_limit += ADAPTIVE_AIMD_INCREASE;
                    atomic_store_explicit(&ctrl->current_in_flight_limit, in_flight_limit,
                                          memory_order_relaxed);
                    params_changed = true;
                }
                ctrl->plateau_count = 0;
            } else {
                /* Plateau detected */
                ctrl->plateau_count++;
                if (ctrl->plateau_count >= 3) {
                    /* Confirmed plateau - enter steady */
                    atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_SETTLING,
                                          memory_order_release);
                    ctrl->settling_timer = 0;
                }
            }
        }
        break;

    case ADAPTIVE_PHASE_SETTLING:
        /* Wait for metrics to stabilize */
        ctrl->settling_timer++;
        if (ctrl->settling_timer >= ADAPTIVE_SETTLING_TICKS) {
            atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_STEADY, memory_order_release);
            ctrl->steady_count = 0;
        }
        break;

    case ADAPTIVE_PHASE_STEADY:
        /* Maintain current config, monitor for changes */
        ctrl->steady_count++;

        /* Check for latency spike */
        if (latency_rising) {
            atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_BACKOFF, memory_order_release);
            ctrl->steady_count = 0;
        }
        /* Check for sustained steady state */
        else if (ctrl->steady_count >= ADAPTIVE_STEADY_THRESHOLD) {
            atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_CONVERGED, memory_order_release);
        }
        break;

    case ADAPTIVE_PHASE_BACKOFF:
        /* Multiplicative decrease */
        in_flight_limit = (int)(in_flight_limit * ADAPTIVE_AIMD_DECREASE);
        if (in_flight_limit < ctrl->min_in_flight) {
            in_flight_limit = ctrl->min_in_flight;
        }
        atomic_store_explicit(&ctrl->current_in_flight_limit, in_flight_limit,
                              memory_order_relaxed);
        params_changed = true;
        ctrl->plateau_count = 0;
        atomic_store_explicit(&ctrl->phase, ADAPTIVE_PHASE_SETTLING, memory_order_release);
        ctrl->settling_timer = 0;
        break;

    case ADAPTIVE_PHASE_CONVERGED:
        /* No more changes - just maintain current config */
        break;
    }

    /* Save for next ER calculation */
    ctrl->prev_throughput_bps = throughput_bps;
    ctrl->prev_in_flight_limit = in_flight_limit;

    /* Clear the old histogram for next use using atomic stores.
     * This avoids TSAN warnings from mixing memset with atomic operations.
     *
     * Known limitation: there is a brief race window between the pointer swap
     * above and this reset where concurrent adaptive_hist_record() calls may
     * write to old_hist. Those ~1-3 samples are lost. Fixing this requires a
     * seq-lock or RCU approach, which is not worth the complexity for
     * statistical metrics that tolerate minor sample loss. */
    adaptive_hist_reset(old_hist);

    /* Reset sample start for next period.
     * submit_calls, sqes_submitted, and sample_bytes are already reset
     * by atomic_exchange at the top of this function. */
    atomic_store_explicit(&ctrl->sample_start_ns, now_ns, memory_order_release);

    return params_changed;
}
