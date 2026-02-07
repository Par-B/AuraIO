/**
 * @file stats.c
 * @brief Stats collection and result computation for BFFIO
 *
 * Provides atomic per-thread I/O counters, latency histograms with 50us
 * bucket resolution, periodic BW/IOPS sampling, multi-thread aggregation,
 * and FIO-compatible result computation including all 17 standard percentiles.
 */

#include "stats.h"

#include <string.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>

/* FIO-standard percentile list */
const double PERCENTILE_LIST[NUM_PERCENTILES] = {
    1.0, 5.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0,
    70.0, 80.0, 90.0, 95.0, 99.0, 99.5, 99.9, 99.95, 99.99
};

/* --------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------- */

void stats_init(thread_stats_t *s)
{
    memset(s, 0, sizeof(*s));
    atomic_store_explicit(&s->lat_min_ns, UINT64_MAX, memory_order_relaxed);
}

void stats_reset(thread_stats_t *s)
{
    atomic_store_explicit(&s->read_ops, 0, memory_order_relaxed);
    atomic_store_explicit(&s->write_ops, 0, memory_order_relaxed);
    atomic_store_explicit(&s->read_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&s->write_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&s->errors, 0, memory_order_relaxed);
    atomic_store_explicit(&s->inflight, 0, memory_order_relaxed);

    for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
        atomic_store_explicit(&s->lat_hist[i], 0, memory_order_relaxed);
    }
    atomic_store_explicit(&s->lat_overflow, 0, memory_order_relaxed);

    atomic_store_explicit(&s->lat_min_ns, UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&s->lat_max_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->lat_sum_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&s->lat_count, 0, memory_order_relaxed);

    memset(s->bw_samples, 0, sizeof(s->bw_samples));
    memset(s->iops_samples, 0, sizeof(s->iops_samples));
    s->sample_count = 0;
    s->last_bytes = 0;
    s->last_ops = 0;
}

/* --------------------------------------------------------------------
 * Recording
 * -------------------------------------------------------------------- */

void stats_record_io(thread_stats_t *s, uint64_t latency_ns, size_t bytes,
                     int is_write)
{
    /* Count ops and bytes */
    if (is_write) {
        atomic_fetch_add_explicit(&s->write_ops, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->write_bytes, bytes, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s->read_ops, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&s->read_bytes, bytes, memory_order_relaxed);
    }

    /* Latency histogram: bucket = latency_us / 50, clamped to [0, 199] */
    uint64_t latency_us = latency_ns / 1000;
    int bucket = (int)(latency_us / LAT_HIST_BUCKET_US);

    if (bucket >= LAT_HIST_BUCKETS) {
        atomic_fetch_add_explicit(&s->lat_overflow, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s->lat_hist[bucket], 1, memory_order_relaxed);
    }

    /* Update lat_min with CAS loop (only if new value is smaller) */
    uint64_t cur_min = atomic_load_explicit(&s->lat_min_ns, memory_order_relaxed);
    while (latency_ns < cur_min) {
        if (atomic_compare_exchange_weak_explicit(&s->lat_min_ns, &cur_min,
                                                  latency_ns,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
        /* cur_min is updated by CAS on failure */
    }

    /* Update lat_max with CAS loop (only if new value is larger) */
    uint64_t cur_max = atomic_load_explicit(&s->lat_max_ns, memory_order_relaxed);
    while (latency_ns > cur_max) {
        if (atomic_compare_exchange_weak_explicit(&s->lat_max_ns, &cur_max,
                                                  latency_ns,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed)) {
            break;
        }
    }

    /* Accumulate sum and count for mean calculation */
    atomic_fetch_add_explicit(&s->lat_sum_ns, latency_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&s->lat_count, 1, memory_order_relaxed);
}

/* --------------------------------------------------------------------
 * Sampling (called from main thread every 1s)
 * -------------------------------------------------------------------- */

void stats_take_sample(thread_stats_t *s)
{
    if (s->sample_count >= MAX_SAMPLES) {
        return;
    }

    uint64_t cur_bytes = atomic_load_explicit(&s->read_bytes, memory_order_relaxed)
                       + atomic_load_explicit(&s->write_bytes, memory_order_relaxed);
    uint64_t cur_ops = atomic_load_explicit(&s->read_ops, memory_order_relaxed)
                     + atomic_load_explicit(&s->write_ops, memory_order_relaxed);

    uint64_t delta_bytes = cur_bytes - s->last_bytes;
    uint64_t delta_ops   = cur_ops   - s->last_ops;

    s->bw_samples[s->sample_count]   = delta_bytes;
    s->iops_samples[s->sample_count] = delta_ops;

    s->last_bytes = cur_bytes;
    s->last_ops   = cur_ops;
    s->sample_count++;
}

/* --------------------------------------------------------------------
 * Aggregation
 * -------------------------------------------------------------------- */

void stats_aggregate(const thread_stats_t *per_thread, int num_threads,
                     thread_stats_t *out)
{
    stats_init(out);

    for (int t = 0; t < num_threads; t++) {
        const thread_stats_t *src = &per_thread[t];

        /* Sum ops and bytes */
        uint64_t rops = atomic_load_explicit(&src->read_ops, memory_order_relaxed);
        uint64_t wops = atomic_load_explicit(&src->write_ops, memory_order_relaxed);
        uint64_t rb   = atomic_load_explicit(&src->read_bytes, memory_order_relaxed);
        uint64_t wb   = atomic_load_explicit(&src->write_bytes, memory_order_relaxed);
        uint64_t err  = atomic_load_explicit(&src->errors, memory_order_relaxed);

        atomic_fetch_add_explicit(&out->read_ops, rops, memory_order_relaxed);
        atomic_fetch_add_explicit(&out->write_ops, wops, memory_order_relaxed);
        atomic_fetch_add_explicit(&out->read_bytes, rb, memory_order_relaxed);
        atomic_fetch_add_explicit(&out->write_bytes, wb, memory_order_relaxed);
        atomic_fetch_add_explicit(&out->errors, err, memory_order_relaxed);

        /* Merge histograms */
        for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
            uint64_t count = atomic_load_explicit(&src->lat_hist[i],
                                                  memory_order_relaxed);
            atomic_fetch_add_explicit(&out->lat_hist[i], count,
                                      memory_order_relaxed);
        }
        uint64_t overflow = atomic_load_explicit(&src->lat_overflow,
                                                 memory_order_relaxed);
        atomic_fetch_add_explicit(&out->lat_overflow, overflow,
                                  memory_order_relaxed);

        /* Min of mins */
        uint64_t src_min = atomic_load_explicit(&src->lat_min_ns,
                                                memory_order_relaxed);
        uint64_t cur_min = atomic_load_explicit(&out->lat_min_ns,
                                                memory_order_relaxed);
        while (src_min < cur_min) {
            if (atomic_compare_exchange_weak_explicit(&out->lat_min_ns,
                                                     &cur_min, src_min,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed)) {
                break;
            }
        }

        /* Max of maxes */
        uint64_t src_max = atomic_load_explicit(&src->lat_max_ns,
                                                memory_order_relaxed);
        uint64_t cur_max = atomic_load_explicit(&out->lat_max_ns,
                                                memory_order_relaxed);
        while (src_max > cur_max) {
            if (atomic_compare_exchange_weak_explicit(&out->lat_max_ns,
                                                     &cur_max, src_max,
                                                     memory_order_relaxed,
                                                     memory_order_relaxed)) {
                break;
            }
        }

        /* Sum latency sum and count */
        uint64_t lsum = atomic_load_explicit(&src->lat_sum_ns,
                                             memory_order_relaxed);
        uint64_t lcnt = atomic_load_explicit(&src->lat_count,
                                             memory_order_relaxed);
        atomic_fetch_add_explicit(&out->lat_sum_ns, lsum, memory_order_relaxed);
        atomic_fetch_add_explicit(&out->lat_count, lcnt, memory_order_relaxed);

        /* Merge BW/IOPS samples (element-wise sum across threads) */
        int max_samples = src->sample_count;
        if (max_samples > out->sample_count) {
            out->sample_count = max_samples;
        }
        for (int i = 0; i < src->sample_count; i++) {
            out->bw_samples[i]   += src->bw_samples[i];
            out->iops_samples[i] += src->iops_samples[i];
        }
    }
}

/* --------------------------------------------------------------------
 * Percentile computation
 * -------------------------------------------------------------------- */

uint64_t stats_percentile(const thread_stats_t *s, double pct)
{
    uint64_t total = 0;
    for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
        total += atomic_load_explicit(&s->lat_hist[i], memory_order_relaxed);
    }
    uint64_t overflow = atomic_load_explicit(&s->lat_overflow,
                                             memory_order_relaxed);
    uint64_t grand_total = total + overflow;

    if (grand_total == 0) {
        return 0;
    }

    /* Threshold: number of samples at or below the percentile */
    double threshold = (pct / 100.0) * (double)grand_total;
    uint64_t cumulative = 0;

    for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
        cumulative += atomic_load_explicit(&s->lat_hist[i],
                                           memory_order_relaxed);
        if ((double)cumulative >= threshold) {
            /* Return upper bound of this bucket in nanoseconds */
            return (uint64_t)(i + 1) * LAT_HIST_BUCKET_US * 1000ULL;
        }
    }

    /* Fell through to overflow: return max tracked value */
    return (uint64_t)LAT_HIST_MAX_US * 1000ULL;
}

/* --------------------------------------------------------------------
 * Helper: compute stddev of a sample array
 * -------------------------------------------------------------------- */

static void compute_sample_stats(const uint64_t *samples, int n,
                                 int64_t *out_min, int64_t *out_max,
                                 double *out_mean, double *out_stddev)
{
    if (n <= 0) {
        *out_min = 0;
        *out_max = 0;
        *out_mean = 0.0;
        *out_stddev = 0.0;
        return;
    }

    int64_t min_val = (int64_t)samples[0];
    int64_t max_val = (int64_t)samples[0];
    double sum = 0.0;

    for (int i = 0; i < n; i++) {
        int64_t v = (int64_t)samples[i];
        if (v < min_val) min_val = v;
        if (v > max_val) max_val = v;
        sum += (double)samples[i];
    }

    double mean = sum / n;
    double variance = 0.0;
    for (int i = 0; i < n; i++) {
        double d = (double)samples[i] - mean;
        variance += d * d;
    }
    double stddev = sqrt(variance / (n > 1 ? n - 1 : 1));

    *out_min = min_val;
    *out_max = max_val;
    *out_mean = mean;
    *out_stddev = stddev;
}

/* --------------------------------------------------------------------
 * Helper: compute latency stddev from histogram
 * -------------------------------------------------------------------- */

static double compute_lat_stddev_from_hist(const thread_stats_t *s,
                                           double mean_ns)
{
    /*
     * Approximate stddev from histogram bucket midpoints.
     * For each bucket, treat all samples as being at the midpoint
     * and compute the weighted sum of squared deviations from the mean.
     */
    double weighted_sq_sum = 0.0;
    uint64_t total_count = 0;

    for (int i = 0; i < LAT_HIST_BUCKETS; i++) {
        uint64_t count = atomic_load_explicit(&s->lat_hist[i],
                                              memory_order_relaxed);
        if (count == 0) {
            continue;
        }
        /* Midpoint of bucket i in nanoseconds */
        double midpoint_ns = ((double)i + 0.5) * LAT_HIST_BUCKET_US * 1000.0;
        double d = midpoint_ns - mean_ns;
        weighted_sq_sum += (double)count * d * d;
        total_count += count;
    }

    /* Include overflow samples at the max tracked value */
    uint64_t overflow = atomic_load_explicit(&s->lat_overflow,
                                             memory_order_relaxed);
    if (overflow > 0) {
        double overflow_ns = (double)LAT_HIST_MAX_US * 1000.0;
        double d = overflow_ns - mean_ns;
        weighted_sq_sum += (double)overflow * d * d;
        total_count += overflow;
    }

    if (total_count <= 1) {
        return 0.0;
    }

    return sqrt(weighted_sq_sum / (double)(total_count - 1));
}

/* --------------------------------------------------------------------
 * Helper: populate one direction result
 * -------------------------------------------------------------------- */

static void compute_direction(const thread_stats_t *raw, uint64_t runtime_ms,
                              int is_write, direction_result_t *dir)
{
    memset(dir, 0, sizeof(*dir));

    uint64_t ops;
    uint64_t bytes;

    if (is_write) {
        ops   = atomic_load_explicit(&raw->write_ops, memory_order_relaxed);
        bytes = atomic_load_explicit(&raw->write_bytes, memory_order_relaxed);
    } else {
        ops   = atomic_load_explicit(&raw->read_ops, memory_order_relaxed);
        bytes = atomic_load_explicit(&raw->read_bytes, memory_order_relaxed);
    }

    dir->total_ios = ops;
    dir->io_bytes  = bytes;
    dir->io_kbytes = bytes / 1024;
    dir->short_ios = 0;
    dir->runtime_ms = (int64_t)runtime_ms;

    if (runtime_ms > 0) {
        double runtime_sec = (double)runtime_ms / 1000.0;
        dir->bw_bytes_sec  = (int64_t)((double)bytes / runtime_sec);
        dir->bw_kbytes_sec = dir->bw_bytes_sec / 1024;
        dir->iops          = (double)ops / runtime_sec;
    }

    /* Latency stats (shared across both directions in our histogram) */
    uint64_t lat_count = atomic_load_explicit(&raw->lat_count,
                                              memory_order_relaxed);
    dir->lat_count = lat_count;

    if (lat_count > 0) {
        uint64_t lat_min = atomic_load_explicit(&raw->lat_min_ns,
                                                memory_order_relaxed);
        uint64_t lat_max = atomic_load_explicit(&raw->lat_max_ns,
                                                memory_order_relaxed);
        uint64_t lat_sum = atomic_load_explicit(&raw->lat_sum_ns,
                                                memory_order_relaxed);

        dir->lat_min_ns = (lat_min == UINT64_MAX) ? 0 : lat_min;
        dir->lat_max_ns = lat_max;
        dir->lat_mean_ns = (double)lat_sum / (double)lat_count;
        dir->lat_stddev_ns = compute_lat_stddev_from_hist(raw,
                                                          dir->lat_mean_ns);

        /* Compute all 17 percentiles */
        for (int i = 0; i < NUM_PERCENTILES; i++) {
            dir->lat_percentiles_ns[i] = stats_percentile(raw,
                                                          PERCENTILE_LIST[i]);
        }
    }
}

/* --------------------------------------------------------------------
 * Result computation
 * -------------------------------------------------------------------- */

void stats_compute_results(const thread_stats_t *raw, uint64_t runtime_ms,
                           auraio_engine_t *engine, const char *jobname,
                           job_result_t *result)
{
    memset(result, 0, sizeof(*result));
    strncpy(result->jobname, jobname, sizeof(result->jobname) - 1);
    result->job_runtime_ms = (int64_t)runtime_ms;

    /* Compute read and write direction results */
    compute_direction(raw, runtime_ms, 0, &result->read);
    compute_direction(raw, runtime_ms, 1, &result->write);

    /* BW sample stats (convert bytes/sec to KiB/s) */
    if (raw->sample_count > 0) {
        /* Build KiB/s sample arrays for BW */
        uint64_t bw_kib[MAX_SAMPLES];
        for (int i = 0; i < raw->sample_count; i++) {
            bw_kib[i] = raw->bw_samples[i] / 1024;
        }

        int64_t bw_min, bw_max;
        double bw_mean, bw_dev;
        compute_sample_stats(bw_kib, raw->sample_count,
                             &bw_min, &bw_max, &bw_mean, &bw_dev);

        /* Apply to whichever direction had I/O (or both) */
        uint64_t rops = atomic_load_explicit(&raw->read_ops,
                                             memory_order_relaxed);
        uint64_t wops = atomic_load_explicit(&raw->write_ops,
                                             memory_order_relaxed);

        if (rops > 0) {
            result->read.bw_min     = bw_min;
            result->read.bw_max     = bw_max;
            result->read.bw_mean    = bw_mean;
            result->read.bw_dev     = bw_dev;
            result->read.bw_samples = raw->sample_count;
        }
        if (wops > 0) {
            result->write.bw_min     = bw_min;
            result->write.bw_max     = bw_max;
            result->write.bw_mean    = bw_mean;
            result->write.bw_dev     = bw_dev;
            result->write.bw_samples = raw->sample_count;
        }

        /* IOPS sample stats */
        int64_t iops_min, iops_max;
        double iops_mean, iops_stddev;
        compute_sample_stats(raw->iops_samples, raw->sample_count,
                             &iops_min, &iops_max, &iops_mean, &iops_stddev);

        if (rops > 0) {
            result->read.iops_min     = iops_min;
            result->read.iops_max     = iops_max;
            result->read.iops_mean    = iops_mean;
            result->read.iops_stddev  = iops_stddev;
            result->read.iops_samples = raw->sample_count;
        }
        if (wops > 0) {
            result->write.iops_min     = iops_min;
            result->write.iops_max     = iops_max;
            result->write.iops_mean    = iops_mean;
            result->write.iops_stddev  = iops_stddev;
            result->write.iops_samples = raw->sample_count;
        }
    }

    /* Pull AuraIO adaptive tuning info */
    if (engine) {
        auraio_stats_t engine_stats;
        auraio_get_stats(engine, &engine_stats);
        result->auraio_final_depth    = engine_stats.optimal_in_flight;
        result->auraio_p99_ms         = engine_stats.p99_latency_ms;
        result->auraio_throughput_bps = engine_stats.current_throughput_bps;

        auraio_ring_stats_t ring_stats;
        if (auraio_get_ring_stats(engine, 0, &ring_stats) == 0) {
            result->auraio_phase = ring_stats.aimd_phase;
            const char *phase_str = auraio_phase_name(ring_stats.aimd_phase);
            strncpy(result->auraio_phase_name, phase_str,
                    sizeof(result->auraio_phase_name) - 1);
        }
    }
}
