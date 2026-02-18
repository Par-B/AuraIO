// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file job_parser.c
 * @brief FIO-compatible job file and CLI parser for BFFIO
 *
 * Parses .fio INI-style job files and --key=value CLI arguments into
 * a bench_config_t that drives the benchmark workloads.
 *
 * INI format:
 *   [global]        - defaults applied to all subsequent jobs
 *   [jobname]       - job section inheriting from global
 *   key=value       - parameter assignment
 *   ; comment       - semicolon comments
 *   # comment       - hash comments
 */

#include "job_parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ============================================================================
 * Constants
 * ============================================================================ */

#define INITIAL_JOBS_CAPACITY 8
#define LINE_MAX_LEN 1024

#include <errno.h>
#include <limits.h>

/**
 * Parse an integer value with validation.
 * Returns 0 on success, -1 on error (not a number, overflow, trailing junk).
 */
static int parse_int(const char *s, int *out) {
    if (!s || !*s) return -1;
    char *end;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    if (val < INT_MIN || val > INT_MAX) return -1;
    *out = (int)val;
    return 0;
}

/* ============================================================================
 * Utility Helpers
 * ============================================================================ */

/**
 * Strip leading and trailing whitespace in-place.
 * Returns pointer into the same buffer (not a new allocation).
 */
static char *strip(char *s) {
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    return s;
}

/* ============================================================================
 * Size Parsing
 * ============================================================================ */

uint64_t parse_size(const char *str) {
    if (!str || !*str) return 0;

    char *endp = NULL;
    unsigned long long val = strtoull(str, &endp, 10);
    if (endp == str) return 0;

    if (endp && *endp) {
        switch (tolower((unsigned char)*endp)) {
        case 'k':
            val *= 1024ULL;
            break;
        case 'm':
            val *= 1048576ULL;
            break;
        case 'g':
            val *= 1073741824ULL;
            break;
        default:
            return 0;
        }
    }

    return (uint64_t)val;
}

/* ============================================================================
 * Latency Parsing
 * ============================================================================ */

double parse_latency(const char *str) {
    if (!str || !*str) return -1.0;

    char *endp = NULL;
    double val = strtod(str, &endp);
    if (endp == str || val < 0.0) return -1.0;

    if (endp && *endp) {
        if (strncasecmp(endp, "us", 2) == 0) {
            val /= 1000.0; /* microseconds → milliseconds */
        } else if (strncasecmp(endp, "ms", 2) == 0) {
            /* already milliseconds */
        } else if (tolower((unsigned char)*endp) == 's') {
            val *= 1000.0; /* seconds → milliseconds */
        } else {
            return -1.0;
        }
    }
    /* bare number = milliseconds */

    return val;
}

/* ============================================================================
 * RW Pattern Parsing
 * ============================================================================ */

static const struct {
    const char *name;
    rw_pattern_t pattern;
} rw_table[] = {
    { "read", RW_READ },           { "write", RW_WRITE },         { "randread", RW_RANDREAD },
    { "randwrite", RW_RANDWRITE }, { "readwrite", RW_READWRITE }, { "rw", RW_READWRITE },
    { "randrw", RW_RANDRW },
};

#define RW_TABLE_SIZE (sizeof(rw_table) / sizeof(rw_table[0]))

int parse_rw(const char *str, rw_pattern_t *out) {
    if (!str || !out) return -1;

    for (size_t i = 0; i < RW_TABLE_SIZE; i++) {
        if (strncasecmp(str, rw_table[i].name, strlen(rw_table[i].name) + 1) == 0) {
            *out = rw_table[i].pattern;
            return 0;
        }
    }
    return -1;
}

const char *rw_pattern_name(rw_pattern_t rw) {
    switch (rw) {
    case RW_READ:
        return "read";
    case RW_WRITE:
        return "write";
    case RW_RANDREAD:
        return "randread";
    case RW_RANDWRITE:
        return "randwrite";
    case RW_READWRITE:
        return "readwrite";
    case RW_RANDRW:
        return "randrw";
    }
    return "unknown";
}

/* ============================================================================
 * Job Config Defaults
 * ============================================================================ */

void job_config_defaults(job_config_t *job) {
    memset(job, 0, sizeof(*job));
    snprintf(job->name, sizeof(job->name), "default");
    job->bs = 4096;
    job->direct = 0;
    job->numjobs = 1;
    job->iodepth = 256;
    job->nrfiles = 1;
    job->file_service_type = FST_ROUNDROBIN;
    job->rwmixread = 50;
    job->rw_set = false;
}

/* ============================================================================
 * Bench Config Management
 * ============================================================================ */

/**
 * Ensure the jobs array has room for at least one more entry.
 * Doubles capacity when full. Returns 0 on success, -1 on alloc failure.
 */
static int bench_config_grow(bench_config_t *config) {
    if (config->num_jobs < config->jobs_capacity) return 0;

    int new_cap = config->jobs_capacity * 2;
    if (new_cap < INITIAL_JOBS_CAPACITY) new_cap = INITIAL_JOBS_CAPACITY;

    job_config_t *new_jobs = realloc(config->jobs, (size_t)new_cap * sizeof(job_config_t));
    if (!new_jobs) {
        fprintf(stderr, "BFFIO: out of memory allocating jobs\n");
        return -1;
    }

    config->jobs = new_jobs;
    config->jobs_capacity = new_cap;
    return 0;
}

void bench_config_free(bench_config_t *config) {
    if (!config) return;
    free(config->jobs);
    config->jobs = NULL;
    config->num_jobs = 0;
    config->jobs_capacity = 0;
}

/* ============================================================================
 * Apply Key=Value to Job Config
 * ============================================================================ */

/**
 * Apply a single key=value parameter to a job config.
 * Returns 0 on success, 1 if the key is unknown (warning printed), -1 on error.
 */
static int apply_param(job_config_t *job, const char *key, const char *value) {
    /* ioengine: accept and ignore (always Aura) */
    if (strncasecmp(key, "ioengine", 9) == 0) {
        return 0;
    }

    if (strcmp(key, "rw") == 0 || strcmp(key, "readwrite") == 0) {
        if (parse_rw(value, &job->rw) < 0) {
            fprintf(stderr, "BFFIO: unknown rw pattern '%s'\n", value);
            return -1;
        }
        job->rw_set = true;
        return 0;
    }

    if (strcmp(key, "bs") == 0 || strcmp(key, "blocksize") == 0) {
        uint64_t sz = parse_size(value);
        if (sz == 0) {
            fprintf(stderr, "BFFIO: invalid block size '%s'\n", value);
            return -1;
        }
        job->bs = sz;
        return 0;
    }

    if (strcmp(key, "size") == 0) {
        uint64_t sz = parse_size(value);
        if (sz == 0) {
            fprintf(stderr, "BFFIO: invalid size '%s'\n", value);
            return -1;
        }
        job->size = sz;
        return 0;
    }

    if (strcmp(key, "filename") == 0) {
        snprintf(job->filename, sizeof(job->filename), "%s", value);
        return 0;
    }

    if (strcmp(key, "directory") == 0) {
        snprintf(job->directory, sizeof(job->directory), "%s", value);
        return 0;
    }

    if (strcmp(key, "direct") == 0) {
        int v;
        if (parse_int(value, &v) != 0) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->direct = v;
        return 0;
    }

    if (strcmp(key, "runtime") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 0) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->runtime_sec = v;
        return 0;
    }

    if (strcmp(key, "time_based") == 0) {
        /* bare flag or explicit value */
        if (!value || !*value) {
            job->time_based = 1;
        } else {
            int v;
            if (parse_int(value, &v) != 0) {
                fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
                return -1;
            }
            job->time_based = v;
        }
        return 0;
    }

    if (strcmp(key, "ramp_time") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 0) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->ramp_time_sec = v;
        return 0;
    }

    if (strcmp(key, "numjobs") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 1) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->numjobs = v;
        return 0;
    }

    if (strcmp(key, "iodepth") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 1) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->iodepth = v;
        return 0;
    }

    if (strcmp(key, "rwmixread") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 0 || v > 100) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s' (0-100)\n", key, value);
            return -1;
        }
        job->rwmixread = v;
        return 0;
    }

    if (strcmp(key, "group_reporting") == 0) {
        if (!value || !*value) {
            job->group_reporting = 1;
        } else {
            int v;
            if (parse_int(value, &v) != 0) {
                fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
                return -1;
            }
            job->group_reporting = v;
        }
        return 0;
    }

    if (strcmp(key, "fsync") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 0) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->fsync_freq = v;
        return 0;
    }

    if (strcmp(key, "name") == 0) {
        snprintf(job->name, sizeof(job->name), "%s", value);
        return 0;
    }

    if (strcmp(key, "target_p99") == 0 || strcmp(key, "target-p99") == 0) {
        double ms = parse_latency(value);
        if (ms < 0.0) {
            fprintf(stderr, "BFFIO: invalid target-p99 latency '%s'\n", value);
            return -1;
        }
        job->target_p99_ms = ms;
        return 0;
    }

    if (strcmp(key, "nrfiles") == 0) {
        int v;
        if (parse_int(value, &v) != 0 || v < 1) {
            fprintf(stderr, "BFFIO: invalid integer for '%s': '%s'\n", key, value);
            return -1;
        }
        job->nrfiles = v;
        return 0;
    }

    if (strcmp(key, "filesize") == 0) {
        uint64_t sz = parse_size(value);
        if (sz == 0) {
            fprintf(stderr, "BFFIO: invalid filesize '%s'\n", value);
            return -1;
        }
        job->filesize = sz;
        return 0;
    }

    if (strcmp(key, "file_service_type") == 0) {
        if (strcasecmp(value, "roundrobin") == 0) {
            job->file_service_type = FST_ROUNDROBIN;
        } else if (strcasecmp(value, "sequential") == 0) {
            job->file_service_type = FST_SEQUENTIAL;
        } else if (strcasecmp(value, "random") == 0) {
            job->file_service_type = FST_RANDOM;
        } else {
            fprintf(stderr,
                    "BFFIO: unknown file_service_type '%s' "
                    "(use: roundrobin, sequential, random)\n",
                    value);
            return -1;
        }
        return 0;
    }

    if (strcmp(key, "ring_select") == 0 || strcmp(key, "ring-select") == 0) {
        if (strcasecmp(value, "adaptive") == 0) {
            job->ring_select = 0;
        } else if (strcasecmp(value, "cpu_local") == 0) {
            job->ring_select = 1;
        } else if (strcasecmp(value, "round_robin") == 0) {
            job->ring_select = 2;
        } else {
            fprintf(stderr,
                    "BFFIO: unknown ring-select mode '%s' "
                    "(use: adaptive, cpu_local, round_robin)\n",
                    value);
            return -1;
        }
        return 0;
    }

    /* Unknown parameter - warn and skip */
    fprintf(stderr, "BFFIO: warning: unknown parameter '%s', skipping\n", key);
    return 1;
}

/* ============================================================================
 * INI File Parser
 * ============================================================================ */

int job_parse_file(const char *path, bench_config_t *config) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "BFFIO: cannot open job file '%s': ", path);
        perror("");
        return -1;
    }

    /* Global defaults template */
    job_config_t global;
    job_config_defaults(&global);

    /* Current job pointer (NULL while in [global] or before any section) */
    job_config_t *current = NULL;
    bool in_global = false;

    char line_buf[LINE_MAX_LEN];
    int line_num = 0;

    while (fgets(line_buf, sizeof(line_buf), fp)) {
        line_num++;
        char *line = strip(line_buf);

        /* Skip empty lines and comments */
        if (*line == '\0' || *line == '#' || *line == ';') {
            continue;
        }

        /* Section header: [name] */
        if (*line == '[') {
            char *close = strchr(line, ']');
            if (!close) {
                fprintf(stderr, "BFFIO: %s:%d: malformed section header\n", path, line_num);
                fclose(fp);
                return -1;
            }
            *close = '\0';
            char *section_name = strip(line + 1);

            if (strncasecmp(section_name, "global", 7) == 0) {
                in_global = true;
                current = NULL;
                continue;
            }

            /* New job section - inherit from global */
            in_global = false;
            if (bench_config_grow(config) < 0) {
                fclose(fp);
                return -1;
            }

            current = &config->jobs[config->num_jobs];
            *current = global; /* inherit global defaults */
            snprintf(current->name, sizeof(current->name), "%s", section_name);
            config->num_jobs++;
            continue;
        }

        /* Key=value pair */
        char *eq = strchr(line, '=');
        char *key;
        char *value;

        if (eq) {
            *eq = '\0';
            key = strip(line);
            value = strip(eq + 1);
        } else {
            /* Bare flag (e.g., "time_based" or "group_reporting") */
            key = strip(line);
            value = "";
        }

        if (in_global) {
            int rc = apply_param(&global, key, value);
            if (rc < 0) {
                fclose(fp);
                return -1;
            }
        } else if (current) {
            int rc = apply_param(current, key, value);
            if (rc < 0) {
                fclose(fp);
                return -1;
            }
        } else {
            /* Parameters before any section - treat as global */
            int rc = apply_param(&global, key, value);
            if (rc < 0) {
                fclose(fp);
                return -1;
            }
        }
    }

    fclose(fp);
    return 0;
}

/* ============================================================================
 * CLI Parser
 * ============================================================================ */

int job_parse_cli(int argc, char **argv, bench_config_t *config) {
    /* Initialize config */
    memset(config, 0, sizeof(*config));
    config->output_format = OUTPUT_NORMAL;

    /* Global defaults template */
    job_config_t global;
    job_config_defaults(&global);

    /* Current job being configured (NULL until --name or job file creates one) */
    job_config_t *current = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        /* Positional argument: job file (ends in .fio) */
        if (arg[0] != '-') {
            size_t len = strlen(arg);
            if (len >= 4 && strcmp(arg + len - 4, ".fio") == 0) {
                if (job_parse_file(arg, config) < 0) {
                    return -1;
                }
                continue;
            }
            fprintf(stderr, "BFFIO: unexpected argument '%s'\n", arg);
            return -1;
        }

        /* Strip leading dashes */
        const char *param = arg;
        if (param[0] == '-') param++;
        if (param[0] == '-') param++;

        /* Split on '=' if present */
        char key_buf[256];
        const char *key;
        const char *value = NULL;

        const char *eq = strchr(param, '=');
        if (eq) {
            size_t key_len = (size_t)(eq - param);
            if (key_len >= sizeof(key_buf)) key_len = sizeof(key_buf) - 1;
            memcpy(key_buf, param, key_len);
            key_buf[key_len] = '\0';
            key = key_buf;
            value = eq + 1;
        } else {
            key = param;
            /* Check if next arg is the value (not another flag) */
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                value = argv[++i];
            } else {
                value = "";
            }
        }

        /* Handle output format and output file specially */
        if (strcmp(key, "output-format") == 0 || strcmp(key, "output_format") == 0) {
            if (strncasecmp(value, "json", 5) == 0) {
                config->output_format = OUTPUT_JSON;
            } else {
                config->output_format = OUTPUT_NORMAL;
            }
            continue;
        }

        if (strcmp(key, "output") == 0) {
            snprintf(config->output_file, sizeof(config->output_file), "%s", value);
            continue;
        }

        /* --name starts a new job */
        if (strcmp(key, "name") == 0) {
            if (bench_config_grow(config) < 0) return -1;

            current = &config->jobs[config->num_jobs];
            *current = global; /* inherit global defaults */
            snprintf(current->name, sizeof(current->name), "%s", value);
            config->num_jobs++;
            continue;
        }

        /* Apply to current job, or to global if no job started yet */
        if (current) {
            int rc = apply_param(current, key, value);
            if (rc < 0) return -1;
        } else {
            int rc = apply_param(&global, key, value);
            if (rc < 0) return -1;
        }
    }

    /* If no jobs were created but global has rw set, create one implicit job */
    if (config->num_jobs == 0 && global.rw_set) {
        if (bench_config_grow(config) < 0) return -1;
        config->jobs[0] = global;
        snprintf(config->jobs[0].name, sizeof(config->jobs[0].name), "job0");
        config->num_jobs = 1;
    }

    return 0;
}

/* ============================================================================
 * Validation
 * ============================================================================ */

const char *file_service_type_name(file_service_type_t fst) {
    switch (fst) {
    case FST_ROUNDROBIN:
        return "roundrobin";
    case FST_SEQUENTIAL:
        return "sequential";
    case FST_RANDOM:
        return "random";
    }
    return "unknown";
}

void job_config_normalize(job_config_t *job) {
    if (job->nrfiles <= 1) return;

    if (job->filesize > 0) {
        /* filesize explicitly set: size = filesize * nrfiles */
        if (job->filesize > UINT64_MAX / (uint64_t)job->nrfiles) {
            fprintf(stderr, "BFFIO: warning: filesize * nrfiles overflows, clamping\n");
            job->size = UINT64_MAX;
        } else {
            job->size = job->filesize * (uint64_t)job->nrfiles;
        }
    } else if (job->size > 0) {
        /* size set but not filesize: filesize = size / nrfiles */
        job->filesize = job->size / (uint64_t)job->nrfiles;
    }
}

int job_config_validate(const job_config_t *job) {
    if (!job->rw_set) {
        fprintf(stderr, "BFFIO: job '%s': 'rw' parameter is required\n", job->name);
        return -1;
    }

    if (job->size == 0 && !(job->runtime_sec > 0 && job->time_based)) {
        fprintf(stderr,
                "BFFIO: job '%s': must specify 'size' or "
                "'runtime' with 'time_based'\n",
                job->name);
        return -1;
    }

    if (job->filename[0] == '\0' && job->directory[0] == '\0') {
        fprintf(stderr,
                "BFFIO: job '%s': must specify 'filename' or "
                "'directory'\n",
                job->name);
        return -1;
    }

    if (job->numjobs < 1) {
        fprintf(stderr, "BFFIO: job '%s': numjobs must be >= 1\n", job->name);
        return -1;
    }

    if (job->iodepth < 1) {
        fprintf(stderr, "BFFIO: job '%s': iodepth must be >= 1\n", job->name);
        return -1;
    }

    if (job->rwmixread < 0 || job->rwmixread > 100) {
        fprintf(stderr, "BFFIO: job '%s': rwmixread must be 0-100\n", job->name);
        return -1;
    }

    if (job->bs == 0) {
        fprintf(stderr, "BFFIO: job '%s': block size must be > 0\n", job->name);
        return -1;
    }

    if (job->nrfiles > 1 && job->filename[0] != '\0') {
        fprintf(stderr,
                "BFFIO: job '%s': nrfiles > 1 requires --directory "
                "(--filename uses a single explicit file)\n",
                job->name);
        return -1;
    }

    return 0;
}
