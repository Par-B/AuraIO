/**
 * @file auraio_syslog.h
 * @brief Syslog log handler integration for AuraIO
 *
 * Forwards AuraIO library log messages to syslog(3).
 * Standalone module — depends only on the public auraio.h API and libc.
 *
 * AuraIO log levels map directly to syslog priorities:
 *   AURAIO_LOG_ERR    (3) -> LOG_ERR     (3)
 *   AURAIO_LOG_WARN   (4) -> LOG_WARNING (4)
 *   AURAIO_LOG_NOTICE (5) -> LOG_NOTICE  (5)
 *   AURAIO_LOG_INFO   (6) -> LOG_INFO    (6)
 *   AURAIO_LOG_DEBUG  (7) -> LOG_DEBUG   (7)
 *
 * Usage:
 * @code
 *   auraio_syslog_install(NULL);   // defaults: ident="auraio", LOG_USER
 *   auraio_engine_t *e = auraio_create();
 *   auraio_log_emit(AURAIO_LOG_INFO, "engine started");
 *   // ... library and app logs forwarded to syslog ...
 *   auraio_syslog_remove();
 *   auraio_destroy(e);
 * @endcode
 */

#ifndef AURAIO_SYSLOG_H
#define AURAIO_SYSLOG_H

#include <auraio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Integration schema contract. */
#define AURAIO_SYSLOG_SCHEMA_VERSION "v0"
#define AURAIO_SYSLOG_SCHEMA_STABILITY "experimental"

/**
 * Syslog configuration options.
 *
 * Pass NULL to auraio_syslog_install() for sensible defaults.
 * A value of -1 means "use default" for numeric fields.
 */
typedef struct {
    const char *ident; /**< openlog ident (default: "auraio").
                            Must remain valid until auraio_syslog_remove()
                            — POSIX openlog() retains the pointer. */
    int facility;      /**< Syslog facility (default: LOG_USER).
                            -1 means "use default". */
    int log_options;   /**< openlog() option flags
                            (default: LOG_PID | LOG_NDELAY).
                            -1 means "use default". */
} auraio_syslog_options_t;

/**
 * Install a syslog-forwarding log handler.
 *
 * Calls openlog() then registers a log handler that calls syslog().
 * Thread-safe: syslog() is thread-safe per POSIX.
 *
 * @param options  Configuration (NULL for defaults)
 */
void auraio_syslog_install(const auraio_syslog_options_t *options);

/**
 * Remove the syslog log handler and close syslog.
 *
 * Restores the default (no handler) state and calls closelog().
 */
void auraio_syslog_remove(void);

#ifdef __cplusplus
}
#endif

#endif /* AURAIO_SYSLOG_H */
