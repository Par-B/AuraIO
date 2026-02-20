// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file bench_histogram.c
 * @brief Microbenchmark: uniform vs tiered histogram record + P99
 *
 * Compares the current uniform histogram (200 buckets, 50µs width, 10ms max)
 * against a proposed tiered histogram (320 buckets, 10-1000µs width, 100ms max)
 * for NFS-class device support.
 *
 * Build: cd tests && make bench_histogram
 * Run:   ./bench_histogram
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* ============================================================================
 * Common
 * ============================================================================ */

#define ITERATIONS 10000000
#define WARMUP 1000000
#define P99_ITERS 100000

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* xorshift64 PRNG */
static uint64_t rng_state = 0xdeadbeefcafe1234ULL;
static inline uint64_t xorshift64(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

/* Generate latency sample with realistic distribution:
 * 70% in 0-500µs (NVMe-like), 20% in 500-5000µs, 8% in 5-20ms, 2% in 20-100ms */
static inline int64_t gen_latency_nvme(void) {
    uint64_t r = xorshift64() % 100;
    uint64_t v = xorshift64();
    if (r < 70) return (int64_t)(v % 500);
    if (r < 90) return (int64_t)(500 + v % 4500);
    if (r < 98) return (int64_t)(5000 + v % 15000);
    return (int64_t)(20000 + v % 80000);
}

/* NFS-like distribution: higher latencies */
static inline int64_t gen_latency_nfs(void) {
    uint64_t r = xorshift64() % 100;
    uint64_t v = xorshift64();
    if (r < 30) return (int64_t)(v % 1000);
    if (r < 60) return (int64_t)(1000 + v % 4000);
    if (r < 85) return (int64_t)(5000 + v % 15000);
    return (int64_t)(20000 + v % 80000);
}

/* ============================================================================
 * Uniform Histogram (current implementation)
 * ============================================================================ */

#define UNI_BUCKET_WIDTH 50
#define UNI_MAX_US 10000
#define UNI_BUCKET_COUNT (UNI_MAX_US / UNI_BUCKET_WIDTH) /* 200 */

typedef struct {
    _Atomic uint32_t buckets[UNI_BUCKET_COUNT];
    _Atomic uint32_t overflow;
    _Atomic uint64_t total_count;
} uniform_hist_t;

static inline void uni_reset(uniform_hist_t *h) {
    memset(h, 0, sizeof(*h));
}

static inline void uni_record(uniform_hist_t *h, int64_t latency_us) {
    if (latency_us < 0) latency_us = 0;
    int64_t bucket = latency_us / UNI_BUCKET_WIDTH;
    if (bucket >= UNI_BUCKET_COUNT)
        atomic_fetch_add_explicit(&h->overflow, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&h->buckets[(int)bucket], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->total_count, 1, memory_order_release);
}

static double uni_p99(uniform_hist_t *h) {
    uint64_t total = atomic_load_explicit(&h->total_count, memory_order_acquire);
    if (total == 0) return -1.0;
    uint32_t target = (total >= 100) ? (uint32_t)(total / 100) : (uint32_t)((total + 19) / 20);
    if (target == 0) target = 1;

    uint64_t count = atomic_load_explicit(&h->overflow, memory_order_relaxed);
    if (count >= target) return (double)UNI_MAX_US * 2.0 / 1000.0;

    for (int i = UNI_BUCKET_COUNT - 1; i >= 0; i--) {
        count += atomic_load_explicit(&h->buckets[i], memory_order_relaxed);
        if (count >= target) {
            return (i + 0.5) * UNI_BUCKET_WIDTH / 1000.0;
        }
    }
    return -1.0;
}

/* ============================================================================
 * Tiered Histogram (proposed)
 * ============================================================================
 *
 * Tier 0:  0–1ms    10µs width   100 buckets  [0..99]
 * Tier 1:  1–5ms    50µs width    80 buckets  [100..179]
 * Tier 2:  5–20ms  250µs width    60 buckets  [180..239]
 * Tier 3: 20–100ms 1000µs width   80 buckets  [240..319]
 */

#define TIER_BUCKET_COUNT 320
#define TIER_MAX_US 100000

typedef struct {
    int32_t start_us;
    int32_t width_us;
    int32_t base_bucket;
} tier_def_t;

static const tier_def_t tiers[] = {
    { 0, 10, 0 },
    { 1000, 50, 100 },
    { 5000, 250, 180 },
    { 20000, 1000, 240 },
};

static inline int tier_bucket(int64_t us) {
    int tier;
    if (us < 1000) tier = 0;
    else if (us < 5000) tier = 1;
    else if (us < 20000) tier = 2;
    else if (us < 100000) tier = 3;
    else return -1;
    return tiers[tier].base_bucket + (int)((us - tiers[tier].start_us) / tiers[tier].width_us);
}

static inline double tier_midpoint_us(int bucket) {
    int tier;
    if (bucket < 100) tier = 0;
    else if (bucket < 180) tier = 1;
    else if (bucket < 240) tier = 2;
    else tier = 3;
    int offset = bucket - tiers[tier].base_bucket;
    return tiers[tier].start_us + (offset + 0.5) * tiers[tier].width_us;
}

typedef struct {
    _Atomic uint32_t buckets[TIER_BUCKET_COUNT];
    _Atomic uint32_t overflow;
    _Atomic uint64_t total_count;
} tiered_hist_t;

static inline void tier_reset(tiered_hist_t *h) {
    memset(h, 0, sizeof(*h));
}

static inline void tier_record(tiered_hist_t *h, int64_t latency_us) {
    if (latency_us < 0) latency_us = 0;
    int b = tier_bucket(latency_us);
    if (b < 0) atomic_fetch_add_explicit(&h->overflow, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&h->buckets[b], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&h->total_count, 1, memory_order_release);
}

static double tier_p99(tiered_hist_t *h) {
    uint64_t total = atomic_load_explicit(&h->total_count, memory_order_acquire);
    if (total == 0) return -1.0;
    uint32_t target = (total >= 100) ? (uint32_t)(total / 100) : (uint32_t)((total + 19) / 20);
    if (target == 0) target = 1;

    uint64_t count = atomic_load_explicit(&h->overflow, memory_order_relaxed);
    if (count >= target) return (double)TIER_MAX_US * 2.0 / 1000.0;

    for (int i = TIER_BUCKET_COUNT - 1; i >= 0; i--) {
        count += atomic_load_explicit(&h->buckets[i], memory_order_relaxed);
        if (count >= target) {
            return tier_midpoint_us(i) / 1000.0;
        }
    }
    return -1.0;
}

/* ============================================================================
 * Benchmark Harness
 * ============================================================================ */

typedef struct {
    const char *name;
    double record_ns; /* ns per record call */
    double p99_ns; /* ns per P99 call */
} bench_result_t;

static void run_bench(const char *dist_name, int64_t (*gen)(void)) {
    uniform_hist_t uh;
    tiered_hist_t th;

    printf("\n  Distribution: %s\n", dist_name);
    printf("  %-20s %12s %12s %12s\n", "Histogram", "record/op", "P99/call", "P99 value");
    printf("  %-20s %12s %12s %12s\n", "---------", "---------", "--------", "---------");

    /* --- Uniform --- */
    uni_reset(&uh);
    rng_state = 0xdeadbeefcafe1234ULL;
    for (int i = 0; i < WARMUP; i++) uni_record(&uh, gen());
    uni_reset(&uh);

    rng_state = 0xdeadbeefcafe1234ULL;
    uint64_t t0 = now_ns();
    for (int i = 0; i < ITERATIONS; i++) uni_record(&uh, gen());
    uint64_t t1 = now_ns();
    double uni_rec_ns = (double)(t1 - t0) / ITERATIONS;

    /* P99 benchmark */
    t0 = now_ns();
    double uni_p99_val = 0;
    for (int i = 0; i < P99_ITERS; i++) uni_p99_val = uni_p99(&uh);
    t1 = now_ns();
    double uni_p99_ns = (double)(t1 - t0) / P99_ITERS;

    printf("  %-20s %9.1f ns %9.1f ns %9.3f ms\n", "Uniform (200)", uni_rec_ns, uni_p99_ns,
           uni_p99_val);

    /* --- Tiered --- */
    tier_reset(&th);
    rng_state = 0xdeadbeefcafe1234ULL;
    for (int i = 0; i < WARMUP; i++) tier_record(&th, gen());
    tier_reset(&th);

    rng_state = 0xdeadbeefcafe1234ULL;
    t0 = now_ns();
    for (int i = 0; i < ITERATIONS; i++) tier_record(&th, gen());
    t1 = now_ns();
    double tier_rec_ns = (double)(t1 - t0) / ITERATIONS;

    /* P99 benchmark */
    t0 = now_ns();
    double tier_p99_val = 0;
    for (int i = 0; i < P99_ITERS; i++) tier_p99_val = tier_p99(&th);
    t1 = now_ns();
    double tier_p99_ns = (double)(t1 - t0) / P99_ITERS;

    printf("  %-20s %9.1f ns %9.1f ns %9.3f ms\n", "Tiered (320)", tier_rec_ns, tier_p99_ns,
           tier_p99_val);

    /* Delta */
    double rec_delta = ((tier_rec_ns - uni_rec_ns) / uni_rec_ns) * 100.0;
    double p99_delta = ((tier_p99_ns - uni_p99_ns) / uni_p99_ns) * 100.0;
    printf("  %-20s %+8.1f %%   %+8.1f %%\n", "Delta", rec_delta, p99_delta);
}

int main(void) {
    printf("=== Histogram Microbenchmark ===\n");
    printf("  Records: %dM   P99 calls: %dK\n", ITERATIONS / 1000000, P99_ITERS / 1000);
    printf("  Uniform: %d buckets (%d bytes)\n", UNI_BUCKET_COUNT, (int)sizeof(uniform_hist_t));
    printf("  Tiered:  %d buckets (%d bytes)\n", TIER_BUCKET_COUNT, (int)sizeof(tiered_hist_t));

    run_bench("NVMe-like (70%% <500µs)", gen_latency_nvme);
    run_bench("NFS-like  (30%% <1ms)", gen_latency_nfs);

    printf("\n");
    return 0;
}
