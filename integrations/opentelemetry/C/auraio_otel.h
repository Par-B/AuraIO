/**
 * @file auraio_otel.h
 * @brief OpenTelemetry OTLP/JSON metrics formatter for AuraIO
 *
 * Standalone tool that depends only on the public auraio.h API.
 * No external dependencies beyond libc (uses snprintf internally).
 *
 * Usage:
 * @code
 *   char buf[131072];
 *   int len = auraio_metrics_otel(engine, buf, sizeof(buf));
 *   if (len < 0) {
 *       // if errno==ENOBUFS: buffer too small — retry with abs(len) bytes
 *       // if errno==ENOMEM: hard error (allocation failure)
 *   }
 *   // POST buf to OTel collector /v1/metrics endpoint
 * @endcode
 */

#ifndef AURAIO_OTEL_H
#define AURAIO_OTEL_H

#include <auraio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* OTLP schema contract.
 * Before 1.0 this may evolve; consumers should check schema version. */
#define AURAIO_OTEL_SCHEMA_VERSION "v0"
#define AURAIO_OTEL_SCHEMA_STABILITY "experimental"

/**
 * Format all AuraIO metrics as OTLP JSON (ExportMetricsServiceRequest)
 *
 * Outputs a complete OTLP JSON blob containing resource metrics, scope
 * metrics, and all AuraIO counters/gauges/histograms mapped to OTLP types.
 *
 * Recommended buffer size: 32KB + 24KB per ring.
 *
 * @param engine   AuraIO engine handle
 * @param buf      Output buffer
 * @param buf_size Size of output buffer in bytes
 * @return Number of bytes written (excluding null terminator) on success;
 *         negative value with errno=ENOBUFS if buffer too small
 *         (abs value = minimum estimate; callers should retry in a loop);
 *         or -1 with errno set on hard failure (e.g., ENOMEM)
 */
int auraio_metrics_otel(auraio_engine_t *engine, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* AURAIO_OTEL_H */
