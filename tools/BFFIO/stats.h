/**
 * @file stats.h
 * @brief Stats collection and result types for BFFIO
 *
 * Per-thread atomic counters, latency histograms, BW/IOPS sampling,
 * and final result aggregation for FIO-compatible output.
 */

#ifndef BFFIO_STATS_H
#define BFFIO_STATS_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <auraio.h>

/* Latency histogram: 200 buckets x 50us = 10ms max tracked */
#define LAT_HIST_BUCKETS 200
#define LAT_HIST_BUCKET_US 50
#define LAT_HIST_MAX_US (LAT_HIST_BUCKETS * LAT_HIST_BUCKET_US)

/* Max BW/IOPS samples (1 per second, up to 600s) */
#define MAX_SAMPLES 600

/* Standard percentiles to compute (matching FIO) */
#define NUM_PERCENTILES 17
extern const double PERCENTILE_LIST[NUM_PERCENTILES];
/* Values: 1, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99, 99.5, 99.9, 99.95, 99.99 */

/* Per-thread I/O statistics (atomic for concurrent callback access) */
typedef struct {
    _Atomic uint64_t read_ops;
    _Atomic uint64_t write_ops;
    _Atomic uint64_t read_bytes;
    _Atomic uint64_t write_bytes;
    _Atomic uint64_t errors;
    _Atomic int inflight;

    /* Latency histogram */
    _Atomic uint64_t lat_hist[LAT_HIST_BUCKETS];
    _Atomic uint64_t lat_overflow;

    /* Min/max/sum for average and stddev */
    _Atomic uint64_t lat_min_ns;
    _Atomic uint64_t lat_max_ns;
    _Atomic uint64_t lat_sum_ns;
    _Atomic uint64_t lat_count;

    /* BW/IOPS samples (written by main thread only, no atomic needed) */
    uint64_t bw_samples[MAX_SAMPLES];   /* bytes/sec per 1s interval */
    uint64_t iops_samples[MAX_SAMPLES]; /* ops/sec per 1s interval */
    int sample_count;

    /* Snapshot values for sampling delta */
    uint64_t last_bytes;
    uint64_t last_ops;
} thread_stats_t;

/* Results for one direction (read or write) */
typedef struct {
    uint64_t io_bytes;
    uint64_t io_kbytes;
    int64_t bw_bytes_sec;
    int64_t bw_kbytes_sec;
    double iops;
    int64_t runtime_ms;
    uint64_t total_ios;
    uint64_t short_ios;

    /* Latency (nanoseconds) */
    uint64_t lat_min_ns;
    uint64_t lat_max_ns;
    double lat_mean_ns;
    double lat_stddev_ns;
    uint64_t lat_count;
    uint64_t lat_percentiles_ns[NUM_PERCENTILES];

    /* BW samples */
    int64_t bw_min; /* KiB/s */
    int64_t bw_max;
    double bw_mean;
    double bw_dev;
    int bw_samples;

    /* IOPS samples */
    int64_t iops_min;
    int64_t iops_max;
    double iops_mean;
    double iops_stddev;
    int iops_samples;
} direction_result_t;

/* Full job result */
typedef struct {
    char jobname[128];
    direction_result_t read;
    direction_result_t write;
    int64_t job_runtime_ms;

    /* AuraIO adaptive tuning info */
    int auraio_final_depth;
    int auraio_phase;
    char auraio_phase_name[32];
    double auraio_p99_ms;
    double auraio_throughput_bps;
    double target_p99_ms;     /* User's P99 ceiling (0=not set) */
    uint64_t adaptive_spills; /* ADAPTIVE ring spill count */
} job_result_t;

/* Initialize thread stats (zero everything, set lat_min to max) */
void stats_init(thread_stats_t *s);

/* Reset stats (for ramp_time boundary) */
void stats_reset(thread_stats_t *s);

/* Record one completed I/O */
void stats_record_io(thread_stats_t *s, uint64_t latency_ns, size_t bytes, int is_write);

/* Take a BW/IOPS sample (called every 1s from main thread) */
void stats_take_sample(thread_stats_t *s);

/* Aggregate multiple per-thread stats into one */
void stats_aggregate(const thread_stats_t *per_thread, int num_threads, thread_stats_t *out);

/* Compute percentile from histogram (returns nanoseconds) */
uint64_t stats_percentile(const thread_stats_t *s, double pct);

/* Build final job result from raw stats + AuraIO engine state */
void stats_compute_results(const thread_stats_t *raw, uint64_t runtime_ms, auraio_engine_t *engine,
                           const char *jobname, job_result_t *result);

#endif /* BFFIO_STATS_H */
