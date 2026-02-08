/**
 * @file auraio_otel_push.h
 * @brief HTTP push helper for OTLP JSON payloads
 *
 * Simple blocking HTTP POST using POSIX sockets.
 * No TLS, no keepalive, no chunked encoding, no external dependencies.
 *
 * Usage:
 * @code
 *   char buf[131072];
 *   int len = auraio_metrics_otel(engine, buf, sizeof(buf));
 *   if (len > 0) {
 *       int status = auraio_otel_push("http://localhost:4318/v1/metrics", buf, len);
 *       if (status == 200) { // success }
 *   }
 * @endcode
 */

#ifndef AURAIO_OTEL_PUSH_H
#define AURAIO_OTEL_PUSH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Push OTLP JSON payload to a collector endpoint
 *
 * @param endpoint URL: "http://host:port/path" (plain HTTP only)
 * @param buf      OTLP JSON from auraio_metrics_otel()
 * @param len      Length of buf in bytes
 * @return HTTP status code (200 = success), or -1 on connection/IO error
 */
int auraio_otel_push(const char *endpoint, const char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* AURAIO_OTEL_PUSH_H */
