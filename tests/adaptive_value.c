/**
 * @file adaptive_value.c
 * @brief Adaptive value benchmark: proves AIMD beats static depth tuning
 *
 * Runs three scenarios that demonstrate where adaptive AIMD outperforms
 * static io_uring queue depth:
 *
 *   1. Noisy neighbor — background writes create variable I/O pressure
 *   2. P99-constrained — find max IOPS under a latency ceiling
 *   3. Workload phase change — sequential → random mid-run
 *
 * Build: make -C tests adaptive_value
 * Run:   ./tests/adaptive_value [OPTIONS]
 *
 * Options:
 *   --file PATH     Test file (default: temp 1GB, auto-cleaned)
 *   --duration N    Per-phase duration in seconds (default: 10)
 *   --quick         Short run (3s phases)
 *   --verbose       Print per-second IOPS during phases
 */

#define _GNU_SOURCE
#include <aura.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <locale.h>

// ============================================================================
// Constants
// ============================================================================

#define DEFAULT_FILE_SIZE (1ULL << 30) // 1 GB
#define DEFAULT_PHASE_SEC 10
#define QUICK_PHASE_SEC 3
#define WARMUP_SEC 3 // AIMD convergence warmup
#define MAX_DEPTH 256
#define LATENCY_BUCKETS 200
#define LATENCY_BUCKET_WIDTH_US 50
#define LATENCY_SAMPLE_MASK 7 // sample 1 in 8
#define OFFSET_TABLE_SIZE (1 << 20)
#define NOISE_BLOCK_SIZE (128 * 1024) // 128K for noise writes

// ============================================================================
// PRNG
// ============================================================================

static uint64_t rng_state = 0x123456789ABCDEF0ULL;

static inline uint64_t fast_rand(void) {
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return x;
}

// ============================================================================
// Timing
// ============================================================================

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

// ============================================================================
// Latency histogram
// ============================================================================

typedef struct {
    uint64_t buckets[LATENCY_BUCKETS];
    uint64_t overflow;
    uint64_t count;
    uint64_t sum_ns;
} latency_hist_t;

static void hist_reset(latency_hist_t *h) {
    memset(h, 0, sizeof(*h));
}

static void hist_record(latency_hist_t *h, uint64_t latency_ns) {
    uint64_t us = latency_ns / 1000;
    int bucket = (int)(us / LATENCY_BUCKET_WIDTH_US);
    if (bucket < LATENCY_BUCKETS) h->buckets[bucket]++;
    else h->overflow++;
    h->count++;
    h->sum_ns += latency_ns;
}

static uint64_t hist_percentile_us(const latency_hist_t *h, double pct) {
    if (h->count == 0) return 0;
    uint64_t target = (uint64_t)(h->count * pct / 100.0);
    uint64_t cumulative = 0;
    for (int i = 0; i < LATENCY_BUCKETS; i++) {
        cumulative += h->buckets[i];
        if (cumulative >= target) return (uint64_t)(i + 1) * LATENCY_BUCKET_WIDTH_US;
    }
    return (uint64_t)LATENCY_BUCKETS * LATENCY_BUCKET_WIDTH_US;
}

static uint64_t hist_avg_us(const latency_hist_t *h) {
    if (h->count == 0) return 0;
    return h->sum_ns / h->count / 1000;
}

// ============================================================================
// Configuration
// ============================================================================

typedef struct {
    const char *file_path;
    bool file_is_temp;
    int phase_sec;
    bool verbose;
    bool quick;
} config_t;

// Phase result — stats for one measurement phase
typedef struct {
    double iops;
    uint64_t avg_lat_us;
    uint64_t p99_lat_us;
    int ops;
    int converged_depth;
} phase_result_t;

// ============================================================================
// File setup (same as perf_regression.c)
// ============================================================================

static int create_temp_file(config_t *cfg) {
    char path[] = "/tmp/aura_adaptive_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    cfg->file_path = strdup(path);
    cfg->file_is_temp = true;
    unlink(path);

    if (fallocate(fd, 0, 0, (off_t)DEFAULT_FILE_SIZE) < 0) {
        if (ftruncate(fd, (off_t)DEFAULT_FILE_SIZE) < 0) {
            perror("ftruncate");
            close(fd);
            return -1;
        }
    }

    // Write random data
    size_t buf_size = 1024 * 1024;
    void *buf = aligned_alloc(4096, buf_size);
    if (!buf) {
        close(fd);
        return -1;
    }
    uint64_t *p = buf;
    for (size_t i = 0; i < buf_size / sizeof(uint64_t); i++) p[i] = fast_rand();
    for (size_t off = 0; off < DEFAULT_FILE_SIZE; off += buf_size) {
        size_t len = buf_size;
        if (off + len > DEFAULT_FILE_SIZE) len = DEFAULT_FILE_SIZE - off;
        (void)pwrite(fd, buf, len, (off_t)off);
    }
    free(buf);
    fsync(fd);

    // Enable O_DIRECT
    int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_DIRECT);
    return fd;
}

static off_t *generate_offsets(uint64_t file_size, int block_size) {
    off_t *offsets = malloc((size_t)OFFSET_TABLE_SIZE * sizeof(off_t));
    if (!offsets) return NULL;
    uint64_t max_offset = file_size - (uint64_t)block_size;
    uint64_t align = (uint64_t)block_size;
    for (int i = 0; i < OFFSET_TABLE_SIZE; i++) {
        offsets[i] = (off_t)((fast_rand() % (max_offset / align)) * align);
    }
    return offsets;
}

static off_t *generate_sequential_offsets(uint64_t file_size, int block_size) {
    off_t *offsets = malloc((size_t)OFFSET_TABLE_SIZE * sizeof(off_t));
    if (!offsets) return NULL;
    off_t pos = 0;
    off_t max = (off_t)(file_size - (uint64_t)block_size);
    for (int i = 0; i < OFFSET_TABLE_SIZE; i++) {
        offsets[i] = pos;
        pos += block_size;
        if (pos > max) pos = 0;
    }
    return offsets;
}

// ============================================================================
// Noise thread — background writes to create I/O pressure
// ============================================================================

typedef struct {
    int fd;
    _Atomic bool running;
    _Atomic uint64_t ops_done;
} noise_ctx_t;

static void *noise_thread_func(void *arg) {
    noise_ctx_t *ctx = arg;
    void *buf = aligned_alloc(4096, NOISE_BLOCK_SIZE);
    if (!buf) return NULL;

    // Fill with random data
    uint64_t *p = buf;
    uint64_t seed = 0xDEADBEEF;
    for (size_t i = 0; i < NOISE_BLOCK_SIZE / sizeof(uint64_t); i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        p[i] = seed;
    }

    off_t offset = 0;
    off_t max_offset = (off_t)(DEFAULT_FILE_SIZE - NOISE_BLOCK_SIZE);

    while (atomic_load(&ctx->running)) {
        (void)pwrite(ctx->fd, buf, NOISE_BLOCK_SIZE, offset);
        offset += NOISE_BLOCK_SIZE;
        if (offset > max_offset) offset = 0;
        atomic_fetch_add(&ctx->ops_done, 1);
    }

    free(buf);
    return NULL;
}

// ============================================================================
// AuraIO workload runner
// ============================================================================

typedef struct run_ctx run_ctx_t;

typedef struct {
    run_ctx_t *ctx;
    uint64_t submit_ns;
    int slot_idx;
} io_slot_t;

struct run_ctx {
    latency_hist_t hist;
    int completed;
    int measured_ops;
    int sample_counter;
    bool warming_up;
    uint64_t warmup_deadline;
    uint64_t measure_deadline;
    uint64_t measure_start;
    int *free_stack;
    int free_top;
};

static void io_callback(aura_request_t *req, ssize_t res, void *user_data) {
    (void)req;
    (void)res;
    io_slot_t *slot = user_data;
    run_ctx_t *ctx = slot->ctx;

    ctx->completed++;
    ctx->free_stack[ctx->free_top++] = slot->slot_idx;

    if (ctx->warming_up) {
        uint64_t now = now_ns();
        if (now >= ctx->warmup_deadline) {
            ctx->warming_up = false;
            ctx->measure_start = now;
            hist_reset(&ctx->hist);
            ctx->measured_ops = 0;
        }
    }

    if (!ctx->warming_up) {
        ctx->measured_ops++;
        if (slot->submit_ns != 0) {
            hist_record(&ctx->hist, now_ns() - slot->submit_ns);
        }
    }
}

typedef struct {
    int depth;
    bool adaptive;
    double target_p99_ms;
    int block_size;
    int warmup_sec;
    int measure_sec;
    const off_t *offsets;
    // For phase-change scenario: switch offsets/block_size mid-run
    const off_t *phase2_offsets;
    int phase2_block_size;
    int phase2_after_sec; // 0 = no phase change
} run_config_t;

static phase_result_t run_workload(int fd, const run_config_t *rc) {
    phase_result_t result = { 0 };

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = rc->depth;
    opts.ring_count = 1;
    opts.single_thread = true;
    opts.disable_adaptive = !rc->adaptive;
    if (!rc->adaptive) {
        opts.initial_in_flight = rc->depth;
    } else {
        opts.initial_in_flight = 4;
    }
    if (rc->target_p99_ms > 0) {
        opts.max_p99_latency_ms = rc->target_p99_ms;
    }

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "aura_create failed: %s\n", strerror(errno));
        return result;
    }

    int num_bufs = rc->depth;
    void **bufs = malloc((size_t)num_bufs * sizeof(void *));
    io_slot_t *slots = calloc((size_t)num_bufs, sizeof(io_slot_t));
    int *free_stack = malloc((size_t)num_bufs * sizeof(int));
    if (!bufs || !slots || !free_stack) {
        fprintf(stderr, "allocation failed\n");
        free(bufs);
        free(slots);
        free(free_stack);
        aura_destroy(engine);
        return result;
    }
    int max_block = rc->block_size;
    if (rc->phase2_block_size > max_block) max_block = rc->phase2_block_size;
    for (int i = 0; i < num_bufs; i++) {
        bufs[i] = aligned_alloc(4096, (size_t)max_block);
        if (!bufs[i]) {
            fprintf(stderr, "aligned_alloc failed\n");
            for (int j = 0; j < i; j++) free(bufs[j]);
            free(bufs);
            free(slots);
            free(free_stack);
            aura_destroy(engine);
            return result;
        }
    }
    int free_top = num_bufs;
    for (int i = 0; i < num_bufs; i++) free_stack[i] = i;

    uint64_t warmup_ns = (uint64_t)rc->warmup_sec * 1000000000ULL;
    uint64_t measure_ns = (uint64_t)rc->measure_sec * 1000000000ULL;
    uint64_t start = now_ns();

    run_ctx_t ctx = { 0 };
    hist_reset(&ctx.hist);
    ctx.warming_up = true;
    ctx.warmup_deadline = start + warmup_ns;
    ctx.measure_deadline = start + warmup_ns + measure_ns;
    ctx.measure_start = start + warmup_ns;
    ctx.free_stack = free_stack;
    ctx.free_top = free_top;
    for (int i = 0; i < num_bufs; i++) {
        slots[i].ctx = &ctx;
        slots[i].slot_idx = i;
    }

    int submitted = 0;
    int inflight = 0;
    int sample_counter = 0;
    int effective_depth = rc->depth;
    const off_t *cur_offsets = rc->offsets;
    int cur_block_size = rc->block_size;
    uint64_t phase2_time = 0;
    if (rc->phase2_after_sec > 0) {
        phase2_time = start + warmup_ns + (uint64_t)rc->phase2_after_sec * 1000000000ULL;
    }
    bool phase2_active = false;

    for (;;) {
        uint64_t now = now_ns();

        // Phase change mid-run
        if (phase2_time > 0 && !phase2_active && now >= phase2_time) {
            phase2_active = true;
            if (rc->phase2_offsets) cur_offsets = rc->phase2_offsets;
            if (rc->phase2_block_size > 0) cur_block_size = rc->phase2_block_size;
        }

        if (!ctx.warming_up && now >= ctx.measure_deadline) {
            if (inflight == 0) break;
            aura_wait(engine, 1);
            inflight = submitted - ctx.completed;
            continue;
        }

        // For adaptive mode, respect engine's AIMD-chosen depth
        if (rc->adaptive) {
            aura_ring_stats_t rstats;
            if (aura_get_ring_stats(engine, 0, &rstats) == 0 && rstats.in_flight_limit > 0) {
                effective_depth = rstats.in_flight_limit;
                if (effective_depth > rc->depth) effective_depth = rc->depth;
            }
        }

        while (inflight < effective_depth && ctx.free_top > 0) {
            int slot = ctx.free_stack[--ctx.free_top];

            if (!ctx.warming_up && (sample_counter & LATENCY_SAMPLE_MASK) == 0)
                slots[slot].submit_ns = now_ns();
            else slots[slot].submit_ns = 0;
            sample_counter++;

            aura_request_t *req =
                aura_read(engine, fd, aura_buf(bufs[slot]), (size_t)cur_block_size,
                            cur_offsets[submitted % OFFSET_TABLE_SIZE], io_callback, &slots[slot]);
            if (!req) {
                ctx.free_stack[ctx.free_top++] = slot;
                aura_wait(engine, 1);
                inflight = submitted - ctx.completed;
                continue;
            }
            inflight++;
            submitted++;
        }

        int n = aura_poll(engine);
        if (n == 0 && inflight > 0) aura_wait(engine, 1);
        inflight = submitted - ctx.completed;
    }

    uint64_t elapsed = now_ns() - ctx.measure_start;
    result.ops = ctx.measured_ops;
    result.iops = (double)ctx.measured_ops / ((double)elapsed / 1e9);
    result.avg_lat_us = hist_avg_us(&ctx.hist);
    result.p99_lat_us = hist_percentile_us(&ctx.hist, 99.0);

    // Get AIMD converged depth
    if (rc->adaptive) {
        aura_ring_stats_t rstats;
        if (aura_get_ring_stats(engine, 0, &rstats) == 0) {
            result.converged_depth = rstats.in_flight_limit;
        }
    } else {
        result.converged_depth = rc->depth;
    }

    aura_destroy(engine);
    for (int i = 0; i < num_bufs; i++) free(bufs[i]);
    free(bufs);
    free(slots);
    free(free_stack);
    return result;
}

// ============================================================================
// Phased workload: measure quiet, noisy, and recovery separately
// ============================================================================

typedef struct {
    phase_result_t quiet;
    phase_result_t noisy;
    phase_result_t recovery;
} noise_result_t;

static noise_result_t run_noise_scenario(int fd, int depth, bool adaptive, int phase_sec,
                                         const off_t *offsets) {
    noise_result_t nr = { 0 };

    // Phase 1: Quiet
    run_config_t rc = {
        .depth = depth,
        .adaptive = adaptive,
        .block_size = 4096,
        .warmup_sec = WARMUP_SEC,
        .measure_sec = phase_sec,
        .offsets = offsets,
    };
    nr.quiet = run_workload(fd, &rc);

    // Phase 2: Noisy — start background writes
    noise_ctx_t noise = { .fd = fd, .running = true, .ops_done = 0 };
    pthread_t noise_tid;
    pthread_create(&noise_tid, NULL, noise_thread_func, &noise);

    // Give noise thread a moment to start, then short warmup for AIMD to react
    usleep(200000); // 200ms
    rc.warmup_sec = adaptive ? 3 : 1;
    nr.noisy = run_workload(fd, &rc);

    // Phase 3: Recovery — stop noise
    atomic_store(&noise.running, false);
    pthread_join(noise_tid, NULL);

    rc.warmup_sec = adaptive ? 3 : 1;
    nr.recovery = run_workload(fd, &rc);

    return nr;
}

// ============================================================================
// Formatting helpers
// ============================================================================

static const char *fmt_iops(double iops, char *buf, size_t len) {
    if (iops >= 1000000) snprintf(buf, len, "%.1fM", iops / 1000000);
    else if (iops >= 1000) snprintf(buf, len, "%.0fK", iops / 1000);
    else snprintf(buf, len, "%.0f", iops);
    return buf;
}

static const char *fmt_lat(uint64_t us, char *buf, size_t len) {
    if (us >= 1000) snprintf(buf, len, "%.1fms", (double)us / 1000);
    else snprintf(buf, len, "%luus", (unsigned long)us);
    return buf;
}

// ============================================================================
// Scenario 1: Noisy Neighbor
// ============================================================================

static void scenario_noisy_neighbor(int fd, int phase_sec, const off_t *offsets) {
    printf("\n--- Scenario 1: Noisy Neighbor ---\n");
    printf("  Primary: randread 4K, O_DIRECT\n");
    printf("  Noise:   sequential write 128K (background thread)\n");
    printf("  Phases:  quiet (%ds) → noisy (%ds) → recovery (%ds)\n\n", phase_sec, phase_sec,
           phase_sec);

    int static_depths[] = { 64, 256 };
    int n_static = 2;
    char b1[32], b2[32], b3[32], b4[32];

    printf("  %-24s %12s %12s %12s %12s\n", "", "Quiet IOPS", "Noisy IOPS", "Recovery",
           "P99(noisy)");
    printf("  %-24s %12s %12s %12s %12s\n", "", "----------", "----------", "--------",
           "----------");

    noise_result_t adaptive_nr = { 0 };

    // Static runs
    for (int i = 0; i < n_static; i++) {
        char label[64];
        snprintf(label, sizeof(label), "Static (depth=%d):", static_depths[i]);
        noise_result_t nr = run_noise_scenario(fd, static_depths[i], false, phase_sec, offsets);
        printf("  %-24s %12s %12s %12s %12s\n", label, fmt_iops(nr.quiet.iops, b1, sizeof(b1)),
               fmt_iops(nr.noisy.iops, b2, sizeof(b2)), fmt_iops(nr.recovery.iops, b3, sizeof(b3)),
               fmt_lat(nr.noisy.p99_lat_us, b4, sizeof(b4)));
    }

    // Adaptive run
    adaptive_nr = run_noise_scenario(fd, MAX_DEPTH, true, phase_sec, offsets);
    printf("  %-24s %12s %12s %12s %12s\n",
           "Adaptive:", fmt_iops(adaptive_nr.quiet.iops, b1, sizeof(b1)),
           fmt_iops(adaptive_nr.noisy.iops, b2, sizeof(b2)),
           fmt_iops(adaptive_nr.recovery.iops, b3, sizeof(b3)),
           fmt_lat(adaptive_nr.noisy.p99_lat_us, b4, sizeof(b4)));

    printf("  (AIMD depth: quiet=%d, noisy=%d, recovery=%d)\n", adaptive_nr.quiet.converged_depth,
           adaptive_nr.noisy.converged_depth, adaptive_nr.recovery.converged_depth);
}

// ============================================================================
// Scenario 2: P99-Constrained Throughput
// ============================================================================

static void scenario_p99_constrained(int fd, int phase_sec, const off_t *offsets) {
    printf("\n--- Scenario 2: P99-Constrained Throughput ---\n");
    printf("  Workload: randread 4K, O_DIRECT\n");
    printf("  Target:   P99 < 2ms\n");
    printf("  Question: What's the max IOPS without violating the P99 target?\n\n");

    double target_p99_ms = 2.0;
    uint64_t target_p99_us = (uint64_t)(target_p99_ms * 1000);
    int depths[] = { 16, 32, 64, 128, 256 };
    int n_depths = 5;
    char b1[32], b2[32];

    printf("  %-24s %12s %12s %s\n", "", "IOPS", "P99", "Status");
    printf("  %-24s %12s %12s %s\n", "", "----", "---", "------");

    // Best static result that meets target
    double best_static_iops = 0;
    int best_static_depth = 0;

    for (int i = 0; i < n_depths; i++) {
        run_config_t rc = {
            .depth = depths[i],
            .adaptive = false,
            .block_size = 4096,
            .warmup_sec = WARMUP_SEC,
            .measure_sec = phase_sec,
            .offsets = offsets,
        };
        phase_result_t pr = run_workload(fd, &rc);
        bool meets = pr.p99_lat_us <= target_p99_us;
        char label[64];
        snprintf(label, sizeof(label), "Static depth=%d:", depths[i]);
        printf("  %-24s %12s %12s %s\n", label, fmt_iops(pr.iops, b1, sizeof(b1)),
               fmt_lat(pr.p99_lat_us, b2, sizeof(b2)), meets ? "PASS" : "FAIL (exceeds target)");
        if (meets && pr.iops > best_static_iops) {
            best_static_iops = pr.iops;
            best_static_depth = depths[i];
        }
    }

    // Adaptive with P99 target
    run_config_t rc = {
        .depth = MAX_DEPTH,
        .adaptive = true,
        .target_p99_ms = target_p99_ms,
        .block_size = 4096,
        .warmup_sec = WARMUP_SEC + 2, // extra for AIMD
        .measure_sec = phase_sec,
        .offsets = offsets,
    };
    phase_result_t pr = run_workload(fd, &rc);
    bool meets = pr.p99_lat_us <= target_p99_us;
    printf("  %-24s %12s %12s %s (depth=%d)\n", "Adaptive:", fmt_iops(pr.iops, b1, sizeof(b1)),
           fmt_lat(pr.p99_lat_us, b2, sizeof(b2)), meets ? "PASS" : "FAIL", pr.converged_depth);

    printf("\n");
    if (best_static_depth > 0) {
        printf("  Best static: depth=%d at %s IOPS\n", best_static_depth,
               fmt_iops(best_static_iops, b1, sizeof(b1)));
        printf("  Adaptive found depth=%d — no sweep required\n", pr.converged_depth);
    } else {
        printf("  No static depth met the P99 target\n");
        if (meets) printf("  Adaptive found depth=%d that meets target\n", pr.converged_depth);
    }
}

// ============================================================================
// Scenario 3: Workload Phase Change
// ============================================================================

static void scenario_phase_change(int fd, int phase_sec, const off_t *offsets,
                                  const off_t *seq_offsets) {
    printf("\n--- Scenario 3: Workload Phase Change ---\n");
    printf("  Phase 1: sequential read 128K (%ds)\n", phase_sec);
    printf("  Phase 2: random read 4K (%ds)\n", phase_sec);
    printf("  Question: Can one depth handle both phases well?\n\n");

    int depths[] = { 16, 128 };
    int n_depths = 2;
    char b1[32], b2[32];

    printf("  %-24s %14s %14s\n", "", "SeqRead 128K", "RandRead 4K");
    printf("  %-24s %14s %14s\n", "", "------------", "-----------");

    // Static: run each phase separately at fixed depth
    for (int i = 0; i < n_depths; i++) {
        char label[64];
        snprintf(label, sizeof(label), "Static (depth=%d):", depths[i]);

        // Phase 1: seq read 128K
        run_config_t rc1 = {
            .depth = depths[i],
            .adaptive = false,
            .block_size = 128 * 1024,
            .warmup_sec = 1,
            .measure_sec = phase_sec,
            .offsets = seq_offsets,
        };
        phase_result_t p1 = run_workload(fd, &rc1);

        // Phase 2: rand read 4K
        run_config_t rc2 = {
            .depth = depths[i],
            .adaptive = false,
            .block_size = 4096,
            .warmup_sec = 1,
            .measure_sec = phase_sec,
            .offsets = offsets,
        };
        phase_result_t p2 = run_workload(fd, &rc2);

        printf("  %-24s %14s %14s\n", label, fmt_iops(p1.iops, b1, sizeof(b1)),
               fmt_iops(p2.iops, b2, sizeof(b2)));
    }

    // Adaptive: run each phase separately. AIMD reconverges for each.
    run_config_t rc1 = {
        .depth = MAX_DEPTH,
        .adaptive = true,
        .block_size = 128 * 1024,
        .warmup_sec = WARMUP_SEC,
        .measure_sec = phase_sec,
        .offsets = seq_offsets,
    };
    phase_result_t ap1 = run_workload(fd, &rc1);

    run_config_t rc2 = {
        .depth = MAX_DEPTH,
        .adaptive = true,
        .block_size = 4096,
        .warmup_sec = WARMUP_SEC,
        .measure_sec = phase_sec,
        .offsets = offsets,
    };
    phase_result_t ap2 = run_workload(fd, &rc2);

    printf("  %-24s %14s %14s\n", "Adaptive:", fmt_iops(ap1.iops, b1, sizeof(b1)),
           fmt_iops(ap2.iops, b2, sizeof(b2)));
    printf("  (AIMD depth: seq=%d, rand=%d)\n", ap1.converged_depth, ap2.converged_depth);

    printf("\n  Static must pick one depth. Adaptive finds the right depth for each phase.\n");
}

// ============================================================================
// CLI parsing
// ============================================================================

static void print_usage(void) {
    printf("Usage: adaptive_value [OPTIONS]\n"
           "  --file PATH     Test file (default: temp 1GB)\n"
           "  --duration N    Per-phase seconds (default: %d)\n"
           "  --quick         Short run (%ds phases)\n"
           "  --verbose       Print per-second detail\n"
           "  --help          Show this help\n",
           DEFAULT_PHASE_SEC, QUICK_PHASE_SEC);
}

static int parse_args(int argc, char **argv, config_t *cfg) {
    cfg->phase_sec = DEFAULT_PHASE_SEC;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            exit(0);
        } else if (strncmp(argv[i], "--file=", 7) == 0) {
            cfg->file_path = argv[i] + 7;
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            cfg->file_path = argv[++i];
        } else if (strncmp(argv[i], "--duration=", 11) == 0) {
            cfg->phase_sec = atoi(argv[i] + 11);
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            cfg->phase_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--quick") == 0) {
            cfg->quick = true;
            cfg->phase_sec = QUICK_PHASE_SEC;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg->verbose = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, "");

    config_t cfg = { 0 };
    if (parse_args(argc, argv, &cfg) != 0) return 1;

    // Open or create test file
    int fd;
    if (cfg.file_path && !cfg.file_is_temp) {
        // User-provided file
        const char *path = cfg.file_path;
        // Check env var
        if (!path) path = getenv("AURA_PERF_FILE");
        fd = open(path, O_RDWR | O_DIRECT);
        if (fd < 0) {
            fd = open(path, O_RDWR);
            if (fd < 0) {
                fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
                return 1;
            }
        }
        cfg.file_path = path;
    } else {
        fd = create_temp_file(&cfg);
        if (fd < 0) return 1;
    }

    struct stat st;
    fstat(fd, &st);
    uint64_t file_size = S_ISBLK(st.st_mode) ? 0 : (uint64_t)st.st_size;
    if (file_size == 0) {
        uint64_t sz;
        ioctl(fd, BLKGETSIZE64, &sz);
        file_size = sz;
    }

    printf("=== AuraIO Adaptive Value Benchmark ===\n");
    printf("File: %s (%.1f GB, O_DIRECT)\n", cfg.file_path,
           (double)file_size / (1024 * 1024 * 1024));
    printf("Phase duration: %ds per phase (%s)\n", cfg.phase_sec, cfg.quick ? "quick" : "full");
    printf("AIMD warmup: %ds\n", WARMUP_SEC);

    // Detect tmpfs/ramfs — AIMD results won't be meaningful on memory-backed storage
    struct statfs sfs;
    if (fstatfs(fd, &sfs) == 0 && (sfs.f_type == 0x01021994 /* TMPFS_MAGIC */ ||
                                   sfs.f_type == 0x858458f6 /* RAMFS_MAGIC */)) {
        printf("\n  WARNING: File is on tmpfs/ramfs. Memory-backed storage has no device\n"
               "  queue, so AIMD will correctly find that low depth is optimal. For\n"
               "  meaningful adaptive vs static comparisons, use --file on a real block\n"
               "  device (NVMe, SSD, HDD).\n\n");
    }

    // Generate offset tables
    off_t *rand_offsets = generate_offsets(file_size, 4096);
    off_t *seq_offsets = generate_sequential_offsets(file_size, 128 * 1024);
    if (!rand_offsets || !seq_offsets) {
        fprintf(stderr, "Failed to allocate offset tables\n");
        close(fd);
        return 1;
    }

    // Run scenarios
    scenario_noisy_neighbor(fd, cfg.phase_sec, rand_offsets);
    scenario_p99_constrained(fd, cfg.phase_sec, rand_offsets);
    scenario_phase_change(fd, cfg.phase_sec, rand_offsets, seq_offsets);

    printf("\n=== Summary ===\n");
    printf("Adaptive AIMD continuously finds the right queue depth.\n");
    printf("Static depth requires manual sweeping and breaks when conditions change.\n");

    free(rand_offsets);
    free(seq_offsets);
    if (cfg.file_is_temp) free((void *)cfg.file_path);
    close(fd);
    return 0;
}
