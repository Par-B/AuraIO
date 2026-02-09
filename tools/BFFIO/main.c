/**
 * @file main.c
 * @brief BFFIO - Better Faster FIO (powered by AuraIO)
 *
 * CLI entry point that ties together job parsing, workload execution,
 * stats computation, and output rendering.  One fresh AuraIO engine
 * is created per job for clean AIMD convergence.
 */

#include "job_parser.h"
#include "workload.h"
#include "stats.h"
#include "output.h"

#include <auraio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------- */
/* Help text                                                                  */
/* -------------------------------------------------------------------------- */

static void print_usage(void) {
    printf("BFFIO - Better Faster FIO (powered by AuraIO)\n"
           "\n"
           "Usage:\n"
           "  BFFIO [options]                    Run with CLI parameters\n"
           "  BFFIO <jobfile.fio>                Run FIO job file\n"
           "  BFFIO --help                       Show this help\n"
           "\n"
           "CLI Options:\n"
           "  --name=NAME            Job name (default: \"BFFIO\")\n"
           "  --rw=PATTERN           I/O pattern: read, write, randread, randwrite, readwrite, "
           "randrw\n"
           "  --bs=SIZE              Block size (default: 4K). Suffixes: K, M, G\n"
           "  --size=SIZE            File size. Suffixes: K, M, G\n"
           "  --filename=PATH        Existing file or block device\n"
           "  --directory=PATH       Directory for auto-created test files\n"
           "  --direct=0|1           Use O_DIRECT (default: 0)\n"
           "  --runtime=SECONDS      Run duration in seconds\n"
           "  --time_based           Run for fixed time (requires --runtime)\n"
           "  --ramp_time=SECONDS    Warmup period (stats discarded)\n"
           "  --numjobs=N            Number of worker threads (default: 1)\n"
           "  --iodepth=N            Max queue depth cap (default: 256, AuraIO auto-tunes below "
           "this)\n"
           "  --nrfiles=N            Number of files per job (default: 1, directory mode only)\n"
           "  --filesize=SIZE        Per-file size (default: size/nrfiles). Suffixes: K, M, G\n"
           "  --file_service_type=T  File selection: roundrobin (default), sequential, random\n"
           "  --rwmixread=N          Read percentage for mixed workloads (default: 50)\n"
           "  --group_reporting      Aggregate stats across threads\n"
           "  --fsync=N              fsync every N writes (default: 0)\n"
           "  --target-p99=LATENCY   P99 latency ceiling. Suffixes: us, ms, s (default: ms)\n"
           "                         AIMD finds max concurrency under this target.\n"
           "                         Examples: 2ms, 500us, 1.5ms, 0.01s\n"
           "  --ring-select=MODE     Ring selection: adaptive, cpu_local, round_robin (default: "
           "adaptive)\n"
           "  --ioengine=ENGINE      Accepted but always uses AuraIO\n"
           "  --output-format=FMT    Output format: normal, json (default: normal)\n");
}

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

/** Check whether a flag (e.g. "--help") appears anywhere in argv. */
static bool has_flag(char **argv, int argc, const char *flag) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

/**
 * Check whether the first argument looks like a .fio job file.
 * Accepts anything ending in ".fio" or an existing path that does not
 * start with '-'.
 */
static bool is_job_file_arg(const char *arg) {
    if (arg[0] == '-') return false;

    const char *dot = strrchr(arg, '.');
    if (dot && strcmp(dot, ".fio") == 0) return true;

    /* Also accept a plain path that doesn't start with '-' as a potential
       job file -- job_parse_file will report the real error if it fails. */
    return true;
}

/**
 * Scan remaining argv (after job file) for --output-format=json.
 * Returns the detected format.
 */
static output_format_t scan_output_format(char **argv, int argc, int start) {
    for (int i = start; i < argc; i++) {
        if (strncmp(argv[i], "--output-format=", 16) == 0) {
            if (strcmp(argv[i] + 16, "json") == 0) return OUTPUT_JSON;
        }
    }
    return OUTPUT_NORMAL;
}

/* -------------------------------------------------------------------------- */
/* main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    /* 1. Check for --help or no arguments */
    if (argc < 2 || has_flag(argv, argc, "--help") || has_flag(argv, argc, "-h")) {
        print_usage();
        return (argc < 2) ? 1 : 0;
    }

    /* 2. Parse configuration */
    bench_config_t bench;
    memset(&bench, 0, sizeof(bench));
    output_format_t format = OUTPUT_NORMAL;

    if (is_job_file_arg(argv[1])) {
        /* First non-flag arg looks like a job file */
        if (job_parse_file(argv[1], &bench) != 0) {
            fprintf(stderr, "Error: failed to parse job file '%s'\n", argv[1]);
            return 1;
        }
        /* Check remaining args for --output-format */
        format = scan_output_format(argv, argc, 2);
    } else {
        /* Parse CLI arguments */
        if (job_parse_cli(argc, argv, &bench) != 0) {
            fprintf(stderr, "Error: failed to parse arguments\n");
            return 1;
        }
        format = bench.output_format;
    }

    /* 3. Normalize and validate all jobs */
    for (int i = 0; i < bench.num_jobs; i++) {
        job_config_normalize(&bench.jobs[i]);
        if (job_config_validate(&bench.jobs[i]) != 0) {
            bench_config_free(&bench);
            return 1;
        }
    }

    /* 4. Allocate results array */
    job_result_t *results = calloc((size_t)bench.num_jobs, sizeof(job_result_t));
    if (!results) {
        fprintf(stderr, "Error: out of memory\n");
        bench_config_free(&bench);
        return 1;
    }

    /* 5. Execute each job sequentially (one engine per job for clean
     *    AIMD convergence) */
    for (int i = 0; i < bench.num_jobs; i++) {
        job_config_t *job = &bench.jobs[i];

        /* Auto ramp_time: AIMD needs convergence time when targeting P99 */
        if (job->target_p99_ms > 0.0 && job->ramp_time_sec == 0) {
            job->ramp_time_sec = 5;
        }

        /* Create fresh AuraIO engine per job */
        auraio_options_t opts;
        auraio_options_init(&opts);
        /* Engine capacity must exceed BFFIO's submission depth so AIMD
         * has headroom to set its inflight limit below what we submit. */
        int engine_depth = job->iodepth * 2;
        if (engine_depth < 256) engine_depth = 256;
        opts.queue_depth = engine_depth;
        opts.ring_count = 0; /* auto-detect cores */
        opts.ring_select = (auraio_ring_select_t)job->ring_select;

        /* Pass user's P99 latency target to AIMD controller */
        if (job->target_p99_ms > 0.0) {
            opts.max_p99_latency_ms = job->target_p99_ms;
            opts.initial_in_flight = 4; /* Start low, let AIMD probe up */
        } else {
            /* Benchmark mode: start at full depth, skip AIMD ramp-up */
            opts.initial_in_flight = engine_depth;
        }

        auraio_engine_t *engine = auraio_create_with_options(&opts);
        if (!engine) {
            fprintf(stderr, "Error: failed to create AuraIO engine for job '%s'\n", job->name);
            free(results);
            bench_config_free(&bench);
            return 1;
        }

        /* Target-p99 mode: auto-scale numjobs to drive all rings.
         * Each ring has its own AIMD controller; we need a thread per ring
         * so every ring sees real I/O and converges to the right depth.
         * The sum of per-ring depths = total device concurrency. */
        if (job->target_p99_ms > 0.0 && job->numjobs == 1) {
            int ring_count = auraio_get_ring_count(engine);
            if (ring_count > 1) {
                job->numjobs = ring_count;
                job->group_reporting = 1;
            }
        }

        /* Allocate per-thread stats */
        thread_stats_t *thread_stats = calloc((size_t)job->numjobs, sizeof(thread_stats_t));
        if (!thread_stats) {
            fprintf(stderr, "Error: out of memory for thread stats\n");
            auraio_destroy(engine);
            free(results);
            bench_config_free(&bench);
            return 1;
        }

        for (int t = 0; t < job->numjobs; t++) stats_init(&thread_stats[t]);

        /* Run workload */
        uint64_t runtime_ms = 0;
        int ret = workload_run(job, engine, thread_stats, &runtime_ms);

        if (ret != 0) {
            fprintf(stderr, "Warning: job '%s' completed with errors\n", job->name);
        }

        /* Compute results.
         * If group_reporting, aggregate all thread stats into a single set. */
        if (job->group_reporting && job->numjobs > 1) {
            thread_stats_t aggregated;
            stats_init(&aggregated);
            stats_aggregate(thread_stats, job->numjobs, &aggregated);
            stats_compute_results(&aggregated, runtime_ms, engine, job->name, &results[i]);
        } else {
            stats_compute_results(&thread_stats[0], runtime_ms, engine, job->name, &results[i]);
        }

        /* Copy target P99 constraint into result for output */
        results[i].target_p99_ms = job->target_p99_ms;

        /* Normal mode: print results per job as they complete */
        if (format == OUTPUT_NORMAL) {
            output_normal(&results[i], 1, &bench, stdout);
        }

        /* Cleanup per-job resources */
        free(thread_stats);
        auraio_destroy(engine);
    }

    /* JSON mode: output all jobs at once at the end */
    if (format == OUTPUT_JSON) {
        output_json(results, bench.num_jobs, &bench, stdout);
    }

    /* Cleanup */
    free(results);
    bench_config_free(&bench);
    return 0;
}
