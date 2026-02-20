// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_engine.c
 * @brief Unit tests for AIMD adaptive controller
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "../src/adaptive_engine.h"

static int test_count = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)            \
    do {                          \
        printf("  %-40s", #name); \
        test_##name();            \
        printf(" OK\n");          \
        test_count++;             \
    } while (0)

/* ============================================================================
 * Histogram Tests
 * ============================================================================ */

TEST(histogram_basic) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);

    assert(hist.total_count == 0);
    assert(adaptive_hist_p99(&hist) < 0); /* No data */

    /* Record some values */
    for (int i = 0; i < 100; i++) {
        adaptive_hist_record(&hist, 100); /* 100us */
    }

    assert(hist.total_count == 100);
    double p99 = adaptive_hist_p99(&hist);
    assert(p99 > 0);
}

TEST(histogram_overflow) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);

    /* Record values above max tracked (100ms = 100000us) */
    for (int i = 0; i < 100; i++) {
        adaptive_hist_record(&hist, 150000); /* 150ms - overflow */
    }

    assert(hist.overflow == 100);
    assert(hist.total_count == 100);

    /* P99 should be at 2x max = 200ms */
    double p99 = adaptive_hist_p99(&hist);
    assert(p99 >= 100.0); /* >= 100ms */
}

TEST(histogram_distribution) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);

    /* Record 99 fast values and 1 slow value */
    for (int i = 0; i < 99; i++) {
        adaptive_hist_record(&hist, 50); /* 50us - fast */
    }
    adaptive_hist_record(&hist, 5000); /* 5ms - slow */

    assert(hist.total_count == 100);

    /* P99 should be the slow value (~5ms) */
    double p99 = adaptive_hist_p99(&hist);
    assert(p99 >= 4.0 && p99 <= 6.0); /* ~5ms */
}

/* ============================================================================
 * Controller Tests
 * ============================================================================ */

TEST(controller_init) {
    adaptive_controller_t ctrl;

    int ret = adaptive_init(&ctrl, 256, 32, 4);
    assert(ret == 0);
    assert(ctrl.max_queue_depth == 256);
    assert(ctrl.current_in_flight_limit == 32);
    assert(ctrl.phase == ADAPTIVE_PHASE_BASELINE);

    adaptive_destroy(&ctrl);
}

TEST(controller_init_invalid) {
    adaptive_controller_t ctrl;

    /* NULL controller */
    assert(adaptive_init(NULL, 256, 32, 4) == -1);

    /* Invalid queue depth */
    assert(adaptive_init(&ctrl, 0, 32, 4) == -1);

    /* Invalid initial inflight */
    assert(adaptive_init(&ctrl, 256, 0, 4) == -1);
}

TEST(controller_getters) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    assert(adaptive_get_inflight_limit(&ctrl) == 32);
    assert(adaptive_get_batch_threshold(&ctrl) == ADAPTIVE_MIN_BATCH);

    adaptive_destroy(&ctrl);
}

TEST(controller_phase_names) {
    assert(strcmp(adaptive_phase_name(ADAPTIVE_PHASE_BASELINE), "BASELINE") == 0);
    assert(strcmp(adaptive_phase_name(ADAPTIVE_PHASE_PROBING), "PROBING") == 0);
    assert(strcmp(adaptive_phase_name(ADAPTIVE_PHASE_STEADY), "STEADY") == 0);
    assert(strcmp(adaptive_phase_name(ADAPTIVE_PHASE_BACKOFF), "BACKOFF") == 0);
    assert(strcmp(adaptive_phase_name(ADAPTIVE_PHASE_SETTLING), "SETTLING") == 0);
    assert(strcmp(adaptive_phase_name(ADAPTIVE_PHASE_CONVERGED), "CONVERGED") == 0);
}

TEST(controller_record_completion) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* Record some completions */
    for (int i = 0; i < 100; i++) {
        adaptive_record_completion(&ctrl, 100000, 4096); /* 100us, 4KB */
    }

    adaptive_histogram_t *hist = adaptive_hist_active(&ctrl.hist_pair);
    assert(hist->total_count == 100);
    assert(ctrl.sample_bytes == 100 * 4096);

    adaptive_destroy(&ctrl);
}

TEST(controller_record_submit) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    adaptive_record_submit(&ctrl, 8);
    adaptive_record_submit(&ctrl, 8);

    assert(ctrl.submit_calls == 2);
    assert(ctrl.sqes_submitted == 16);

    adaptive_destroy(&ctrl);
}

TEST(controller_tick_baseline) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    assert(ctrl.phase == ADAPTIVE_PHASE_BASELINE);

    /* Run through warmup period */
    for (int i = 0; i < ADAPTIVE_WARMUP_SAMPLES; i++) {
        /* Simulate completions */
        for (int j = 0; j < 100; j++) {
            adaptive_record_completion(&ctrl, 100000, 4096);
        }
        adaptive_tick(&ctrl);
    }

    /* Should have transitioned to PROBING */
    assert(ctrl.phase == ADAPTIVE_PHASE_PROBING);

    adaptive_destroy(&ctrl);
}

TEST(controller_min_inflight) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* Force backoff repeatedly */
    ctrl.phase = ADAPTIVE_PHASE_BACKOFF;
    ctrl.current_in_flight_limit = 4;

    adaptive_tick(&ctrl);

    /* Should not go below min_in_flight */
    assert(ctrl.current_in_flight_limit >= ctrl.min_in_flight);

    adaptive_destroy(&ctrl);
}

TEST(controller_probing_additive_increase) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* Ensure adaptive_tick enters PROBING increase path:
     * - enough samples (>= LOW_IOPS_MIN_SAMPLES)
     * - no latency spike (p99 << default 10ms guard)
     * - positive efficiency ratio */
    for (int i = 0; i < ADAPTIVE_LOW_IOPS_MIN_SAMPLES; i++) {
        adaptive_record_completion(&ctrl, 100000, 4096); /* 100us */
    }

    ctrl.phase = ADAPTIVE_PHASE_PROBING;
    ctrl.prev_in_flight_limit = 31; /* current is 32 after init */
    ctrl.prev_throughput_bps = 0.0;

    bool changed = adaptive_tick(&ctrl);
    assert(changed == true);
    assert(ctrl.current_in_flight_limit == 32 + ADAPTIVE_AIMD_INCREASE);
    assert(ctrl.phase == ADAPTIVE_PHASE_PROBING);

    adaptive_destroy(&ctrl);
}

/* ============================================================================
 * Low-IOPS Sample Window Tests
 * ============================================================================ */

TEST(low_iops_skips_with_few_samples) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* Record only a few completions (< 20) */
    for (int i = 0; i < 5; i++) {
        adaptive_record_completion(&ctrl, 100000, 4096);
    }

    /* Tick should return false (skip) because:
     * - samples < 20
     * - time < 100ms (just started)
     */
    bool changed = adaptive_tick(&ctrl);
    assert(changed == false);

    /* Histogram should NOT be reset (still accumulating) */
    adaptive_histogram_t *hist = adaptive_hist_active(&ctrl.hist_pair);
    assert(hist->total_count == 5);

    adaptive_destroy(&ctrl);
}

TEST(low_iops_proceeds_with_min_samples) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* Record exactly 20 completions (minimum threshold) */
    for (int i = 0; i < ADAPTIVE_LOW_IOPS_MIN_SAMPLES; i++) {
        adaptive_record_completion(&ctrl, 100000, 4096);
    }

    /* Tick should proceed because we have enough samples */
    adaptive_tick(&ctrl);

    /* Active histogram should be empty after swap (we're now on the fresh one) */
    adaptive_histogram_t *hist = adaptive_hist_active(&ctrl.hist_pair);
    assert(hist->total_count == 0);

    adaptive_destroy(&ctrl);
}

TEST(low_iops_proceeds_with_min_time) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* Record only a few completions (< 20) */
    for (int i = 0; i < 5; i++) {
        adaptive_record_completion(&ctrl, 100000, 4096);
    }

    /* Simulate 100ms+ elapsed time */
    ctrl.sample_start_ns -= (ADAPTIVE_MIN_SAMPLE_WINDOW_MS + 10) * 1000000LL;

    /* Tick should proceed because enough time has passed */
    adaptive_tick(&ctrl);

    /* Active histogram should be empty after swap (we're now on the fresh one) */
    adaptive_histogram_t *hist = adaptive_hist_active(&ctrl.hist_pair);
    assert(hist->total_count == 0);

    adaptive_destroy(&ctrl);
}

TEST(low_iops_p99_validity_threshold) {
    adaptive_controller_t ctrl;
    adaptive_init(&ctrl, 256, 32, 4);

    /* With < 20 samples, latency_rising should NOT trigger backoff */
    for (int i = 0; i < 10; i++) {
        adaptive_record_completion(&ctrl, 50000000, 4096); /* 50ms - very slow */
    }

    /* Simulate time passing minimum window */
    ctrl.sample_start_ns -= 200 * 1000000LL; /* 200ms ago */

    /* Setup: in PROBING phase with low baseline */
    ctrl.phase = ADAPTIVE_PHASE_PROBING;
    ctrl.baseline_p99_ms = 1.0; /* 1ms baseline */
    ctrl.baseline_count = 10;

    adaptive_tick(&ctrl);

    /* Should NOT have transitioned to BACKOFF (not enough samples for valid P99) */
    /* With < 20 samples, have_valid_p99 is false, so latency_rising stays false */
    assert(ctrl.phase != ADAPTIVE_PHASE_BACKOFF);

    adaptive_destroy(&ctrl);
}

/* ============================================================================
 * Double-Buffered Histogram Tests
 * ============================================================================ */

TEST(histogram_swap) {
    adaptive_histogram_pair_t pair;
    adaptive_hist_pair_init(&pair);

    /* Initially active index is 0 */
    assert(pair.active_index == 0);

    adaptive_histogram_t *hist0 = adaptive_hist_active(&pair);
    assert(hist0 == &pair.histograms[0]);

    /* Record some values to histogram 0 */
    for (int i = 0; i < 50; i++) {
        adaptive_hist_record(hist0, 100); /* 100us */
    }
    assert(hist0->total_count == 50);

    /* Swap - should return pointer to old histogram (index 0) */
    adaptive_histogram_t *old = adaptive_hist_swap(&pair);
    assert(old == &pair.histograms[0]);
    assert(old->total_count == 50); /* Still has data for reading */

    /* Active is now histogram 1 */
    assert(pair.active_index == 1);
    adaptive_histogram_t *hist1 = adaptive_hist_active(&pair);
    assert(hist1 == &pair.histograms[1]);
    assert(hist1->total_count == 0); /* Fresh histogram */

    /* Record to new active histogram */
    for (int i = 0; i < 30; i++) {
        adaptive_hist_record(hist1, 200);
    }
    assert(hist1->total_count == 30);

    /* Swap again */
    old = adaptive_hist_swap(&pair);
    assert(old == &pair.histograms[1]);
    assert(old->total_count == 30);

    /* Back to histogram 0 */
    assert(pair.active_index == 0);
    adaptive_histogram_t *hist0_again = adaptive_hist_active(&pair);
    assert(hist0_again == &pair.histograms[0]);
    /* Note: hist0 still has old data until caller clears it */
}

#include <pthread.h>

#define CONCURRENT_THREADS 4
#define CONCURRENT_RECORDS 10000

static adaptive_histogram_pair_t *concurrent_pair;
static _Atomic int concurrent_running = 1;

static void *record_thread_func(void *arg) {
    (void)arg;
    int count = 0;

    while (atomic_load(&concurrent_running) && count < CONCURRENT_RECORDS) {
        adaptive_histogram_t *hist = adaptive_hist_active(concurrent_pair);
        adaptive_hist_record(hist, (count % 200) * 50); /* 0-10000us */
        count++;
    }

    return NULL;
}

static void *swap_thread_func(void *arg) {
    (void)arg;
    int swaps = 0;

    while (atomic_load(&concurrent_running) && swaps < 100) {
        /* Just swap - don't clear here to avoid race with recorders.
         * In real code, adaptive_tick() clears after reading P99,
         * which is serialized. This test just verifies swap is safe. */
        (void)adaptive_hist_swap(concurrent_pair);
        swaps++;

        /* Small delay to let recorders work */
        struct timespec ts = { 0, 1000000 }; /* 1ms */
        nanosleep(&ts, NULL);
    }

    atomic_store(&concurrent_running, 0);
    return NULL;
}

TEST(concurrent_record_swap) {
    adaptive_histogram_pair_t pair;
    adaptive_hist_pair_init(&pair);
    concurrent_pair = &pair;
    concurrent_running = 1;

    pthread_t recorders[CONCURRENT_THREADS];
    pthread_t swapper;

    /* Start recorder threads */
    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        int ret = pthread_create(&recorders[i], NULL, record_thread_func, NULL);
        assert(ret == 0);
    }

    /* Start swapper thread */
    int ret = pthread_create(&swapper, NULL, swap_thread_func, NULL);
    assert(ret == 0);

    /* Wait for all threads */
    pthread_join(swapper, NULL);
    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        pthread_join(recorders[i], NULL);
    }

    /* If we get here without crash/TSAN errors, the test passes */
    /* The counts may vary due to races, but no corruption should occur */
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("Running adaptive engine tests...\n");

    /* Histogram tests */
    RUN_TEST(histogram_basic);
    RUN_TEST(histogram_overflow);
    RUN_TEST(histogram_distribution);

    /* Double-buffered histogram tests */
    RUN_TEST(histogram_swap);
    RUN_TEST(concurrent_record_swap);

    /* Controller tests */
    RUN_TEST(controller_init);
    RUN_TEST(controller_init_invalid);
    RUN_TEST(controller_getters);
    RUN_TEST(controller_phase_names);
    RUN_TEST(controller_record_completion);
    RUN_TEST(controller_record_submit);
    RUN_TEST(controller_tick_baseline);
    RUN_TEST(controller_min_inflight);
    RUN_TEST(controller_probing_additive_increase);

    /* Low-IOPS sample window tests */
    RUN_TEST(low_iops_skips_with_few_samples);
    RUN_TEST(low_iops_proceeds_with_min_samples);
    RUN_TEST(low_iops_proceeds_with_min_time);
    RUN_TEST(low_iops_p99_validity_threshold);

    printf("\n%d tests passed\n", test_count);
    return 0;
}
