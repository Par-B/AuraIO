#ifndef BFFIO_JOB_PARSER_H
#define BFFIO_JOB_PARSER_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <limits.h>
#include <stdbool.h>

/* I/O pattern */
typedef enum {
    RW_READ,        /* rw=read       - sequential read */
    RW_WRITE,       /* rw=write      - sequential write */
    RW_RANDREAD,    /* rw=randread   - random read */
    RW_RANDWRITE,   /* rw=randwrite  - random write */
    RW_READWRITE,   /* rw=readwrite  - sequential mixed */
    RW_RANDRW,      /* rw=randrw     - random mixed */
} rw_pattern_t;

/* Output format */
typedef enum {
    OUTPUT_NORMAL,  /* FIO-style text summary */
    OUTPUT_JSON,    /* FIO-compatible JSON */
} output_format_t;

/* Single job configuration */
typedef struct {
    char        name[128];
    rw_pattern_t rw;
    bool        rw_set;             /* true if rw was explicitly set */
    uint64_t    bs;                 /* block size in bytes (default 4096) */
    uint64_t    size;               /* file size in bytes */
    char        filename[PATH_MAX]; /* explicit file/device path */
    char        directory[PATH_MAX];/* auto-create files here */
    int         direct;             /* O_DIRECT (0 or 1) */
    int         runtime_sec;        /* max runtime in seconds */
    int         time_based;         /* run for fixed time */
    int         ramp_time_sec;      /* warmup seconds (stats discarded) */
    int         numjobs;            /* worker thread count */
    int         iodepth;            /* max in-flight cap */
    int         rwmixread;          /* read percentage for mixed (0-100) */
    int         group_reporting;    /* aggregate stats across threads */
    int         fsync_freq;         /* fsync every N writes (0=never) */
    double      target_p99_ms;      /* P99 latency ceiling (0=disabled) */
} job_config_t;

/* Top-level benchmark config */
typedef struct {
    job_config_t   *jobs;
    int             num_jobs;
    int             jobs_capacity;
    output_format_t output_format;
    char            output_file[PATH_MAX];
} bench_config_t;

/* Initialize job config with FIO-compatible defaults */
void job_config_defaults(job_config_t *job);

/* Parse a .fio job file. Returns 0 on success, -1 on error. */
int job_parse_file(const char *path, bench_config_t *config);

/* Parse CLI arguments into config. Returns 0 on success, -1 on error. */
int job_parse_cli(int argc, char **argv, bench_config_t *config);

/* Validate job config. Prints error and returns -1 if invalid. */
int job_config_validate(const job_config_t *job);

/* Free bench config resources */
void bench_config_free(bench_config_t *config);

/* Parse size string with K/M/G suffixes. Returns bytes, 0 on error. */
uint64_t parse_size(const char *str);

/* Parse rw pattern string. Returns -1 on error. */
int parse_rw(const char *str, rw_pattern_t *out);

/* Get string name for rw pattern */
const char *rw_pattern_name(rw_pattern_t rw);

/* Parse latency string with us/ms/s suffixes. Returns milliseconds, -1 on error. */
double parse_latency(const char *str);

#endif /* BFFIO_JOB_PARSER_H */
