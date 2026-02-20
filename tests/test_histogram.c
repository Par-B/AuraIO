// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file test_histogram.c
 * @brief Unit tests for tiered latency histogram
 *
 * Tests the tiered histogram mapping functions and histogram operations
 * in isolation, without requiring a full engine instance (except for
 * the public API test).
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <aura.h>
#include "../engine/src/adaptive_engine.h"

/* ============================================================================
 * Test Framework (minimal)
 * ============================================================================ */

static int tests_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void) { \
        printf("  %-50s", #name);  \
        test_##name();             \
        printf("PASS\n");          \
        tests_passed++;            \
    }                              \
    static void test_##name(void)

#define RUN_TEST(name) run_##name()

/* ============================================================================
 * Tier Boundary Mapping Tests
 * ============================================================================ */

TEST(tier_boundary_0us) {
    assert(latency_us_to_bucket(0) == 0);
}

TEST(tier_boundary_999us) {
    /* 999µs is in tier 0 (0-1ms, 10µs width): bucket = 999/10 = 99 */
    assert(latency_us_to_bucket(999) == 99);
}

TEST(tier_boundary_1000us) {
    /* 1000µs is start of tier 1: bucket = 100 */
    assert(latency_us_to_bucket(1000) == 100);
}

TEST(tier_boundary_4999us) {
    /* 4999µs is in tier 1 (1-5ms, 50µs width): offset = (4999-1000)/50 = 79 */
    assert(latency_us_to_bucket(4999) == 100 + 79);
}

TEST(tier_boundary_5000us) {
    /* 5000µs is start of tier 2: bucket = 180 */
    assert(latency_us_to_bucket(5000) == 180);
}

TEST(tier_boundary_19999us) {
    /* 19999µs is in tier 2 (5-20ms, 250µs width): offset = (19999-5000)/250 = 59 */
    assert(latency_us_to_bucket(19999) == 180 + 59);
}

TEST(tier_boundary_20000us) {
    /* 20000µs is start of tier 3: bucket = 240 */
    assert(latency_us_to_bucket(20000) == 240);
}

TEST(tier_boundary_99999us) {
    /* 99999µs is in tier 3 (20-100ms, 1000µs width): offset = (99999-20000)/1000 = 79 */
    assert(latency_us_to_bucket(99999) == 240 + 79);
}

TEST(tier_boundary_overflow) {
    /* 100000µs should overflow */
    assert(latency_us_to_bucket(100000) == -1);
    assert(latency_us_to_bucket(200000) == -1);
}

TEST(negative_latency_clamped) {
    /* Negative latencies should map to bucket 0 */
    assert(latency_us_to_bucket(-1) == 0);
    assert(latency_us_to_bucket(-1000) == 0);
}

/* ============================================================================
 * Midpoint Round-Trip Tests
 * ============================================================================ */

TEST(midpoint_tier0) {
    /* Bucket 50 in tier 0: midpoint = 50 * 10 + 5 = 505 µs */
    double mid = bucket_to_midpoint_us(50);
    assert(fabs(mid - 505.0) < 0.01);
}

TEST(midpoint_tier1) {
    /* Bucket 120 = tier 1, offset 20: midpoint = 1000 + 20*50 + 25 = 2025 µs */
    double mid = bucket_to_midpoint_us(120);
    assert(fabs(mid - 2025.0) < 0.01);
}

TEST(midpoint_tier2) {
    /* Bucket 200 = tier 2, offset 20: midpoint = 5000 + 20*250 + 125 = 10125 µs */
    double mid = bucket_to_midpoint_us(200);
    assert(fabs(mid - 10125.0) < 0.01);
}

TEST(midpoint_tier3) {
    /* Bucket 260 = tier 3, offset 20: midpoint = 20000 + 20*1000 + 500 = 40500 µs */
    double mid = bucket_to_midpoint_us(260);
    assert(fabs(mid - 40500.0) < 0.01);
}

TEST(upper_bound_tier0) {
    /* Bucket 0: upper = 10 µs */
    assert(bucket_upper_bound_us(0) == 10);
    /* Bucket 99: upper = 1000 µs */
    assert(bucket_upper_bound_us(99) == 1000);
}

TEST(upper_bound_tier1) {
    /* Bucket 100: upper = 1000 + 50 = 1050 µs */
    assert(bucket_upper_bound_us(100) == 1050);
}

TEST(upper_bound_tier3_last) {
    /* Bucket 319: upper = 20000 + 80*1000 = 100000 µs */
    assert(bucket_upper_bound_us(319) == 100000);
}

/* ============================================================================
 * Midpoint Round-Trip Accuracy Tests
 * ============================================================================ */

TEST(roundtrip_tier0) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    /* Record a single sample at 500µs */
    adaptive_hist_record(&hist, 500);
    double p99 = adaptive_hist_p99(&hist);
    /* Should return midpoint of bucket containing 500µs.
     * Bucket 50 (500-510µs), midpoint = 505µs = 0.505ms */
    assert(p99 > 0.0);
    assert(fabs(p99 - 0.505) < 0.02); /* within one bucket width */
}

TEST(roundtrip_tier1) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    adaptive_hist_record(&hist, 2500);
    double p99 = adaptive_hist_p99(&hist);
    /* 2500µs → tier 1, offset (2500-1000)/50 = 30, midpoint = 1000+30*50+25 = 2525µs = 2.525ms */
    assert(p99 > 0.0);
    assert(fabs(p99 - 2.525) < 0.06);
}

TEST(roundtrip_tier2) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    adaptive_hist_record(&hist, 10000);
    double p99 = adaptive_hist_p99(&hist);
    /* 10000µs → tier 2, offset (10000-5000)/250 = 20, midpoint = 5000+20*250+125 = 10125µs */
    assert(p99 > 0.0);
    assert(fabs(p99 - 10.125) < 0.3);
}

TEST(roundtrip_tier3) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    adaptive_hist_record(&hist, 50000);
    double p99 = adaptive_hist_p99(&hist);
    /* 50000µs → tier 3, offset (50000-20000)/1000 = 30, midpoint = 20000+30*1000+500 = 50500µs */
    assert(p99 > 0.0);
    assert(fabs(p99 - 50.5) < 1.1);
}

/* ============================================================================
 * Overflow Handling Tests
 * ============================================================================ */

TEST(overflow_recording) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    adaptive_hist_record(&hist, 150000); /* 150ms > 100ms max */
    assert(atomic_load(&hist.overflow) == 1);
    assert(atomic_load(&hist.total_count) == 1);
    /* P99 of overflow should return 2x max = 200ms */
    double p99 = adaptive_hist_p99(&hist);
    assert(fabs(p99 - 200.0) < 0.01);
}

/* ============================================================================
 * P99 Accuracy Tests
 * ============================================================================ */

TEST(p99_accuracy_tier0) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    /* Fill with 1000 samples uniformly in tier 0 (0-1000µs) */
    for (int i = 0; i < 1000; i++) {
        adaptive_hist_record(&hist, i);
    }
    double p99 = adaptive_hist_p99(&hist);
    /* P99 of uniform 0-999 should be near 990µs = 0.99ms */
    assert(p99 > 0.9 && p99 < 1.1);
}

TEST(p99_accuracy_tier3) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    /* Fill with 1000 samples uniformly in tier 3 (20-100ms) */
    for (int i = 0; i < 1000; i++) {
        int64_t latency = 20000 + (int64_t)i * 80;
        adaptive_hist_record(&hist, latency);
    }
    double p99 = adaptive_hist_p99(&hist);
    /* P99 of uniform 20000-99920 should be near 99ms */
    assert(p99 > 90.0 && p99 < 105.0);
}

/* ============================================================================
 * Empty Histogram Tests
 * ============================================================================ */

TEST(empty_histogram_p99) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    double p99 = adaptive_hist_p99(&hist);
    assert(p99 == -1.0);
}

/* ============================================================================
 * Reset Tests
 * ============================================================================ */

TEST(reset_correctness) {
    adaptive_histogram_t hist;
    adaptive_hist_reset(&hist);
    for (int i = 0; i < 100; i++) {
        adaptive_hist_record(&hist, i * 10);
    }
    assert(atomic_load(&hist.total_count) == 100);
    adaptive_hist_reset(&hist);
    assert(atomic_load(&hist.total_count) == 0);
    assert(adaptive_hist_p99(&hist) == -1.0);
}

/* ============================================================================
 * Double-Buffer Swap Tests
 * ============================================================================ */

TEST(double_buffer_swap) {
    adaptive_histogram_pair_t pair;
    adaptive_hist_pair_init(&pair);

    /* Record to active */
    adaptive_histogram_t *active = adaptive_hist_active(&pair);
    adaptive_hist_record(active, 500);
    assert(atomic_load(&active->total_count) == 1);

    /* Swap */
    adaptive_histogram_t *old = adaptive_hist_swap(&pair);
    assert(atomic_load(&old->total_count) == 1);

    /* New active should be clean */
    adaptive_histogram_t *new_active = adaptive_hist_active(&pair);
    assert(new_active != old);
    assert(atomic_load(&new_active->total_count) == 0);
}

/* ============================================================================
 * Public API Tests
 * ============================================================================ */

TEST(public_api_percentile) {
    aura_histogram_t hist;
    memset(&hist, 0, sizeof(hist));

    /* Populate tier metadata (as aura_get_histogram would) */
    hist.tier_count = AURA_HISTOGRAM_TIER_COUNT;
    for (int t = 0; t < LATENCY_TIER_COUNT; t++) {
        hist.tier_start_us[t] = LATENCY_TIERS[t].start_us;
        hist.tier_width_us[t] = LATENCY_TIERS[t].width_us;
        hist.tier_base_bucket[t] = LATENCY_TIERS[t].base_bucket;
    }
    hist.max_tracked_us = LATENCY_TIERED_MAX_US;

    /* Put 100 samples in bucket 50 (tier 0, 500-510µs) */
    hist.buckets[50] = 100;
    hist.total_count = 100;

    double p50 = aura_histogram_percentile(&hist, 50.0);
    assert(p50 > 0.4 && p50 < 0.6);

    double p99 = aura_histogram_percentile(&hist, 99.0);
    assert(p99 > 0.4 && p99 < 0.6);
}

TEST(public_api_bucket_upper_bound) {
    aura_histogram_t hist;
    memset(&hist, 0, sizeof(hist));
    hist.tier_count = AURA_HISTOGRAM_TIER_COUNT;
    for (int t = 0; t < LATENCY_TIER_COUNT; t++) {
        hist.tier_start_us[t] = LATENCY_TIERS[t].start_us;
        hist.tier_width_us[t] = LATENCY_TIERS[t].width_us;
        hist.tier_base_bucket[t] = LATENCY_TIERS[t].base_bucket;
    }

    assert(aura_histogram_bucket_upper_bound_us(&hist, 0) == 10);
    assert(aura_histogram_bucket_upper_bound_us(&hist, 99) == 1000);
    assert(aura_histogram_bucket_upper_bound_us(&hist, 100) == 1050);
    assert(aura_histogram_bucket_upper_bound_us(&hist, 319) == 100000);
    assert(aura_histogram_bucket_upper_bound_us(&hist, -1) == 0);
    assert(aura_histogram_bucket_upper_bound_us(&hist, 320) == 0);
    assert(aura_histogram_bucket_upper_bound_us(NULL, 0) == 0);
}

TEST(public_api_percentile_empty) {
    aura_histogram_t hist;
    memset(&hist, 0, sizeof(hist));
    double val = aura_histogram_percentile(&hist, 50.0);
    assert(val == -1.0);
}

TEST(public_api_percentile_null) {
    double val = aura_histogram_percentile(NULL, 50.0);
    assert(val == -1.0);
}

TEST(public_api_percentile_out_of_range) {
    aura_histogram_t hist;
    memset(&hist, 0, sizeof(hist));
    hist.total_count = 1;
    hist.buckets[0] = 1;
    assert(aura_histogram_percentile(&hist, -1.0) == -1.0);
    assert(aura_histogram_percentile(&hist, 101.0) == -1.0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== test_histogram ===\n");

    /* Tier boundary mapping */
    RUN_TEST(tier_boundary_0us);
    RUN_TEST(tier_boundary_999us);
    RUN_TEST(tier_boundary_1000us);
    RUN_TEST(tier_boundary_4999us);
    RUN_TEST(tier_boundary_5000us);
    RUN_TEST(tier_boundary_19999us);
    RUN_TEST(tier_boundary_20000us);
    RUN_TEST(tier_boundary_99999us);
    RUN_TEST(tier_boundary_overflow);
    RUN_TEST(negative_latency_clamped);

    /* Midpoint */
    RUN_TEST(midpoint_tier0);
    RUN_TEST(midpoint_tier1);
    RUN_TEST(midpoint_tier2);
    RUN_TEST(midpoint_tier3);
    RUN_TEST(upper_bound_tier0);
    RUN_TEST(upper_bound_tier1);
    RUN_TEST(upper_bound_tier3_last);

    /* Round-trip accuracy */
    RUN_TEST(roundtrip_tier0);
    RUN_TEST(roundtrip_tier1);
    RUN_TEST(roundtrip_tier2);
    RUN_TEST(roundtrip_tier3);

    /* Overflow */
    RUN_TEST(overflow_recording);

    /* P99 accuracy */
    RUN_TEST(p99_accuracy_tier0);
    RUN_TEST(p99_accuracy_tier3);

    /* Empty / reset */
    RUN_TEST(empty_histogram_p99);
    RUN_TEST(reset_correctness);

    /* Double-buffer */
    RUN_TEST(double_buffer_swap);

    /* Public API */
    RUN_TEST(public_api_percentile);
    RUN_TEST(public_api_bucket_upper_bound);
    RUN_TEST(public_api_percentile_empty);
    RUN_TEST(public_api_percentile_null);
    RUN_TEST(public_api_percentile_out_of_range);

    printf("\n%d tests passed\n", tests_passed);
    return 0;
}
