#ifndef BFFIO_OUTPUT_H
#define BFFIO_OUTPUT_H

#include "job_parser.h"
#include "stats.h"
#include <stdio.h>

/*
 * Print FIO-compatible normal text output.
 *
 * @param results  Array of job results
 * @param num_jobs Number of results
 * @param config   Bench config (for job parameters in header)
 * @param out      Output stream (stdout or file)
 */
void output_normal(const job_result_t *results, int num_jobs,
                   const bench_config_t *config, FILE *out);

/*
 * Print FIO 3.36-compatible JSON output.
 *
 * Matches the structure in tests/bench_results/fio_randread_4k.json.
 * Includes slat_ns/clat_ns (zeroed) for parser compatibility.
 * Adds "AuraIO" section with adaptive tuning info.
 *
 * @param results  Array of job results
 * @param num_jobs Number of results
 * @param config   Bench config (for job_options mirror)
 * @param out      Output stream (stdout or file)
 */
void output_json(const job_result_t *results, int num_jobs,
                 const bench_config_t *config, FILE *out);

#endif /* BFFIO_OUTPUT_H */
