// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors

/**
 * @file sspa.c
 * @brief sspa - Simple Storage Performance Analyzer powered by AuraIO
 *
 * Point it at a path and get a quick "how's your storage doing?" report.
 * Runs 8 workloads simulating real application I/O patterns and reports
 * bandwidth, IOPS, and latency for each.
 *
 * Usage: sspa [path] [size]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#include <inttypes.h>

#include <aura.h>

// ============================================================================
// Constants
// ============================================================================

#define MIN_TEST_SIZE (256ULL * 1024 * 1024) /* 256 MiB */
#define MAX_AUTO_SIZE (1024ULL * 1024 * 1024) /* 1 GiB */
#define AUTO_FRACTION 10 /* 10% of free space */
#define PIPELINE_DEPTH 32
#define MAX_WORKERS 64
#define TMP_FILENAME ".sspa.tmp"

static volatile sig_atomic_t g_interrupted = 0;

#define LAT_BUCKETS 4096
#define LAT_BUCKET_US 10 /* 10us per bucket = 0-40.96ms range */

// ============================================================================
// Workload definitions
// ============================================================================

typedef enum {
    PATTERN_SEQ_WRITE,
    PATTERN_SEQ_READ,
    PATTERN_RAND_RW,
    PATTERN_RAND_READ,
    PATTERN_MULTI_SEQ_WRITE,
    PATTERN_KV_STORE,
    PATTERN_TRAINING,
    PATTERN_LAKEHOUSE,
} pattern_t;

typedef struct {
    const char *name;
    const char *description;
    pattern_t pattern;
    size_t io_size;
    int read_pct;
    int threads; /* 0 = per-CPU */
} workload_t;

static const workload_t workloads[] = {
    { "Backup", "sequential write", PATTERN_SEQ_WRITE, 512 * 1024, 0, 1 },
    { "Recovery", "sequential read", PATTERN_SEQ_READ, 512 * 1024, 100, 1 },
    { "Database", "random rw mix", PATTERN_RAND_RW, 4096, 70, 0 },
    { "Cache", "random read", PATTERN_RAND_READ, 4096, 100, 0 },
    { "Logging", "multi-stream write", PATTERN_MULTI_SEQ_WRITE, 4096, 0, 4 },
    { "KV-Store", "append+rand read", PATTERN_KV_STORE, 65536, 80, 0 },
    { "Training", "seq read+shuffle", PATTERN_TRAINING, 256 * 1024, 100, 2 },
    { "Lakehouse", "scan+compact+lookup", PATTERN_LAKEHOUSE, 1024 * 1024, 60, 0 },
};

#define NUM_WORKLOADS (int)(sizeof(workloads) / sizeof(workloads[0]))

// ============================================================================
// Per-thread xorshift64 PRNG
// ============================================================================

static _Thread_local uint64_t t_rng_state;

static void rng_seed(int thread_id) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t_rng_state = (uint64_t)thread_id ^ (uint64_t)ts.tv_sec ^ (uint64_t)ts.tv_nsec;
    if (t_rng_state == 0) t_rng_state = 1;
}

static uint64_t rng_next(void) {
    uint64_t x = t_rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    t_rng_state = x;
    return x;
}

static off_t rng_offset(off_t file_size, size_t io_size) {
    if ((off_t)io_size >= file_size) return 0;
    uint64_t num_blocks = (uint64_t)(file_size / (off_t)io_size);
    return (off_t)(rng_next() % num_blocks) * (off_t)io_size;
}

// ============================================================================
// Latency histogram (per-thread, lock-free)
// ============================================================================

typedef struct {
    uint32_t buckets[LAT_BUCKETS];
    uint32_t overflow;
    uint32_t count;
} lat_hist_t;

static void lat_record(lat_hist_t *h, double us) {
    int idx = (int)(us / LAT_BUCKET_US);
    if (idx < 0) idx = 0;
    if (idx >= LAT_BUCKETS) {
        h->overflow++;
    } else {
        h->buckets[idx]++;
    }
    h->count++;
}

static double lat_percentile(const lat_hist_t *h, double pct) {
    if (h->count == 0) return 0.0;
    uint32_t target = (uint32_t)((double)h->count * pct / 100.0);
    uint32_t cumul = 0;
    for (int i = 0; i < LAT_BUCKETS; i++) {
        cumul += h->buckets[i];
        if (cumul >= target) return (double)(i + 1) * LAT_BUCKET_US;
    }
    return (double)LAT_BUCKETS * LAT_BUCKET_US;
}

static double lat_avg(const lat_hist_t *h) {
    if (h->count == 0) return 0.0;
    double sum = 0;
    for (int i = 0; i < LAT_BUCKETS; i++)
        sum += (double)h->buckets[i] * ((double)i + 0.5) * LAT_BUCKET_US;
    return sum / (double)h->count;
}

static void lat_merge(lat_hist_t *dst, const lat_hist_t *src) {
    for (int i = 0; i < LAT_BUCKETS; i++) dst->buckets[i] += src->buckets[i];
    dst->overflow += src->overflow;
    dst->count += src->count;
}

// ============================================================================
// I/O slot and worker context
// ============================================================================

typedef enum { SLOT_FREE = 0, SLOT_INFLIGHT } slot_state_t;

struct worker_ctx;

typedef struct {
    void *buf;
    struct worker_ctx *wctx;
    struct timespec submit_time;
    bool is_write;
    slot_state_t state;
} io_slot_t;

typedef struct worker_ctx {
    int worker_id;
    int fd;
    off_t file_size;
    size_t io_size;
    const workload_t *workload;

    /* Role within mixed workloads */
    bool force_read;
    bool force_write;
    bool sequential;
    off_t seq_offset;

    /* Pipeline state */
    aura_engine_t *engine;
    io_slot_t *slots;
    bool stopping;
    double max_p99_latency_ms;
    struct timespec warmup_end; /* completions before this are excluded from stats */

    /* Measurement timing — set on first post-warmup completion */
    struct timespec measure_start;
    struct timespec measure_end;
    bool measure_started;

    /* Results (thread-local, no synchronization needed — each worker has its
     * own engine with single_thread=true, so callbacks run on the same thread
     * that calls aura_wait). */
    int64_t bytes_read;
    int64_t bytes_written;
    int64_t ops_read;
    int64_t ops_written;
    lat_hist_t hist;
    int error;
    int active_ops;
} worker_ctx_t;

// ============================================================================
// I/O submission and completion (callback-driven resubmit)
// ============================================================================

static void submit_one(worker_ctx_t *wctx, io_slot_t *slot);

static void on_complete(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    io_slot_t *slot = (io_slot_t *)user_data;
    worker_ctx_t *wctx = slot->wctx;

    slot->state = SLOT_FREE;
    wctx->active_ops--;

    if (result < 0) {
        if (wctx->error == 0) wctx->error = (int)(-result);
        return;
    }

    /* Short reads/writes are counted at actual bytes transferred. With O_DIRECT
     * and block-aligned offsets these are rare; not worth resubmitting the
     * remainder for a benchmark. */

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Skip warmup period — let AIMD converge before recording stats */
    if (now.tv_sec < wctx->warmup_end.tv_sec ||
        (now.tv_sec == wctx->warmup_end.tv_sec && now.tv_nsec < wctx->warmup_end.tv_nsec)) {
        if (!wctx->stopping && wctx->error == 0) submit_one(wctx, slot);
        return;
    }

    /* Track actual measurement window */
    if (!wctx->measure_started) {
        wctx->measure_start = now;
        wctx->measure_started = true;
    }
    wctx->measure_end = now;

    double lat_us = (double)(now.tv_sec - slot->submit_time.tv_sec) * 1e6 +
                    (double)(now.tv_nsec - slot->submit_time.tv_nsec) / 1e3;
    lat_record(&wctx->hist, lat_us);

    if (slot->is_write) {
        wctx->bytes_written += result;
        wctx->ops_written++;
    } else {
        wctx->bytes_read += result;
        wctx->ops_read++;
    }

    /* Resubmit immediately to keep pipeline full */
    if (!wctx->stopping && wctx->error == 0) submit_one(wctx, slot);
}

static void submit_one(worker_ctx_t *wctx, io_slot_t *slot) {
    const workload_t *w = wctx->workload;
    size_t io_size = wctx->io_size;
    bool do_write;

    if (wctx->force_write) {
        do_write = true;
    } else if (wctx->force_read) {
        do_write = false;
    } else {
        do_write = ((int)(rng_next() % 100) >= w->read_pct);
    }

    off_t offset;
    if (wctx->sequential) {
        offset = wctx->seq_offset;
        wctx->seq_offset += (off_t)io_size;
        if (wctx->seq_offset + (off_t)io_size > wctx->file_size) wctx->seq_offset = 0;
    } else {
        offset = rng_offset(wctx->file_size, io_size);
    }

    slot->is_write = do_write;
    slot->state = SLOT_INFLIGHT;
    clock_gettime(CLOCK_MONOTONIC, &slot->submit_time);

    aura_request_t *r;
    if (do_write) {
        /* Stamp offset into first 8 bytes to defeat dedup */
        memcpy(slot->buf, &offset, sizeof(offset));
        r = aura_write(wctx->engine, wctx->fd, aura_buf(slot->buf), io_size, offset, on_complete,
                       slot);
    } else {
        r = aura_read(wctx->engine, wctx->fd, aura_buf(slot->buf), io_size, offset, on_complete,
                      slot);
    }
    if (!r) {
        slot->state = SLOT_FREE;
        if (errno != EAGAIN && wctx->error == 0) wctx->error = errno;
    } else {
        wctx->active_ops++;
    }
}

// ============================================================================
// Worker lifecycle (used by both single-thread and multi-thread paths)
// ============================================================================

static int worker_run(worker_ctx_t *wctx, double duration_sec) {
    rng_seed(wctx->worker_id);

    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = PIPELINE_DEPTH * 4;
    opts.single_thread = true;
    opts.ring_count = 1;
    if (wctx->max_p99_latency_ms > 0) opts.max_p99_latency_ms = wctx->max_p99_latency_ms;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) return errno;
    wctx->engine = engine;

    /* Stack-allocated slots — safe because aura_drain() below completes all
     * in-flight ops before we return. */
    io_slot_t slots[PIPELINE_DEPTH];
    memset(slots, 0, sizeof(slots));
    wctx->slots = slots;

    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        slots[i].buf = aura_buffer_alloc(engine, wctx->io_size);
        if (!slots[i].buf) {
            wctx->error = ENOMEM;
            goto cleanup;
        }
        /* Fill with PRNG data to defeat compression on data-reducing arrays */
        uint64_t *p = (uint64_t *)slots[i].buf;
        for (size_t j = 0; j < wctx->io_size / sizeof(uint64_t); j++) p[j] = rng_next();
        slots[i].wctx = wctx;
        slots[i].state = SLOT_FREE;
    }

    /* Fill pipeline */
    for (int i = 0; i < PIPELINE_DEPTH && wctx->error == 0; i++) submit_one(wctx, &slots[i]);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Set warmup end: 3 seconds after start when latency target is set,
     * otherwise no warmup (record from the beginning). */
    wctx->warmup_end = start;
    if (wctx->max_p99_latency_ms > 0) {
        wctx->warmup_end.tv_sec += 3;
    }

    /* Event loop: wait for completions, callbacks resubmit automatically.
     * Also retry any FREE slots that failed with EAGAIN on submission. */
    while (!wctx->stopping && wctx->error == 0 && !g_interrupted) {
        int n = aura_wait(engine, 100);
        if (n < 0 && errno != EINTR && errno != ETIME && errno != ETIMEDOUT) break;

        /* Retry slots that couldn't submit (EAGAIN) */
        for (int i = 0; i < PIPELINE_DEPTH && wctx->error == 0 && !wctx->stopping; i++) {
            if (slots[i].state == SLOT_FREE) submit_one(wctx, &slots[i]);
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed =
            (double)(now.tv_sec - start.tv_sec) + (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        if (elapsed >= duration_sec) break;
    }

    /* Stop resubmitting and drain in-flight ops */
    wctx->stopping = true;
    if (wctx->active_ops > 0) aura_drain(engine, 3000);

cleanup:
    for (int i = 0; i < PIPELINE_DEPTH; i++) {
        if (slots[i].buf) aura_buffer_free(engine, slots[i].buf);
    }
    wctx->engine = NULL;
    wctx->slots = NULL;
    aura_destroy(engine);
    return wctx->error;
}

// ============================================================================
// Multi-threaded test runner
// ============================================================================

static int get_num_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > MAX_WORKERS) n = MAX_WORKERS;
    return (int)n;
}

typedef struct {
    int64_t bytes_read;
    int64_t bytes_written;
    int64_t ops_read;
    int64_t ops_written;
    lat_hist_t hist;
    double elapsed_sec;
} test_result_t;

/* Assign roles to threads based on workload pattern */
static void assign_thread_role(worker_ctx_t *wctx, int thread_id, int total_threads,
                               const workload_t *w) {
    wctx->workload = w;
    wctx->worker_id = thread_id;
    wctx->io_size = w->io_size;

    switch (w->pattern) {
    case PATTERN_SEQ_WRITE:
        wctx->force_write = true;
        wctx->sequential = true;
        break;
    case PATTERN_SEQ_READ:
        wctx->force_read = true;
        wctx->sequential = true;
        break;
    case PATTERN_RAND_RW:
        break;
    case PATTERN_RAND_READ:
        wctx->force_read = true;
        break;
    case PATTERN_MULTI_SEQ_WRITE:
        wctx->force_write = true;
        wctx->sequential = true;
        wctx->seq_offset = (wctx->file_size / total_threads) * thread_id;
        break;
    case PATTERN_KV_STORE:
        if (thread_id == 0) {
            wctx->force_write = true;
            wctx->sequential = true;
        } else {
            wctx->force_read = true;
        }
        break;
    case PATTERN_TRAINING:
        wctx->force_read = true;
        if (thread_id == 0) wctx->sequential = true;
        break;
    case PATTERN_LAKEHOUSE:
        if (thread_id < total_threads * 4 / 10 || total_threads == 1) {
            wctx->force_read = true;
            wctx->sequential = true;
            wctx->seq_offset = (wctx->file_size / total_threads) * thread_id;
        } else if (thread_id < total_threads * 7 / 10) {
            wctx->force_write = true;
            wctx->sequential = true;
            wctx->seq_offset = (wctx->file_size / total_threads) * thread_id;
        } else {
            wctx->force_read = true;
            wctx->io_size = 4096;
        }
        break;
    }
}

typedef struct {
    worker_ctx_t wctx;
    double duration_sec;
} mt_worker_t;

static void *mt_worker_fn(void *arg) {
    mt_worker_t *mw = (mt_worker_t *)arg;
    worker_run(&mw->wctx, mw->duration_sec);
    return NULL;
}

static int run_test(int fd, off_t file_size, const workload_t *w, int num_threads,
                    double duration_sec, double max_p99_latency_ms, test_result_t *result) {
    memset(result, 0, sizeof(*result));

    /* IO size must be 512-aligned for O_DIRECT */
    if (w->io_size == 0 || w->io_size % 512 != 0) return EINVAL;

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    if (num_threads <= 1) {
        worker_ctx_t wctx;
        memset(&wctx, 0, sizeof(wctx));
        wctx.fd = fd;
        wctx.file_size = file_size;
        wctx.max_p99_latency_ms = max_p99_latency_ms;
        assign_thread_role(&wctx, 0, 1, w);

        worker_run(&wctx, duration_sec);

        if (wctx.measure_started) {
            result->elapsed_sec =
                (double)(wctx.measure_end.tv_sec - wctx.measure_start.tv_sec) +
                (double)(wctx.measure_end.tv_nsec - wctx.measure_start.tv_nsec) / 1e9;
        } else {
            struct timespec end;
            clock_gettime(CLOCK_MONOTONIC, &end);
            result->elapsed_sec =
                (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
        }
        if (result->elapsed_sec < 0.1) result->elapsed_sec = 0.1;
        result->bytes_read = wctx.bytes_read;
        result->bytes_written = wctx.bytes_written;
        result->ops_read = wctx.ops_read;
        result->ops_written = wctx.ops_written;
        result->hist = wctx.hist;
        return wctx.error;
    }

    /* Multi-threaded: all workers share the same fd. This is safe because
     * io_uring preadv/pwritev operations are independent per-offset. */
    mt_worker_t *workers = calloc((size_t)num_threads, sizeof(mt_worker_t));
    if (!workers) return ENOMEM;

    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));
    if (!threads) {
        free(workers);
        return ENOMEM;
    }

    for (int i = 0; i < num_threads; i++) {
        workers[i].wctx.fd = fd;
        workers[i].wctx.file_size = file_size;
        workers[i].wctx.max_p99_latency_ms = max_p99_latency_ms;
        workers[i].duration_sec = duration_sec;
        assign_thread_role(&workers[i].wctx, i, num_threads, w);
    }

    for (int i = 0; i < num_threads; i++) {
        int rc = pthread_create(&threads[i], NULL, mt_worker_fn, &workers[i]);
        if (rc != 0) {
            for (int j = 0; j < i; j++) {
                workers[j].wctx.stopping = true;
                pthread_join(threads[j], NULL);
            }
            free(threads);
            free(workers);
            return rc;
        }
    }

    for (int i = 0; i < num_threads; i++) pthread_join(threads[i], NULL);

    /* Use actual measurement window: earliest start to latest end across workers */
    struct timespec m_start = { 0 }, m_end = { 0 };
    bool any_measured = false;
    for (int i = 0; i < num_threads; i++) {
        if (!workers[i].wctx.measure_started) continue;
        struct timespec ws = workers[i].wctx.measure_start;
        struct timespec we = workers[i].wctx.measure_end;
        if (!any_measured || ws.tv_sec < m_start.tv_sec ||
            (ws.tv_sec == m_start.tv_sec && ws.tv_nsec < m_start.tv_nsec))
            m_start = ws;
        if (!any_measured || we.tv_sec > m_end.tv_sec ||
            (we.tv_sec == m_end.tv_sec && we.tv_nsec > m_end.tv_nsec))
            m_end = we;
        any_measured = true;
    }
    if (any_measured) {
        result->elapsed_sec = (double)(m_end.tv_sec - m_start.tv_sec) +
                              (double)(m_end.tv_nsec - m_start.tv_nsec) / 1e9;
    } else {
        struct timespec end;
        clock_gettime(CLOCK_MONOTONIC, &end);
        result->elapsed_sec =
            (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    }
    if (result->elapsed_sec < 0.1) result->elapsed_sec = 0.1;

    int err = 0;
    for (int i = 0; i < num_threads; i++) {
        result->bytes_read += workers[i].wctx.bytes_read;
        result->bytes_written += workers[i].wctx.bytes_written;
        result->ops_read += workers[i].wctx.ops_read;
        result->ops_written += workers[i].wctx.ops_written;
        lat_merge(&result->hist, &workers[i].wctx.hist);
        if (workers[i].wctx.error && !err) err = workers[i].wctx.error;
    }

    free(threads);
    free(workers);
    return err;
}

// ============================================================================
// Formatting helpers
// ============================================================================

static void fmt_comma(char *buf, size_t bufsz, int64_t val) {
    char tmp[32];
    int len = snprintf(tmp, sizeof(tmp), "%" PRId64, val);
    if (len <= 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    /* Skip past minus sign for comma insertion */
    int prefix = (tmp[0] == '-') ? 1 : 0;
    int digits_len = len - prefix;

    int commas = (digits_len - 1) / 3;
    int total = len + commas;
    if ((size_t)total >= bufsz) {
        snprintf(buf, bufsz, "%" PRId64, val);
        return;
    }

    buf[total] = '\0';
    int src = len - 1, dst = total - 1, digits = 0;
    while (src >= prefix) {
        buf[dst--] = tmp[src--];
        digits++;
        if (digits % 3 == 0 && src >= prefix) buf[dst--] = ',';
    }
    if (prefix) buf[0] = '-';
}

static void fmt_lat(char *buf, size_t bufsz, double us) {
    snprintf(buf, bufsz, "%.2f ms", us / 1000.0);
}

/* Bandwidth in base-1000 MB/s (fio convention); IO sizes in binary KiB/MiB */
static void fmt_bw(char *buf, size_t bufsz, double bps) {
    int64_t mbps = (int64_t)(bps / (1000.0 * 1000.0));
    fmt_comma(buf, bufsz, mbps);
}

static void fmt_io_size(char *buf, size_t bufsz, size_t sz) {
    if (sz >= 1024 * 1024) snprintf(buf, bufsz, "%zuM", sz / (1024 * 1024));
    else if (sz >= 1024) snprintf(buf, bufsz, "%zuK", sz / 1024);
    else snprintf(buf, bufsz, "%zu", sz);
}

// ============================================================================
// Size parsing
// ============================================================================

static ssize_t parse_size(const char *str) {
    char *endp;
    double val = strtod(str, &endp);
    if (endp == str || val < 0) return -1;

    switch (*endp) {
    case 'G':
    case 'g':
        val *= 1024.0 * 1024.0 * 1024.0;
        break;
    case 'M':
    case 'm':
        val *= 1024.0 * 1024.0;
        break;
    case 'K':
    case 'k':
        val *= 1024.0;
        break;
    case '\0':
        break;
    default:
        return -1;
    }

    if (val > (double)SSIZE_MAX) return -1;
    return (ssize_t)val;
}

// ============================================================================
// Test file management
// ============================================================================

static char g_tmpfile[4096];

static void cleanup_tmpfile(void) {
    if (g_tmpfile[0]) {
        unlink(g_tmpfile);
        g_tmpfile[0] = '\0';
    }
}

static void sigint_handler(int sig) {
    (void)sig;
    g_interrupted = 1;
}

static int create_test_file(const char *path, off_t size) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    const size_t chunk = 1024 * 1024;
    char *buf = aligned_alloc(4096, chunk);
    if (!buf) {
        close(fd);
        return -1;
    }
    /* Fill with PRNG data so data-reducing arrays can't compress/dedup it away */
    uint64_t rng = 0xDEADBEEFCAFE1234ULL;
    uint64_t *p = (uint64_t *)buf;
    for (size_t i = 0; i < chunk / sizeof(uint64_t); i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        p[i] = rng;
    }

    off_t written = 0;
    while (written < size) {
        size_t to_write = chunk;
        if (written + (off_t)to_write > size) to_write = (size_t)(size - written);
        /* Stamp offset to make each chunk unique (defeats dedup) */
        memcpy(buf, &written, sizeof(written));
        ssize_t n = write(fd, buf, to_write);
        if (n <= 0) {
            free(buf);
            close(fd);
            return -1;
        }
        written += n;

        if (isatty(STDERR_FILENO)) {
            int pct = (int)((double)written / (double)size * 100.0);
            fprintf(stderr, "\r  Creating test file... %d%%", pct);
        }
    }

    if (isatty(STDERR_FILENO)) fprintf(stderr, "\r  Creating test file... done    \n");

    free(buf);
    fsync(fd);
    close(fd);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options] [path] [size]\n"
            "\n"
            "Simple storage performance analyzer powered by AuraIO.\n"
            "\n"
            "Arguments:\n"
            "  path   Test directory (default: current directory)\n"
            "  size   Test file size, e.g. 512M, 1G (default: auto)\n"
            "\n"
            "Options:\n"
            "  -l MS  Max P99 latency target in milliseconds (enables AIMD tuning)\n"
            "  -h     Show this help\n"
            "\n"
            "Auto-sizing: 10%% of free space, clamped to 256M-1G.\n"
            "Minimum 256 MiB free space required.\n",
            argv0);
}

int main(int argc, char **argv) {
    const char *test_dir = ".";
    off_t test_size = 0;
    bool user_size = false;
    double max_p99_latency_ms = 0;

    int opt;
    while ((opt = getopt(argc, argv, "hl:")) != -1) {
        switch (opt) {
        case 'l': {
            char *endp;
            max_p99_latency_ms = strtod(optarg, &endp);
            if (endp == optarg || max_p99_latency_ms <= 0) {
                fprintf(stderr, "sspa: invalid latency: %s\n", optarg);
                return 1;
            }
            break;
        }
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind < argc) test_dir = argv[optind++];
    if (optind < argc) {
        ssize_t sz = parse_size(argv[optind++]);
        if (sz < 0) {
            fprintf(stderr, "sspa: invalid size: %s\n", argv[optind - 1]);
            return 1;
        }
        test_size = (off_t)sz;
        user_size = true;
    }
    if (optind < argc) {
        print_usage(argv[0]);
        return 1;
    }

    struct stat st;
    if (stat(test_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "sspa: '%s' is not a valid directory\n", test_dir);
        return 1;
    }

    if (!user_size) {
        struct statvfs vfs;
        if (statvfs(test_dir, &vfs) != 0) {
            fprintf(stderr, "sspa: cannot stat filesystem: %s\n", strerror(errno));
            return 1;
        }
        uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        uint64_t auto_size = free_bytes / AUTO_FRACTION;

        if (auto_size < MIN_TEST_SIZE) {
            fprintf(stderr,
                    "sspa: insufficient free space (need at least 2.56 GiB for auto-sizing)\n"
                    "  256 MiB minimum required. Use explicit size to override.\n");
            return 1;
        }
        if (auto_size > MAX_AUTO_SIZE) auto_size = MAX_AUTO_SIZE;
        test_size = (off_t)auto_size;
    } else {
        if ((uint64_t)test_size < MIN_TEST_SIZE) {
            fprintf(stderr, "sspa: test size must be at least 256 MiB\n");
            return 1;
        }
    }

    double duration_sec = 10.0;

    int num_cpus = get_num_cpus();

    snprintf(g_tmpfile, sizeof(g_tmpfile), "%s/%s", test_dir, TMP_FILENAME);
    atexit(cleanup_tmpfile);
    struct sigaction sa = { .sa_handler = sigint_handler, .sa_flags = SA_RESETHAND };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int64_t size_mib = test_size / (1024 * 1024);
    char size_str[32];
    fmt_comma(size_str, sizeof(size_str), size_mib);
    if (max_p99_latency_ms > 0)
        fprintf(stderr, "\nsspa: %s  (%s MiB test file, %.0fs per test, P99 target: %.1f ms)\n\n",
                test_dir, size_str, duration_sec, max_p99_latency_ms);
    else
        fprintf(stderr, "\nsspa: %s  (%s MiB test file, %.0fs per test)\n\n", test_dir, size_str,
                duration_sec);

    if (create_test_file(g_tmpfile, test_size) != 0) {
        fprintf(stderr, "sspa: failed to create test file: %s\n", strerror(errno));
        return 1;
    }

    int fd = open(g_tmpfile, O_RDWR | O_DIRECT);
    if (fd < 0) fd = open(g_tmpfile, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "sspa: cannot open test file: %s\n", strerror(errno));
        return 1;
    }

    (void)posix_fadvise(fd, 0, test_size, POSIX_FADV_DONTNEED);

    printf("  %-12s %-20s %7s  %6s %4s  %10s %10s  %9s %9s\n", "Test", "Pattern", "IO Size",
           "R/W %", "Thr", "Bandwidth", "IOPS", "Avg Lat", "P99 Lat");
    printf("  ");
    for (int i = 0; i < 98; i++) printf("%s", "\xe2\x94\x80");
    printf("\n");

    int64_t total_ops = 0, total_reads = 0, total_writes = 0;

    for (int t = 0; t < NUM_WORKLOADS && !g_interrupted; t++) {
        const workload_t *w = &workloads[t];
        int threads = w->threads ? w->threads : num_cpus;
        if (threads < 1) threads = 1;

        if (isatty(STDERR_FILENO)) {
            fprintf(stderr, "\r  Running %-12s...", w->name);
            fflush(stderr);
        }

        (void)posix_fadvise(fd, 0, test_size, POSIX_FADV_DONTNEED);

        test_result_t result;
        int err = run_test(fd, test_size, w, threads, duration_sec, max_p99_latency_ms, &result);

        if (isatty(STDERR_FILENO)) fprintf(stderr, "\r                                    \r");

        if (err && !g_interrupted) {
            fprintf(stderr, "  %-12s ERROR: %s\n", w->name, strerror(err));
            continue;
        }

        int64_t total_bytes = result.bytes_read + result.bytes_written;
        int64_t total_io = result.ops_read + result.ops_written;
        double bw = result.elapsed_sec > 0 ? (double)total_bytes / result.elapsed_sec : 0;
        double iops = result.elapsed_sec > 0 ? (double)total_io / result.elapsed_sec : 0;
        double avg_lat_us = lat_avg(&result.hist);
        double p99_lat_us = lat_percentile(&result.hist, 99.0);

        total_ops += total_io;
        total_reads += result.ops_read;
        total_writes += result.ops_written;

        char bw_str[32], iops_str[32], avg_str[32], p99_str[32], io_str[24], rw_str[24];
        fmt_bw(bw_str, sizeof(bw_str), bw);
        fmt_comma(iops_str, sizeof(iops_str), (int64_t)iops);
        fmt_lat(avg_str, sizeof(avg_str), avg_lat_us);
        fmt_lat(p99_str, sizeof(p99_str), p99_lat_us);
        fmt_io_size(io_str, sizeof(io_str), w->io_size);
        snprintf(rw_str, sizeof(rw_str), "%d/%d", w->read_pct, 100 - w->read_pct);

        printf("  %-12s %-20s %5s    %5s  %3d  %7s MB/s %10s  %9s %9s\n", w->name, w->description,
               io_str, rw_str, threads, bw_str, iops_str, avg_str, p99_str);
    }

    close(fd);

    printf("\n");
    char total_str[32];
    fmt_comma(total_str, sizeof(total_str), total_ops);
    int read_pct = total_ops > 0 ? (int)((double)total_reads / (double)total_ops * 100.0) : 0;
    printf("  Total I/O: %s ops  (%d%% read, %d%% write)\n\n", total_str, read_pct, 100 - read_pct);
    printf("done.\n");

    return g_interrupted ? 130 : 0;
}
