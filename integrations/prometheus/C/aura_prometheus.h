/**
 * @file aura_prometheus.h
 * @brief Prometheus exposition text formatter for AuraIO metrics
 *
 * Standalone tool that depends only on the public aura.h API.
 * No external dependencies beyond libc (uses snprintf internally).
 *
 * Usage:
 * @code
 *   char buf[65536];
 *   int len = aura_metrics_prometheus(engine, buf, sizeof(buf));
 *   if (len < 0) {
 *       // if errno==ENOBUFS: buffer too small — retry with abs(len) bytes
 *       // if errno==ENOMEM: hard error (allocation failure)
 *   }
 *   // write buf to HTTP response, file, etc.
 * @endcode
 */

#ifndef AURA_PROMETHEUS_H
#define AURA_PROMETHEUS_H

#include <aura.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prometheus schema contract.
 * Before 1.0 this may evolve; the emitted schema info metric reports status. */
#define AURA_PROMETHEUS_SCHEMA_VERSION "v0"
#define AURA_PROMETHEUS_SCHEMA_STABILITY "experimental"

/**
 * Format all AuraIO metrics in Prometheus exposition text format
 *
 * Outputs TYPE and HELP annotations, counter/gauge/histogram metrics
 * with per-ring labels. The output is a valid Prometheus text exposition
 * blob suitable for serving on a /metrics endpoint.
 *
 * Recommended buffer size: 16KB + 12KB per ring.
 *
 * @param engine   AuraIO engine handle
 * @param buf      Output buffer
 * @param buf_size Size of output buffer in bytes
 * @return Number of bytes written (excluding null terminator) on success;
 *         negative value with errno=ENOBUFS if buffer too small
 *         (abs value = minimum estimate; callers should retry in a loop);
 *         or -1 with errno set on hard failure (e.g., ENOMEM)
 */
int aura_metrics_prometheus(aura_engine_t *engine, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* AURA_PROMETHEUS_H */
