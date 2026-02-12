/**
 * @file perf_regression.c
 * @brief Performance regression test: raw io_uring vs AuraIO overhead
 *
 * Measures AuraIO's per-operation overhead by comparing against a minimal
 * raw io_uring implementation. Sweeps queue depths to find each optimal
 * configuration, then reports overhead as a percentage.
 *
 * Each test phase runs for a configurable duration (default 5s) to ensure
 * stable measurements regardless of device speed.
 *
 * Build: make -C tests perf_regression
 * Run:   ./tests/perf_regression [OPTIONS]
 *
 * Options:
 *   --file PATH        Test file (default: temp file, 1GB, auto-cleaned)
 *   --duration N       Measurement duration per test in seconds (default: 5)
 *   --warmup N         Warmup duration in seconds (default: 1)
 *   --block-size N     Block size in bytes (default: 4096)
 *   --depths D1,D2,..  Queue depths to sweep (default: 32,64,128,256)
 *   --batch-size N     SQEs per io_uring_submit() call (default: 8)
 *   --threshold N      Max allowed IOPS overhead % (default: 10)
 *   --verbose          Show per-depth sweep results
 *
 * Environment:
 *   AURAIO_PERF_FILE   Same as --file (CLI takes precedence)
 */

#define _GNU_SOURCE
#include <auraio.h>
#include <liburing.h>
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
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <locale.h>

// ============================================================================
// Constants
// ============================================================================

#define DEFAULT_FILE_SIZE (1ULL << 30) // 1 GB
#define DEFAULT_DURATION_SEC 5
#define DEFAULT_WARMUP_SEC 1
#define DEFAULT_ADAPTIVE_WARMUP_SEC 2
#define DEFAULT_BLOCK_SIZE 4096
#define DEFAULT_BATCH_SIZE 8
#define DEFAULT_THRESHOLD 10
#define MAX_DEPTHS 16
#define LATENCY_BUCKETS 200
#define LATENCY_BUCKET_WIDTH_US 50
#define LATENCY_SAMPLE_MASK 7 // sample 1 in 8
#define OFFSET_TABLE_SIZE (1 << 20) // 1M entries, wraps around

// ============================================================================
// PRNG (xorshift64, deterministic)
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
// Configuration and results
// ============================================================================

typedef struct {
    const char *file_path;
    bool file_is_temp;
    int duration_sec;
    int warmup_sec;
    int block_size;
    int depths[MAX_DEPTHS];
    int num_depths;
    int batch_size;
    int threshold_pct;
    bool verbose;
} perf_config_t;

typedef struct {
    double iops;
    uint64_t avg_lat_us;
    uint64_t p99_lat_us;
    uint64_t elapsed_ns;
    int ops_completed;
    int depth_used; // for adaptive: AIMD converged depth
} perf_result_t;

// ============================================================================
// File setup
// ============================================================================

static int create_temp_file(perf_config_t *cfg) {
    char path[] = "/tmp/auraio_perf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return -1;
    }
    // Store path for display, unlink so cleanup is automatic
    cfg->file_path = strdup(path);
    cfg->file_is_temp = true;
    unlink(path);

    if (fallocate(fd, 0, 0, (off_t)DEFAULT_FILE_SIZE) < 0) {
        // fallocate may fail on some filesystems, try ftruncate
        if (ftruncate(fd, (off_t)DEFAULT_FILE_SIZE) < 0) {
            perror("ftruncate");
            close(fd);
            return -1;
        }
    }

    // Write random data so reads don't just return zeros
    size_t buf_size = 1024 * 1024;
    void *buf = aligned_alloc(4096, buf_size);
    if (!buf) {
        close(fd);
        return -1;
    }
    // Fill with deterministic random data
    uint64_t *p = buf;
    for (size_t i = 0; i < buf_size / sizeof(uint64_t); i++) p[i] = fast_rand();
    for (size_t off = 0; off < DEFAULT_FILE_SIZE; off += buf_size) {
        size_t len = buf_size;
        if (off + len > DEFAULT_FILE_SIZE) len = DEFAULT_FILE_SIZE - off;
        if (pwrite(fd, buf, len, (off_t)off) != (ssize_t)len) {
            perror("pwrite");
            free(buf);
            close(fd);
            return -1;
        }
    }
    free(buf);
    fsync(fd);
    return fd;
}

static uint64_t get_file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) return 0;
    if (S_ISBLK(st.st_mode)) {
        uint64_t size = 0;
        if (ioctl(fd, BLKGETSIZE64, &size) == 0) return size;
        return 0;
    }
    return (uint64_t)st.st_size;
}

// ============================================================================
// Random offset table (deterministic, pre-generated, wraps around)
// ============================================================================

static off_t *generate_offsets(uint64_t file_size, int block_size) {
    off_t *offsets = malloc((size_t)OFFSET_TABLE_SIZE * sizeof(off_t));
    if (!offsets) return NULL;
    uint64_t max_offset = file_size - (uint64_t)block_size;
    uint64_t align = (uint64_t)block_size;
    for (int i = 0; i < OFFSET_TABLE_SIZE; i++) {
        uint64_t r = fast_rand() % (max_offset / align);
        offsets[i] = (off_t)(r * align);
    }
    return offsets;
}

// ============================================================================
// Raw io_uring benchmark — naive version
// ============================================================================
//
// Correct but unoptimized io_uring loop:
// - No COOP_TASKRUN/SINGLE_ISSUER (pays for async interrupts)
// - Separate submit() + wait_cqe() (two syscalls per loop)
// - Clock check every iteration (adds overhead on hot path)
// Kept as a reference to show the impact of io_uring best practices.

static perf_result_t run_raw_uring_naive(int fd, const perf_config_t *cfg, int depth,
                                         const off_t *offsets, int warmup_sec) {
    perf_result_t result = { 0 };
    struct io_uring ring;
    int ret = io_uring_queue_init(depth, &ring, 0);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init: %s\n", strerror(-ret));
        return result;
    }

    int num_bufs = depth;
    void **bufs = malloc((size_t)num_bufs * sizeof(void *));
    uint64_t *slot_submit_ns = calloc((size_t)num_bufs, sizeof(uint64_t));
    for (int i = 0; i < num_bufs; i++) bufs[i] = aligned_alloc(4096, (size_t)cfg->block_size);

    // Free-slot stack to prevent buffer aliasing on OOO completions
    int *free_stack = malloc((size_t)num_bufs * sizeof(int));
    int free_top = num_bufs;
    for (int i = 0; i < num_bufs; i++) free_stack[i] = i;

    latency_hist_t hist;
    hist_reset(&hist);

    uint64_t warmup_ns = (uint64_t)warmup_sec * 1000000000ULL;
    uint64_t measure_ns = (uint64_t)cfg->duration_sec * 1000000000ULL;

    uint64_t submitted = 0;
    int inflight = 0;
    int sample_counter = 0;
    uint64_t measured_ops = 0;
    uint64_t phase_start = now_ns();
    bool warming_up = true;
    uint64_t measure_start = 0;
    int batch = cfg->batch_size;

    for (;;) {
        uint64_t now = now_ns();

        if (warming_up) {
            if (now - phase_start >= warmup_ns) {
                warming_up = false;
                measure_start = now;
                hist_reset(&hist);
                measured_ops = 0;
            }
        } else {
            if (now - measure_start >= measure_ns && inflight == 0) break;
        }

        bool past_deadline = !warming_up && (now - measure_start >= measure_ns);
        int queued = 0;
        while (!past_deadline && queued < batch && free_top > 0) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;
            int slot = free_stack[--free_top];
            io_uring_prep_read(sqe, fd, bufs[slot], (unsigned)cfg->block_size,
                               offsets[submitted % OFFSET_TABLE_SIZE]);
            io_uring_sqe_set_data(sqe, (void *)(uintptr_t)slot);

            if (!warming_up && (sample_counter & LATENCY_SAMPLE_MASK) == 0)
                slot_submit_ns[slot] = now_ns();
            else slot_submit_ns[slot] = 0;
            sample_counter++;

            inflight++;
            submitted++;
            queued++;
        }

        if (queued > 0) {
            ret = io_uring_submit(&ring);
            if (ret < 0 && ret != -EBUSY) break;
        }

        if (inflight == 0) continue;
        struct io_uring_cqe *cqe;
        ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0) break;

        do {
            int slot = (int)(uintptr_t)io_uring_cqe_get_data(cqe);
            if (slot_submit_ns[slot] != 0) {
                uint64_t lat = now_ns() - slot_submit_ns[slot];
                hist_record(&hist, lat);
            }
            io_uring_cqe_seen(&ring, cqe);
            free_stack[free_top++] = slot;
            inflight--;
            if (!warming_up) measured_ops++;
        } while (io_uring_peek_cqe(&ring, &cqe) == 0);
    }

    uint64_t elapsed = now_ns() - measure_start;
    result.ops_completed = (int)measured_ops;
    result.elapsed_ns = elapsed;
    result.iops = (double)measured_ops / ((double)elapsed / 1e9);
    result.avg_lat_us = hist_avg_us(&hist);
    result.p99_lat_us = hist_percentile_us(&hist, 99.0);
    result.depth_used = depth;

    for (int i = 0; i < num_bufs; i++) free(bufs[i]);
    free(bufs);
    free(slot_submit_ns);
    free(free_stack);
    io_uring_queue_exit(&ring);
    return result;
}

// ============================================================================
// Raw io_uring benchmark — optimized (time-bound)
// ============================================================================
//
// Best-practice io_uring loop representing the theoretical floor:
// - COOP_TASKRUN + SINGLE_ISSUER for minimal kernel overhead
// - submit_and_wait() merges submit+reap into one syscall
// - Free-slot stack prevents buffer aliasing on OOO completions
// - Deadline checked every 1024 completions to reduce clock overhead

// Per-slot context for latency tracking
typedef struct {
    uint64_t submit_ns; // 0 if not sampling this op
    bool inflight; // true while kernel owns this slot's buffer
} raw_slot_t;

// Check deadline every N completions to avoid clock overhead on hot path
#define DEADLINE_CHECK_INTERVAL 1024

static perf_result_t run_raw_uring(int fd, const perf_config_t *cfg, int depth,
                                   const off_t *offsets, int warmup_sec) {
    perf_result_t result = { 0 };

    // Use COOP_TASKRUN + SINGLE_ISSUER for optimal single-threaded perf.
    // COOP_TASKRUN: completions delivered cooperatively (no async interrupts).
    // SINGLE_ISSUER: kernel skips cross-thread synchronization.
    struct io_uring_params params = { 0 };
#ifdef IORING_SETUP_COOP_TASKRUN
    params.flags |= IORING_SETUP_COOP_TASKRUN;
#endif
#ifdef IORING_SETUP_SINGLE_ISSUER
    params.flags |= IORING_SETUP_SINGLE_ISSUER;
#endif

    struct io_uring ring;
    int ret = io_uring_queue_init_params(depth, &ring, &params);
    if (ret < 0) {
        fprintf(stderr, "io_uring_queue_init_params: %s\n", strerror(-ret));
        return result;
    }

    // Allocate buffers and slot contexts
    int num_bufs = depth;
    void **bufs = malloc((size_t)num_bufs * sizeof(void *));
    raw_slot_t *slots = calloc((size_t)num_bufs, sizeof(raw_slot_t));
    for (int i = 0; i < num_bufs; i++) bufs[i] = aligned_alloc(4096, (size_t)cfg->block_size);

    // Free-slot stack: tracks which buffer slots are available.
    // Prevents buffer aliasing when completions arrive out of order.
    int *free_stack = malloc((size_t)num_bufs * sizeof(int));
    int free_top = num_bufs;
    for (int i = 0; i < num_bufs; i++) free_stack[i] = i;

    latency_hist_t hist;
    hist_reset(&hist);

    uint64_t warmup_ns = (uint64_t)warmup_sec * 1000000000ULL;
    uint64_t measure_ns = (uint64_t)cfg->duration_sec * 1000000000ULL;

    uint64_t submitted = 0;
    uint64_t completed = 0;
    int inflight = 0;
    int sample_counter = 0;
    uint64_t measured_ops = 0;
    uint64_t phase_start = now_ns();
    bool warming_up = true;
    bool past_deadline = false;
    uint64_t measure_start = 0;
    int completions_since_check = 0;

    int batch = cfg->batch_size;

    for (;;) {
        // Check deadline periodically (not every iteration) to reduce clock overhead
        if (completions_since_check >= DEADLINE_CHECK_INTERVAL || inflight == 0) {
            uint64_t now = now_ns();
            completions_since_check = 0;

            if (warming_up) {
                if (now - phase_start >= warmup_ns) {
                    warming_up = false;
                    measure_start = now;
                    hist_reset(&hist);
                    measured_ops = 0;
                }
            } else if (!past_deadline) {
                if (now - measure_start >= measure_ns) past_deadline = true;
            }

            if (past_deadline && inflight == 0) break;
        }

        // Submit in fixed-size batches (don't submit new work past deadline)
        int queued = 0;
        while (!past_deadline && queued < batch && free_top > 0) {
            struct io_uring_sqe *sqe = io_uring_get_sqe(&ring);
            if (!sqe) break;
            int slot = free_stack[--free_top];
            slots[slot].inflight = true;
            io_uring_prep_read(sqe, fd, bufs[slot], (unsigned)cfg->block_size,
                               offsets[submitted % OFFSET_TABLE_SIZE]);
            io_uring_sqe_set_data(sqe, (void *)(uintptr_t)slot);

            // Latency sampling (1 in 8, measurement phase only)
            // Note: stamp is set here but the actual kernel submission happens
            // after the batch loop. This is intentional — we stamp per-SQE to
            // avoid measuring batch-fill time in latency.
            slots[slot].submit_ns = 0;
            if (!warming_up && (sample_counter & LATENCY_SAMPLE_MASK) == 0)
                slots[slot].submit_ns = now_ns();
            sample_counter++;

            inflight++;
            submitted++;
            queued++;
        }

        // Merged submit+wait: single io_uring_enter syscall for both submission
        // and completion reaping, matching AuraIO's merged flush+poll pattern.
        if (queued > 0) {
            ret = io_uring_submit_and_wait(&ring, 1);
            if (ret < 0 && ret != -EBUSY) {
                fprintf(stderr, "io_uring_submit_and_wait: %s\n", strerror(-ret));
                break;
            }
        } else if (inflight > 0) {
            // Nothing to submit but need to wait for inflight completions
            struct io_uring_cqe *cqe;
            ret = io_uring_wait_cqe(&ring, &cqe);
            if (ret < 0) break;
        } else {
            continue;
        }

        // Peek and reap all available completions
        struct io_uring_cqe *cqe;
        while (io_uring_peek_cqe(&ring, &cqe) == 0) {
            int slot = (int)(uintptr_t)io_uring_cqe_get_data(cqe);

            // Record latency if sampled (stamp was taken just before submit)
            if (slots[slot].submit_ns != 0) {
                uint64_t lat = now_ns() - slots[slot].submit_ns;
                hist_record(&hist, lat);
            }

            io_uring_cqe_seen(&ring, cqe);
            slots[slot].inflight = false;
            free_stack[free_top++] = slot;
            inflight--;
            completed++;
            completions_since_check++;
            if (!warming_up) measured_ops++;
        }
    }

    uint64_t elapsed = now_ns() - measure_start;

    result.ops_completed = (int)measured_ops;
    result.elapsed_ns = elapsed;
    result.iops = (double)measured_ops / ((double)elapsed / 1e9);
    result.avg_lat_us = hist_avg_us(&hist);
    result.p99_lat_us = hist_percentile_us(&hist, 99.0);
    result.depth_used = depth;

    for (int i = 0; i < num_bufs; i++) free(bufs[i]);
    free(bufs);
    free(slots);
    free(free_stack);
    io_uring_queue_exit(&ring);

    return result;
}

// ============================================================================
// AuraIO benchmark (time-bound)
// ============================================================================

typedef struct aura_ctx aura_ctx_t;

typedef struct {
    aura_ctx_t *ctx;
    uint64_t submit_ns;
    int slot_idx;
} aura_slot_t;

struct aura_ctx {
    latency_hist_t hist;
    int completed;
    int measured_ops;
    int sample_counter;
    bool warming_up;
    uint64_t warmup_deadline;
    uint64_t measure_deadline;
    uint64_t measure_start_actual; // set when warmup->measure transition happens
    // Free-slot stack to prevent buffer aliasing on OOO completions
    int *free_stack;
    int free_top;
};

static void aura_callback(auraio_request_t *req, ssize_t res, void *user_data) {
    (void)req;
    (void)res;
    aura_slot_t *slot = user_data;
    aura_ctx_t *ctx = slot->ctx;

    ctx->completed++;

    // Return slot to free stack
    ctx->free_stack[ctx->free_top++] = slot->slot_idx;

    // Phase transition: warmup -> measure
    if (ctx->warming_up) {
        uint64_t now = now_ns();
        if (now >= ctx->warmup_deadline) {
            ctx->warming_up = false;
            ctx->measure_start_actual = now;
            hist_reset(&ctx->hist);
            ctx->measured_ops = 0;
        }
    }

    if (!ctx->warming_up) {
        ctx->measured_ops++;
        if (slot->submit_ns != 0) {
            uint64_t lat = now_ns() - slot->submit_ns;
            hist_record(&ctx->hist, lat);
        }
    }
}

static perf_result_t run_auraio(int fd, const perf_config_t *cfg, int depth, bool adaptive,
                                const off_t *offsets, int warmup_sec) {
    perf_result_t result = { 0 };

    auraio_options_t opts;
    auraio_options_init(&opts);
    opts.queue_depth = depth;
    opts.ring_count = 1;
    opts.single_thread = true;
    opts.disable_adaptive = !adaptive;
    if (!adaptive) opts.initial_in_flight = depth;

    auraio_engine_t *engine = auraio_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "auraio_create_with_options failed: %s\n", strerror(errno));
        return result;
    }

    int num_bufs = depth;
    void **bufs = malloc((size_t)num_bufs * sizeof(void *));
    aura_slot_t *slots = calloc((size_t)num_bufs, sizeof(aura_slot_t));
    for (int i = 0; i < num_bufs; i++) bufs[i] = aligned_alloc(4096, (size_t)cfg->block_size);

    // Free-slot stack to prevent buffer aliasing on OOO completions
    int *free_stack = malloc((size_t)num_bufs * sizeof(int));
    int free_top = num_bufs;
    for (int i = 0; i < num_bufs; i++) free_stack[i] = i;

    uint64_t warmup_ns = (uint64_t)warmup_sec * 1000000000ULL;
    uint64_t measure_ns = (uint64_t)cfg->duration_sec * 1000000000ULL;
    uint64_t start = now_ns();

    aura_ctx_t ctx = { 0 };
    hist_reset(&ctx.hist);
    ctx.warming_up = true;
    ctx.warmup_deadline = start + warmup_ns;
    ctx.measure_deadline = start + warmup_ns + measure_ns;
    ctx.measure_start_actual = start + warmup_ns; // fallback if no completions during transition
    ctx.free_stack = free_stack;
    ctx.free_top = free_top;
    for (int i = 0; i < num_bufs; i++) {
        slots[i].ctx = &ctx;
        slots[i].slot_idx = i;
    }

    int submitted = 0;
    int inflight = 0;
    int sample_counter = 0;

    for (;;) {
        uint64_t now = now_ns();

        // Check if past measure deadline — stop submitting, drain inflight
        if (!ctx.warming_up && now >= ctx.measure_deadline) {
            if (inflight == 0) break;
            // Drain remaining
            auraio_wait(engine, 1);
            inflight = submitted - ctx.completed;
            continue;
        }

        // Submit up to depth using free-slot stack
        while (inflight < depth && ctx.free_top > 0) {
            int slot = ctx.free_stack[--ctx.free_top];

            // Latency sampling (measurement phase only)
            if (!ctx.warming_up && (sample_counter & LATENCY_SAMPLE_MASK) == 0)
                slots[slot].submit_ns = now_ns();
            else slots[slot].submit_ns = 0;
            sample_counter++;

            auraio_request_t *req =
                auraio_read(engine, fd, auraio_buf(bufs[slot]), (size_t)cfg->block_size,
                            offsets[submitted % OFFSET_TABLE_SIZE], aura_callback, &slots[slot]);
            if (!req) {
                // Engine full, return slot and drain some completions
                ctx.free_stack[ctx.free_top++] = slot;
                auraio_wait(engine, 1);
                inflight = submitted - ctx.completed;
                continue;
            }
            inflight++;
            submitted++;
        }

        // Drain completions
        int n = auraio_poll(engine);
        if (n == 0 && inflight > 0) auraio_wait(engine, 1);
        inflight = submitted - ctx.completed;
    }

    uint64_t elapsed = now_ns() - ctx.measure_start_actual;

    result.ops_completed = ctx.measured_ops;
    result.elapsed_ns = elapsed;
    result.iops = (double)ctx.measured_ops / ((double)elapsed / 1e9);
    result.avg_lat_us = hist_avg_us(&ctx.hist);
    result.p99_lat_us = hist_percentile_us(&ctx.hist, 99.0);

    // Get AIMD converged depth
    if (adaptive) {
        auraio_ring_stats_t rstats;
        if (auraio_get_ring_stats(engine, 0, &rstats) == 0)
            result.depth_used = rstats.in_flight_limit;
        else result.depth_used = depth;
    } else {
        result.depth_used = depth;
    }

    auraio_destroy(engine);
    for (int i = 0; i < num_bufs; i++) free(bufs[i]);
    free(bufs);
    free(slots);
    free(free_stack);

    return result;
}

// ============================================================================
// Output formatting
// ============================================================================

static void format_iops(char *buf, size_t len, double iops) {
    if (iops >= 1e6) {
        snprintf(buf, len, "%.3fM", iops / 1e6);
    } else if (iops >= 1e3) {
        int whole = (int)iops;
        snprintf(buf, len, "%d,%03d", whole / 1000, whole % 1000);
    } else {
        snprintf(buf, len, "%.0f", iops);
    }
}

static void format_lat(char *buf, size_t len, uint64_t us) {
    if (us >= 1000) snprintf(buf, len, "%.1fms", (double)us / 1000.0);
    else snprintf(buf, len, "%luus", (unsigned long)us);
}

static void print_result(const char *label, const perf_result_t *r, bool mark_best) {
    char iops[32], avg[32], p99[32];
    format_iops(iops, sizeof(iops), r->iops);
    format_lat(avg, sizeof(avg), r->avg_lat_us);
    format_lat(p99, sizeof(p99), r->p99_lat_us);
    printf("  %-12s %14s IOPS   avg=%-8s p99=%-8s%s\n", label, iops, avg, p99,
           mark_best ? "  << best" : "");
}

static double pct_diff(double baseline, double test) {
    if (baseline == 0) return 0;
    return (test - baseline) / baseline * 100.0;
}

// ============================================================================
// CLI parsing
// ============================================================================

static void parse_depths(const char *str, perf_config_t *cfg) {
    cfg->num_depths = 0;
    char *s = strdup(str);
    char *tok = strtok(s, ",");
    while (tok && cfg->num_depths < MAX_DEPTHS) {
        cfg->depths[cfg->num_depths++] = atoi(tok);
        tok = strtok(NULL, ",");
    }
    free(s);
}

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "  --file PATH        Test file (default: temp 1GB file)\n"
            "  --duration N       Measurement duration per test, seconds (default: %d)\n"
            "  --warmup N         Warmup duration, seconds (default: %d)\n"
            "  --block-size N     Block size (default: %d)\n"
            "  --depths D1,D2,..  Queue depths (default: 32,64,128,256)\n"
            "  --batch-size N     SQEs per submit in raw baseline (default: %d)\n"
            "  --threshold N      Max IOPS overhead %% (default: %d)\n"
            "  --verbose          Show all sweep results\n"
            "\n"
            "Environment:\n"
            "  AURAIO_PERF_FILE   Same as --file (CLI takes precedence)\n",
            prog, DEFAULT_DURATION_SEC, DEFAULT_WARMUP_SEC, DEFAULT_BLOCK_SIZE, DEFAULT_BATCH_SIZE,
            DEFAULT_THRESHOLD);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    setlocale(LC_NUMERIC, ""); // enable thousands separators

    perf_config_t cfg = {
        .file_path = NULL,
        .file_is_temp = false,
        .duration_sec = DEFAULT_DURATION_SEC,
        .warmup_sec = DEFAULT_WARMUP_SEC,
        .block_size = DEFAULT_BLOCK_SIZE,
        .depths = { 32, 64, 128, 256 },
        .num_depths = 4,
        .batch_size = DEFAULT_BATCH_SIZE,
        .threshold_pct = DEFAULT_THRESHOLD,
        .verbose = false,
    };

    // Parse CLI
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            cfg.file_path = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            cfg.duration_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--warmup") == 0 && i + 1 < argc) {
            cfg.warmup_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--block-size") == 0 && i + 1 < argc) {
            cfg.block_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--depths") == 0 && i + 1 < argc) {
            parse_depths(argv[++i], &cfg);
        } else if (strcmp(argv[i], "--batch-size") == 0 && i + 1 < argc) {
            cfg.batch_size = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            cfg.threshold_pct = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            cfg.verbose = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    // File: CLI > env > temp
    if (!cfg.file_path) {
        const char *env = getenv("AURAIO_PERF_FILE");
        if (env && env[0]) cfg.file_path = env;
    }

    // Open or create test file
    int fd;
    bool using_odirect = false;
    if (cfg.file_path && !cfg.file_is_temp) {
        fd = open(cfg.file_path, O_RDONLY | O_DIRECT);
        if (fd >= 0) {
            using_odirect = true;
        } else {
            fd = open(cfg.file_path, O_RDONLY);
        }
        if (fd < 0) {
            fprintf(stderr, "Cannot open %s: %s\n", cfg.file_path, strerror(errno));
            return 1;
        }
    } else {
        fd = create_temp_file(&cfg);
        if (fd < 0) return 1;
        // Reopen with O_DIRECT
        // Can't reopen an unlinked file by path, so try F_SETFL
        int flags = fcntl(fd, F_GETFL);
        if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_DIRECT) == 0) using_odirect = true;
    }

    uint64_t file_size = get_file_size(fd);
    if (file_size < (uint64_t)cfg.block_size * 2) {
        fprintf(stderr, "File too small: %lu bytes\n", (unsigned long)file_size);
        close(fd);
        return 1;
    }

    // Generate offset table (large, wraps around during tests)
    off_t *offsets = generate_offsets(file_size, cfg.block_size);
    if (!offsets) {
        fprintf(stderr, "Failed to allocate offset table\n");
        close(fd);
        return 1;
    }

    // Estimate total time
    // naive sweep + optimized sweep + static + adaptive
    int num_tests = cfg.num_depths * 2 + 2;
    int adaptive_warmup =
        cfg.warmup_sec < DEFAULT_ADAPTIVE_WARMUP_SEC ? DEFAULT_ADAPTIVE_WARMUP_SEC : cfg.warmup_sec;
    int est_sec = cfg.num_depths * 2 * (cfg.warmup_sec + cfg.duration_sec) +
                  (cfg.warmup_sec + cfg.duration_sec) + (adaptive_warmup + cfg.duration_sec);

    // Print header
    printf("=== AuraIO Performance Regression Test ===\n");
    printf("File: %s (%.1f GB%s)\n", cfg.file_path ? cfg.file_path : "(temp)",
           (double)file_size / (1ULL << 30), using_odirect ? ", O_DIRECT" : "");
    printf("Block size: %d  Duration: %ds  Warmup: %ds  Batch: %d\n", cfg.block_size,
           cfg.duration_sec, cfg.warmup_sec, cfg.batch_size);
    printf("Tests: %d  Estimated time: ~%dm%02ds\n\n", num_tests, est_sec / 60, est_sec % 60);

    // ---- Raw io_uring naive sweep ----
    printf("--- Raw io_uring (naive) sweep ---\n");
    perf_result_t naive_best = { 0 };
    int naive_best_depth = 0;

    for (int i = 0; i < cfg.num_depths; i++) {
        int d = cfg.depths[i];
        perf_result_t r = run_raw_uring_naive(fd, &cfg, d, offsets, cfg.warmup_sec);
        bool is_best = r.iops > naive_best.iops;
        if (is_best) {
            naive_best = r;
            naive_best_depth = d;
        }
        if (cfg.verbose || is_best) {
            char label[32];
            snprintf(label, sizeof(label), "depth=%d", d);
            print_result(label, &r, is_best);
        }
    }

    if (!cfg.verbose) {
        char label[32];
        snprintf(label, sizeof(label), "depth=%d", naive_best_depth);
        printf("  Best: ");
        print_result(label, &naive_best, false);
    }
    printf("\n");

    // ---- Raw io_uring optimized sweep ----
    printf("--- Raw io_uring (optimized) sweep ---\n");
    perf_result_t raw_best = { 0 };
    int best_depth = 0;

    for (int i = 0; i < cfg.num_depths; i++) {
        int d = cfg.depths[i];
        perf_result_t r = run_raw_uring(fd, &cfg, d, offsets, cfg.warmup_sec);
        bool is_best = r.iops > raw_best.iops;
        if (is_best) {
            raw_best = r;
            best_depth = d;
        }
        if (cfg.verbose || is_best) {
            char label[32];
            snprintf(label, sizeof(label), "depth=%d", d);
            print_result(label, &r, is_best);
        }
    }

    if (!cfg.verbose) {
        char label[32];
        snprintf(label, sizeof(label), "depth=%d", best_depth);
        printf("  Best: ");
        print_result(label, &raw_best, false);
    }
    printf("\n");

    // ---- AuraIO static (matching raw-optimal depth) ----
    printf("--- AuraIO (static, 1 ring, depth=%d) ---\n", best_depth);
    perf_result_t aura_static = run_auraio(fd, &cfg, best_depth, false, offsets, cfg.warmup_sec);
    print_result("", &aura_static, false);
    printf("\n");

    // ---- AuraIO adaptive ----
    int adaptive_depth = 256;
    if (adaptive_depth < best_depth) adaptive_depth = best_depth * 2;
    printf("--- AuraIO (adaptive, 1 ring, depth=%d) ---\n", adaptive_depth);

    perf_result_t aura_adaptive =
        run_auraio(fd, &cfg, adaptive_depth, true, offsets, adaptive_warmup);
    print_result("", &aura_adaptive, false);
    printf("  (AIMD converged depth: %d)\n", aura_adaptive.depth_used);
    printf("\n");

    // ---- Overhead summary ----
    printf("=== Overhead Summary ===\n");
    printf("%-28s %14s  %10s  %10s  %s\n", "", "IOPS", "Avg Lat", "P99 Lat",
           "vs optimized raw (IOPS / Avg / P99)");

    char iops_buf[32], avg[32], p99[32];

    format_iops(iops_buf, sizeof(iops_buf), naive_best.iops);
    format_lat(avg, sizeof(avg), naive_best.avg_lat_us);
    format_lat(p99, sizeof(p99), naive_best.p99_lat_us);
    double naive_iops_pct = pct_diff(raw_best.iops, naive_best.iops);
    printf("Raw naive (d=%d):            %14s  %10s  %10s  %+.1f%%\n", naive_best_depth, iops_buf,
           avg, p99, naive_iops_pct);

    format_iops(iops_buf, sizeof(iops_buf), raw_best.iops);
    format_lat(avg, sizeof(avg), raw_best.avg_lat_us);
    format_lat(p99, sizeof(p99), raw_best.p99_lat_us);
    printf("Raw optimized (d=%d):        %14s  %10s  %10s  (baseline)\n", best_depth, iops_buf, avg,
           p99);

    double static_iops_pct = pct_diff(raw_best.iops, aura_static.iops);
    double static_avg_pct = pct_diff((double)raw_best.avg_lat_us, (double)aura_static.avg_lat_us);
    double static_p99_pct = pct_diff((double)raw_best.p99_lat_us, (double)aura_static.p99_lat_us);
    format_iops(iops_buf, sizeof(iops_buf), aura_static.iops);
    format_lat(avg, sizeof(avg), aura_static.avg_lat_us);
    format_lat(p99, sizeof(p99), aura_static.p99_lat_us);
    printf("AuraIO static:               %14s  %10s  %10s  %+.1f%% / %+.1f%% / %+.1f%%\n", iops_buf,
           avg, p99, static_iops_pct, static_avg_pct, static_p99_pct);

    double adapt_iops_pct = pct_diff(raw_best.iops, aura_adaptive.iops);
    double adapt_avg_pct = pct_diff((double)raw_best.avg_lat_us, (double)aura_adaptive.avg_lat_us);
    double adapt_p99_pct = pct_diff((double)raw_best.p99_lat_us, (double)aura_adaptive.p99_lat_us);
    format_iops(iops_buf, sizeof(iops_buf), aura_adaptive.iops);
    format_lat(avg, sizeof(avg), aura_adaptive.avg_lat_us);
    format_lat(p99, sizeof(p99), aura_adaptive.p99_lat_us);
    printf("AuraIO adaptive:             %14s  %10s  %10s  %+.1f%% / %+.1f%% / %+.1f%%\n", iops_buf,
           avg, p99, adapt_iops_pct, adapt_avg_pct, adapt_p99_pct);

    // ---- Regression check ----
    // Use worst-case (most negative) IOPS overhead vs optimized raw
    double worst_iops_pct = static_iops_pct;
    if (adapt_iops_pct < worst_iops_pct) worst_iops_pct = adapt_iops_pct;
    double overhead = -worst_iops_pct; // negate: negative IOPS diff = positive overhead

    printf("\n");
    if (overhead <= (double)cfg.threshold_pct) {
        printf("RESULT: PASS (max IOPS overhead: %.1f%%, threshold: %d%%)\n", overhead,
               cfg.threshold_pct);
    } else {
        printf("RESULT: FAIL (max IOPS overhead: %.1f%%, threshold: %d%%)\n", overhead,
               cfg.threshold_pct);
    }

    free(offsets);
    close(fd);

    return overhead > (double)cfg.threshold_pct ? 1 : 0;
}
