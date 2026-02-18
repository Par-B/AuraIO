// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file output.c
 * @brief FIO-compatible text and JSON output formatters for BFFIO
 *
 * Produces output matching FIO 3.36 format so existing dashboards,
 * fio-plot, and automation scripts work unchanged. Two modes:
 *
 *   output_normal() - FIO-style text summary to stdout
 *   output_json()   - Full FIO 3.36-compatible JSON structure
 *
 * Both include an extra "Aura" section with adaptive tuning info
 * (converged depth, phase, p99) that FIO parsers safely ignore.
 */

#define _GNU_SOURCE /* clock_gettime */

#include "output.h"

#include <string.h>
#include <time.h>
#include <inttypes.h>

/* ========================================================================
 * Internal helpers
 * ======================================================================== */

/**
 * Write a JSON-escaped string to the given FILE.
 * Escapes: \, ", control characters (\n, \r, \t, and \uXXXX for others).
 */
static void json_escape_string(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (c < 0x20) {
                fprintf(out, "\\u%04x", c);
            } else {
                fputc(c, out);
            }
            break;
        }
    }
    fputc('"', out);
}

/**
 * Pick latency unit based on nanosecond magnitude.
 *   < 10,000 ns (10 us)   -> "nsec"
 *   < 10,000,000 ns (10 ms) -> "usec"
 *   otherwise              -> "msec"
 */
static const char *pick_lat_unit(uint64_t ns) {
    if (ns < 10000ULL) {
        return "nsec";
    } else if (ns < 10000000ULL) {
        return "usec";
    } else {
        return "msec";
    }
}

/**
 * Convert nanoseconds to the given unit.
 */
static double lat_convert(uint64_t ns, const char *unit) {
    if (strcmp(unit, "nsec") == 0) {
        return (double)ns;
    } else if (strcmp(unit, "usec") == 0) {
        return (double)ns / 1000.0;
    } else {
        /* msec */
        return (double)ns / 1000000.0;
    }
}

/**
 * Format a byte count as KiB, MiB, or GiB with appropriate precision.
 */
static void format_size(uint64_t bytes, char *buf, size_t buflen) {
    double gib = (double)bytes / (1024.0 * 1024.0 * 1024.0);
    double mib = (double)bytes / (1024.0 * 1024.0);
    double kib = (double)bytes / 1024.0;

    if (gib >= 1.0) {
        snprintf(buf, buflen, "%.1fGiB", gib);
    } else if (mib >= 1.0) {
        snprintf(buf, buflen, "%.1fMiB", mib);
    } else {
        snprintf(buf, buflen, "%.1fKiB", kib);
    }
}

/**
 * Format IOPS with k/M suffix for readability.
 *   < 1000      -> "999"
 *   < 1000000   -> "142k"
 *   otherwise   -> "1.42M"
 */
static void format_iops(double iops, char *buf, size_t buflen) {
    if (iops >= 1000000.0) {
        snprintf(buf, buflen, "%.2fM", iops / 1000000.0);
    } else if (iops >= 1000.0) {
        snprintf(buf, buflen, "%.0fk", iops / 1000.0);
    } else {
        snprintf(buf, buflen, "%.0f", iops);
    }
}

/**
 * Format bandwidth as "XXXMiB/s (YYYmb/s)" or "XXXKiB/s (YYYkB/s)".
 * Shows both binary (MiB) and decimal (MB) units like FIO does.
 */
static void format_bw(int64_t bw_bytes_sec, char *buf, size_t buflen) {
    double mib_s = (double)bw_bytes_sec / (1024.0 * 1024.0);
    double mb_s = (double)bw_bytes_sec / (1000.0 * 1000.0);
    double kib_s = (double)bw_bytes_sec / 1024.0;
    double kb_s = (double)bw_bytes_sec / 1000.0;

    if (mib_s >= 1.0) {
        snprintf(buf, buflen, "%.0fMiB/s (%.0fMB/s)", mib_s, mb_s);
    } else {
        snprintf(buf, buflen, "%.0fKiB/s (%.0fkB/s)", kib_s, kb_s);
    }
}

/**
 * Print the clat percentile table for one direction.
 *
 * FIO format: 4 percentiles per line, right-aligned in brackets.
 * Unit is chosen based on the median (P50) value.
 *
 * Example:
 *     clat percentiles (usec):
 *      |  1.00th=[   42],  5.00th=[   45], 10.00th=[   46], 20.00th=[   47],
 *      | 30.00th=[   48], 40.00th=[   49], 50.00th=[   50], 60.00th=[   51],
 *      ...
 */
static void print_percentiles(const direction_result_t *r, FILE *out) {
    /* P50 is index 6 in our 17-element array */
    uint64_t median_ns = r->lat_percentiles_ns[6];
    const char *unit = pick_lat_unit(median_ns);

    fprintf(out, "    clat percentiles (%s):\n", unit);

    /* Percentile labels matching FIO's format */
    static const char *pct_labels[NUM_PERCENTILES] = { " 1.00th", " 5.00th", "10.00th", "20.00th",
                                                       "30.00th", "40.00th", "50.00th", "60.00th",
                                                       "70.00th", "80.00th", "90.00th", "95.00th",
                                                       "99.00th", "99.50th", "99.90th", "99.95th",
                                                       "99.99th" };

    for (int i = 0; i < NUM_PERCENTILES; i++) {
        double val = lat_convert(r->lat_percentiles_ns[i], unit);

        /* Start of line: 4 per line */
        if (i % 4 == 0) {
            fprintf(out, "     |");
        }

        /*
         * Print formatted: right-aligned integer in brackets.
         * FIO uses a field width that accommodates the largest value in
         * the set.  We use width 5 for usec/nsec (values typically < 99999)
         * and width 7 for msec (values can be larger).
         */
        if (strcmp(unit, "msec") == 0) {
            fprintf(out, " %s=[%7.0f]", pct_labels[i], val);
        } else {
            fprintf(out, " %s=[%5.0f]", pct_labels[i], val);
        }

        /* End of line or comma */
        if (i == NUM_PERCENTILES - 1) {
            fprintf(out, "\n");
        } else if (i % 4 == 3) {
            fprintf(out, ",\n");
        } else {
            fprintf(out, ",");
        }
    }
}

/**
 * Print one direction's stats in FIO text format.
 */
static void print_direction_normal(const direction_result_t *r, const char *name, FILE *out) {
    if (r->total_ios == 0) {
        return;
    }

    /* IOPS and BW summary line */
    char iops_buf[32];
    char bw_buf[64];
    char size_buf[32];

    format_iops(r->iops, iops_buf, sizeof(iops_buf));
    format_bw(r->bw_bytes_sec, bw_buf, sizeof(bw_buf));
    format_size(r->io_bytes, size_buf, sizeof(size_buf));

    fprintf(out, "  %s: IOPS=%s, BW=%s(%s/%" PRId64 "msec)\n", name, iops_buf, bw_buf, size_buf,
            r->runtime_ms);

    /* Latency summary line */
    const char *lat_unit = pick_lat_unit(r->lat_percentiles_ns[6]);
    double lat_min = lat_convert(r->lat_min_ns, lat_unit);
    double lat_max = lat_convert(r->lat_max_ns, lat_unit);
    double lat_avg = lat_convert((uint64_t)r->lat_mean_ns, lat_unit);
    double lat_dev = lat_convert((uint64_t)r->lat_stddev_ns, lat_unit);

    fprintf(out, "    lat (%s): min=%.0f, max=%.0f, avg=%.2f, stdev=%.2f\n", lat_unit, lat_min,
            lat_max, lat_avg, lat_dev);

    /* Clat percentiles */
    print_percentiles(r, out);

    /* BW sample stats */
    fprintf(out,
            "   bw (  KiB/s): min=%" PRId64 ", max=%" PRId64
            ", per=100.00%%, avg=%.2f, stdev=%.2f, samples=%d\n",
            r->bw_min, r->bw_max, r->bw_mean, r->bw_dev, r->bw_samples);

    /* IOPS sample stats */
    fprintf(out,
            "   iops        : min=%" PRId64 ", max=%" PRId64 ", avg=%.2f, stdev=%.2f, samples=%d\n",
            r->iops_min, r->iops_max, r->iops_mean, r->iops_stddev, r->iops_samples);
}

/* ========================================================================
 * Normal (text) output
 * ======================================================================== */

void output_normal(const job_result_t *results, int num_jobs, const bench_config_t *config,
                   FILE *out) {
    for (int j = 0; j < num_jobs; j++) {
        const job_result_t *r = &results[j];
        const job_config_t *job = &config->jobs[j];

        /* Job header line */
        if (job->rw == RW_READWRITE || job->rw == RW_RANDRW) {
            fprintf(out,
                    "%s: (g=0): rw=%s, bs=(R) %" PRIu64 "B-(W) %" PRIu64
                    "B, ioengine=Aura, direct=%d\n",
                    r->jobname, rw_pattern_name(job->rw), job->bs, job->bs, job->direct);
        } else {
            fprintf(out, "%s: (g=0): rw=%s, bs=(R) %" PRIu64 "B, ioengine=Aura, direct=%d\n",
                    r->jobname, rw_pattern_name(job->rw), job->bs, job->direct);
        }

        /* Read direction */
        print_direction_normal(&r->read, "read", out);

        /* Write direction */
        print_direction_normal(&r->write, "write", out);

        /* Blank line before Aura info */
        fprintf(out, "\n");

        /* Aura adaptive tuning info */
        if (r->target_p99_ms > 0.0) {
            fprintf(out,
                    "  Aura: max concurrency=%d at p99=%.2fms"
                    " (target: %.2fms), phase=%s\n",
                    r->aura_final_depth, r->aura_p99_ms, r->target_p99_ms,
                    r->aura_phase_name);
        } else {
            fprintf(out,
                    "  Aura: converged to depth %d, phase=%s,"
                    " p99=%.2fms, spills=%" PRIu64 "\n",
                    r->aura_final_depth, r->aura_phase_name, r->aura_p99_ms,
                    r->adaptive_spills);
        }
    }
}

/* ========================================================================
 * JSON helpers
 * ======================================================================== */

/**
 * Emit a lat_ns-style JSON object with min/max/mean/stddev/N.
 * Optionally includes a percentile sub-object.
 *
 * The opening '{' is printed inline (no leading indent) since the caller
 * has already printed the key name on the same line. The @p indent param
 * is used for the inner fields and the closing '}'.
 */
static void json_lat_ns(FILE *out, const char *indent, uint64_t min_ns, uint64_t max_ns,
                        double mean_ns, double stddev_ns, uint64_t count,
                        const uint64_t *percentiles_ns, int include_percentiles) {
    fprintf(out, "{\n");
    fprintf(out, "%s  \"min\" : %" PRIu64 ",\n", indent, min_ns);
    fprintf(out, "%s  \"max\" : %" PRIu64 ",\n", indent, max_ns);
    fprintf(out, "%s  \"mean\" : %.6f,\n", indent, mean_ns);
    fprintf(out, "%s  \"stddev\" : %.6f,\n", indent, stddev_ns);
    fprintf(out, "%s  \"N\" : %" PRIu64, indent, count);

    if (include_percentiles && percentiles_ns != NULL && count > 0) {
        fprintf(out, ",\n");
        fprintf(out, "%s  \"percentile\" : {\n", indent);

        /* FIO percentile keys: 6 decimal places */
        static const char *pct_keys[NUM_PERCENTILES] = {
            "1.000000",  "5.000000",  "10.000000", "20.000000", "30.000000", "40.000000",
            "50.000000", "60.000000", "70.000000", "80.000000", "90.000000", "95.000000",
            "99.000000", "99.500000", "99.900000", "99.950000", "99.990000"
        };

        for (int i = 0; i < NUM_PERCENTILES; i++) {
            fprintf(out, "%s    \"%s\" : %" PRIu64, indent, pct_keys[i], percentiles_ns[i]);
            if (i < NUM_PERCENTILES - 1) {
                fprintf(out, ",\n");
            } else {
                fprintf(out, "\n");
            }
        }

        fprintf(out, "%s  }\n", indent);
    } else {
        fprintf(out, "\n");
    }

    fprintf(out, "%s}", indent);
}

/**
 * Emit a zeroed lat_ns-style object (no percentiles).
 */
static void json_zeroed_lat_ns(FILE *out, const char *indent) {
    json_lat_ns(out, indent, 0, 0, 0.0, 0.0, 0, NULL, 0);
}

/**
 * Emit one direction's complete JSON block (read or write).
 */
static void json_direction(FILE *out, const direction_result_t *r, const char *indent) {
    /* Build inner indent for nested objects (indent + 2 spaces) */
    char inner[64];
    snprintf(inner, sizeof(inner), "%s  ", indent);

    /* Opening brace is inline after the key, no leading indent */
    fprintf(out, "{\n");

    fprintf(out, "%s  \"io_bytes\" : %" PRIu64 ",\n", indent, r->io_bytes);
    fprintf(out, "%s  \"io_kbytes\" : %" PRIu64 ",\n", indent, r->io_kbytes);
    fprintf(out, "%s  \"bw_bytes\" : %" PRId64 ",\n", indent, r->bw_bytes_sec);
    fprintf(out, "%s  \"bw\" : %" PRId64 ",\n", indent, r->bw_kbytes_sec);
    fprintf(out, "%s  \"iops\" : %.6f,\n", indent, r->iops);
    fprintf(out, "%s  \"runtime\" : %" PRId64 ",\n", indent, r->runtime_ms);
    fprintf(out, "%s  \"total_ios\" : %" PRIu64 ",\n", indent, r->total_ios);
    fprintf(out, "%s  \"short_ios\" : %" PRIu64 ",\n", indent, r->short_ios);
    fprintf(out, "%s  \"drop_ios\" : 0,\n", indent);

    /* slat_ns: zeroed (Aura tracks total latency only) */
    fprintf(out, "%s  \"slat_ns\" : ", indent);
    json_zeroed_lat_ns(out, inner);
    fprintf(out, ",\n");

    /* clat_ns: zeroed (Aura tracks total latency only) */
    fprintf(out, "%s  \"clat_ns\" : ", indent);
    json_zeroed_lat_ns(out, inner);
    fprintf(out, ",\n");

    /* lat_ns: contains all latency data + percentiles */
    fprintf(out, "%s  \"lat_ns\" : ", indent);
    json_lat_ns(out, inner, r->lat_min_ns, r->lat_max_ns, r->lat_mean_ns, r->lat_stddev_ns,
                r->lat_count, r->lat_percentiles_ns, 1);
    fprintf(out, ",\n");

    /* BW sample stats */
    fprintf(out, "%s  \"bw_min\" : %" PRId64 ",\n", indent, r->bw_min);
    fprintf(out, "%s  \"bw_max\" : %" PRId64 ",\n", indent, r->bw_max);
    fprintf(out, "%s  \"bw_agg\" : %.6f,\n", indent, 100.0);
    fprintf(out, "%s  \"bw_mean\" : %.6f,\n", indent, r->bw_mean);
    fprintf(out, "%s  \"bw_dev\" : %.6f,\n", indent, r->bw_dev);
    fprintf(out, "%s  \"bw_samples\" : %d,\n", indent, r->bw_samples);

    /* IOPS sample stats */
    fprintf(out, "%s  \"iops_min\" : %" PRId64 ",\n", indent, r->iops_min);
    fprintf(out, "%s  \"iops_max\" : %" PRId64 ",\n", indent, r->iops_max);
    fprintf(out, "%s  \"iops_mean\" : %.6f,\n", indent, r->iops_mean);
    fprintf(out, "%s  \"iops_stddev\" : %.6f,\n", indent, r->iops_stddev);
    fprintf(out, "%s  \"iops_samples\" : %d\n", indent, r->iops_samples);

    fprintf(out, "%s}", indent);
}

/**
 * Emit a completely zeroed direction block (for inactive directions).
 */
static void json_zeroed_direction(FILE *out, const char *indent) {
    /* Build inner indent for nested objects (indent + 2 spaces) */
    char inner[64];
    snprintf(inner, sizeof(inner), "%s  ", indent);

    /* Opening brace is inline after the key, no leading indent */
    fprintf(out, "{\n");

    fprintf(out, "%s  \"io_bytes\" : 0,\n", indent);
    fprintf(out, "%s  \"io_kbytes\" : 0,\n", indent);
    fprintf(out, "%s  \"bw_bytes\" : 0,\n", indent);
    fprintf(out, "%s  \"bw\" : 0,\n", indent);
    fprintf(out, "%s  \"iops\" : 0.000000,\n", indent);
    fprintf(out, "%s  \"runtime\" : 0,\n", indent);
    fprintf(out, "%s  \"total_ios\" : 0,\n", indent);
    fprintf(out, "%s  \"short_ios\" : 0,\n", indent);
    fprintf(out, "%s  \"drop_ios\" : 0,\n", indent);

    fprintf(out, "%s  \"slat_ns\" : ", indent);
    json_zeroed_lat_ns(out, inner);
    fprintf(out, ",\n");

    fprintf(out, "%s  \"clat_ns\" : ", indent);
    json_zeroed_lat_ns(out, inner);
    fprintf(out, ",\n");

    fprintf(out, "%s  \"lat_ns\" : ", indent);
    json_zeroed_lat_ns(out, inner);
    fprintf(out, ",\n");

    fprintf(out, "%s  \"bw_min\" : 0,\n", indent);
    fprintf(out, "%s  \"bw_max\" : 0,\n", indent);
    fprintf(out, "%s  \"bw_agg\" : 0.000000,\n", indent);
    fprintf(out, "%s  \"bw_mean\" : 0.000000,\n", indent);
    fprintf(out, "%s  \"bw_dev\" : 0.000000,\n", indent);
    fprintf(out, "%s  \"bw_samples\" : 0,\n", indent);

    fprintf(out, "%s  \"iops_min\" : 0,\n", indent);
    fprintf(out, "%s  \"iops_max\" : 0,\n", indent);
    fprintf(out, "%s  \"iops_mean\" : 0.000000,\n", indent);
    fprintf(out, "%s  \"iops_stddev\" : 0.000000,\n", indent);
    fprintf(out, "%s  \"iops_samples\" : 0\n", indent);

    fprintf(out, "%s}", indent);
}

/**
 * Emit the FIO sync section (minimal: just total_ios and lat_ns).
 */
static void json_zeroed_sync(FILE *out, const char *indent) {
    /* Build inner indent for nested objects (indent + 2 spaces) */
    char inner[64];
    snprintf(inner, sizeof(inner), "%s  ", indent);

    /* Opening brace is inline after the key, no leading indent */
    fprintf(out, "{\n");
    fprintf(out, "%s  \"total_ios\" : 0,\n", indent);
    fprintf(out, "%s  \"lat_ns\" : ", indent);
    json_zeroed_lat_ns(out, inner);
    fprintf(out, "\n");
    fprintf(out, "%s}", indent);
}

/**
 * Emit the "job options" JSON object mirroring the job config.
 */
static void json_job_options(FILE *out, const job_config_t *job, const char *indent) {
    char bs_buf[32];
    char size_buf[32];

    /* Format block size with suffix */
    if (job->bs >= 1024 * 1024) {
        snprintf(bs_buf, sizeof(bs_buf), "%" PRIu64 "m", job->bs / (1024 * 1024));
    } else if (job->bs >= 1024) {
        snprintf(bs_buf, sizeof(bs_buf), "%" PRIu64 "k", job->bs / 1024);
    } else {
        snprintf(bs_buf, sizeof(bs_buf), "%" PRIu64, job->bs);
    }

    /* Format file size with suffix */
    if (job->size >= (uint64_t)1024 * 1024 * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%" PRIu64 "G",
                 job->size / ((uint64_t)1024 * 1024 * 1024));
    } else if (job->size >= (uint64_t)1024 * 1024) {
        snprintf(size_buf, sizeof(size_buf), "%" PRIu64 "M", job->size / ((uint64_t)1024 * 1024));
    } else if (job->size >= 1024) {
        snprintf(size_buf, sizeof(size_buf), "%" PRIu64 "K", job->size / 1024);
    } else {
        snprintf(size_buf, sizeof(size_buf), "%" PRIu64, job->size);
    }

    fprintf(out, "\"job options\" : {\n");
    fprintf(out, "%s  \"name\" : ", indent);
    json_escape_string(out, job->name);
    fprintf(out, ",\n");
    fprintf(out, "%s  \"ioengine\" : \"Aura\",\n", indent);
    fprintf(out, "%s  \"rw\" : \"%s\",\n", indent, rw_pattern_name(job->rw));
    fprintf(out, "%s  \"bs\" : \"%s\",\n", indent, bs_buf);
    fprintf(out, "%s  \"direct\" : \"%d\",\n", indent, job->direct);

    if (job->size > 0) {
        fprintf(out, "%s  \"size\" : \"%s\",\n", indent, size_buf);
    }

    fprintf(out, "%s  \"numjobs\" : \"%d\",\n", indent, job->numjobs);
    fprintf(out, "%s  \"iodepth\" : \"%d\"", indent, job->iodepth);

    if (job->nrfiles > 1) {
        fprintf(out, ",\n%s  \"nrfiles\" : \"%d\"", indent, job->nrfiles);
    }
    if (job->filesize > 0) {
        char filesize_buf[32];
        if (job->filesize >= (uint64_t)1024 * 1024 * 1024) {
            snprintf(filesize_buf, sizeof(filesize_buf), "%" PRIu64 "G",
                     job->filesize / ((uint64_t)1024 * 1024 * 1024));
        } else if (job->filesize >= (uint64_t)1024 * 1024) {
            snprintf(filesize_buf, sizeof(filesize_buf), "%" PRIu64 "M",
                     job->filesize / ((uint64_t)1024 * 1024));
        } else if (job->filesize >= 1024) {
            snprintf(filesize_buf, sizeof(filesize_buf), "%" PRIu64 "K", job->filesize / 1024);
        } else {
            snprintf(filesize_buf, sizeof(filesize_buf), "%" PRIu64, job->filesize);
        }
        fprintf(out, ",\n%s  \"filesize\" : \"%s\"", indent, filesize_buf);
    }
    if (job->file_service_type != FST_ROUNDROBIN) {
        fprintf(out, ",\n%s  \"file_service_type\" : \"%s\"", indent,
                file_service_type_name(job->file_service_type));
    }

    if (job->runtime_sec > 0) {
        fprintf(out, ",\n%s  \"runtime\" : \"%d\"", indent, job->runtime_sec);
    }
    if (job->directory[0] != '\0') {
        fprintf(out, ",\n%s  \"directory\" : ", indent);
        json_escape_string(out, job->directory);
    }
    if (job->filename[0] != '\0') {
        fprintf(out, ",\n%s  \"filename\" : ", indent);
        json_escape_string(out, job->filename);
    }

    fprintf(out, "\n");
    fprintf(out, "%s}", indent);
}

/* ========================================================================
 * JSON output
 * ======================================================================== */

void output_json(const job_result_t *results, int num_jobs, const bench_config_t *config,
                 FILE *out) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%c", tm_info);

    /* Millisecond timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t timestamp_ms = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    /* Top-level object */
    fprintf(out, "{\n");
    fprintf(out, "  \"fio version\" : \"BFFIO-1.0 (Aura)\",\n");
    fprintf(out, "  \"timestamp\" : %ld,\n", (long)now);
    fprintf(out, "  \"timestamp_ms\" : %" PRId64 ",\n", timestamp_ms);
    fprintf(out, "  \"time\" : \"%s\",\n", time_str);

    /* Global options */
    fprintf(out, "  \"global options\" : {\n");
    fprintf(out, "    \"direct\" : \"1\",\n");
    fprintf(out, "    \"ioengine\" : \"Aura\"\n");
    fprintf(out, "  },\n");

    /* Jobs array */
    fprintf(out, "  \"jobs\" : [\n");

    for (int j = 0; j < num_jobs; j++) {
        const job_result_t *r = &results[j];
        const job_config_t *job = &config->jobs[j];

        fprintf(out, "    {\n");
        fprintf(out, "      \"jobname\" : ");
        json_escape_string(out, r->jobname);
        fprintf(out, ",\n");
        fprintf(out, "      \"groupid\" : 0,\n");
        fprintf(out, "      \"error\" : 0,\n");

        /* Job options sub-object */
        fprintf(out, "      ");
        json_job_options(out, job, "      ");
        fprintf(out, ",\n");

        /* Read direction */
        fprintf(out, "      \"read\" : ");
        if (r->read.total_ios > 0) {
            json_direction(out, &r->read, "      ");
        } else {
            json_zeroed_direction(out, "      ");
        }
        fprintf(out, ",\n");

        /* Write direction */
        fprintf(out, "      \"write\" : ");
        if (r->write.total_ios > 0) {
            json_direction(out, &r->write, "      ");
        } else {
            json_zeroed_direction(out, "      ");
        }
        fprintf(out, ",\n");

        /* Trim: always zeroed (BFFIO doesn't support trim) */
        fprintf(out, "      \"trim\" : ");
        json_zeroed_direction(out, "      ");
        fprintf(out, ",\n");

        /* Sync: minimal zeroed section matching FIO's format */
        fprintf(out, "      \"sync\" : ");
        json_zeroed_sync(out, "      ");
        fprintf(out, ",\n");

        /* Job runtime and CPU stats */
        fprintf(out, "      \"job_runtime\" : %" PRId64 ",\n", r->job_runtime_ms);
        fprintf(out, "      \"usr_cpu\" : 0.000000,\n");
        fprintf(out, "      \"sys_cpu\" : 0.000000,\n");
        fprintf(out, "      \"ctx\" : 0,\n");
        fprintf(out, "      \"majf\" : 0,\n");
        fprintf(out, "      \"minf\" : 0,\n");

        /* Aura adaptive tuning info (extra section, parsers ignore it) */
        fprintf(out, "      \"Aura\" : {\n");
        if (r->target_p99_ms > 0.0) {
            fprintf(out, "        \"target_p99_ms\" : %.2f,\n", r->target_p99_ms);
            fprintf(out, "        \"max_concurrency\" : %d,\n", r->aura_final_depth);
        }
        fprintf(out, "        \"final_depth\" : %d,\n", r->aura_final_depth);
        fprintf(out, "        \"phase\" : ");
        json_escape_string(out, r->aura_phase_name);
        fprintf(out, ",\n");
        fprintf(out, "        \"p99_ms\" : %.2f,\n", r->aura_p99_ms);
        fprintf(out, "        \"throughput_bps\" : %.0f\n", r->aura_throughput_bps);
        fprintf(out, "      }\n");

        /* Close job object */
        fprintf(out, "    }");
        if (j < num_jobs - 1) {
            fprintf(out, ",");
        }
        fprintf(out, "\n");
    }

    fprintf(out, "  ]\n");
    fprintf(out, "}\n");
}
