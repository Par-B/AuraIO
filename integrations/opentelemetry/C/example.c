// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file example.c
 * @brief Example: AuraIO OpenTelemetry OTLP/JSON metrics push
 *
 * Demonstrates periodic metrics push to an OTel collector.
 *
 * Build:
 *   make -C /path/to/AuraIO exporters
 *
 * Usage:
 *   ./otel_example                          # Push to localhost:4318
 *   ./otel_example --dry-run                # Format and print to stdout
 *   ./otel_example --endpoint http://host:4318/v1/metrics
 */

#include <aura.h>
#include "aura_otel.h"
#include "aura_otel_push.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DEFAULT_ENDPOINT "http://localhost:4318/v1/metrics"
#define METRICS_BUF_SIZE (256 * 1024)
#define PUSH_INTERVAL_SEC 10

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

int main(int argc, char *argv[]) {
    const char *endpoint = DEFAULT_ENDPOINT;
    int dry_run = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc) {
            endpoint = argv[++i];
        } else if (strncmp(argv[i], "--endpoint=", 11) == 0) {
            endpoint = argv[i] + 11;
        } else {
            fprintf(stderr, "Usage: %s [--dry-run] [--endpoint URL]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    /* Create a minimal AuraIO engine for demonstration */
    aura_options_t opts;
    aura_options_init(&opts);
    opts.ring_count = 1;
    opts.queue_depth = 64;

    aura_engine_t *engine = aura_create_with_options(&opts);
    if (!engine) {
        fprintf(stderr, "Failed to create AuraIO engine: %s\n", strerror(errno));
        return 1;
    }

    printf("AuraIO OpenTelemetry Exporter (v%s)\n", aura_version());
    printf("Engine: %d ring(s), queue_depth=%d\n", aura_get_ring_count(engine), opts.queue_depth);

    if (dry_run) {
        printf("Mode: dry-run (stdout)\n\n");
    } else {
        printf("Endpoint: %s\n", endpoint);
        printf("Push interval: %ds\n\n", PUSH_INTERVAL_SEC);
    }

    char *buf = malloc(METRICS_BUF_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate metrics buffer\n");
        aura_destroy(engine);
        return 1;
    }

    while (running) {
        int len = aura_metrics_otel(engine, buf, METRICS_BUF_SIZE);
        if (len < 0) {
            fprintf(stderr, "Failed to format metrics: %s\n",
                    errno == ENOBUFS ? "buffer too small" : strerror(errno));
        } else if (dry_run) {
            printf("%.*s\n", len, buf);
            break;
        } else {
            int status = aura_otel_push(endpoint, buf, (size_t)len);
            if (status == 200) {
                printf("Pushed %d bytes -> HTTP %d OK\n", len, status);
            } else {
                fprintf(stderr, "Push failed: %s (status=%d)\n",
                        status < 0 ? "connection error" : "HTTP error", status);
            }
        }

        if (!dry_run) {
            for (int s = 0; s < PUSH_INTERVAL_SEC && running; s++) {
                sleep(1);
            }
        }
    }

    free(buf);
    printf("\nShutting down...\n");
    aura_destroy(engine);
    return 0;
}
