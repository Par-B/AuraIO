// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file aura_syslog.h
 * @brief Syslog log handler integration for AuraIO
 *
 * Forwards AuraIO library log messages to syslog(3).
 * Standalone module — depends only on the public aura.h API and libc.
 *
 * AuraIO log levels map directly to syslog priorities:
 *   AURA_LOG_ERR    (3) -> LOG_ERR     (3)
 *   AURA_LOG_WARN   (4) -> LOG_WARNING (4)
 *   AURA_LOG_NOTICE (5) -> LOG_NOTICE  (5)
 *   AURA_LOG_INFO   (6) -> LOG_INFO    (6)
 *   AURA_LOG_DEBUG  (7) -> LOG_DEBUG   (7)
 *
 * Usage:
 * @code
 *   aura_syslog_install(NULL);   // defaults: ident="aura", LOG_USER
 *   aura_engine_t *e = aura_create();
 *   aura_log_emit(AURA_LOG_INFO, "engine started");
 *   // ... library and app logs forwarded to syslog ...
 *   aura_syslog_remove();
 *   aura_destroy(e);
 * @endcode
 */

#ifndef AURA_SYSLOG_H
#define AURA_SYSLOG_H

#include <aura.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Integration schema contract. */
#define AURA_SYSLOG_SCHEMA_VERSION "v0"
#define AURA_SYSLOG_SCHEMA_STABILITY "experimental"

/**
 * Syslog configuration options.
 *
 * Pass NULL to aura_syslog_install() for sensible defaults.
 * A value of -1 means "use default" for numeric fields.
 */
typedef struct {
    const char *ident; /**< openlog ident (default: "aura").
                            Must remain valid until aura_syslog_remove()
                            — POSIX openlog() retains the pointer. */
    int facility;      /**< Syslog facility (default: LOG_USER).
                            -1 means "use default". */
    int log_options;   /**< openlog() option flags
                            (default: LOG_PID | LOG_NDELAY).
                            -1 means "use default". */
} aura_syslog_options_t;

/**
 * Install a syslog-forwarding log handler.
 *
 * Calls openlog() then registers a log handler that calls syslog().
 * Thread-safe: syslog() is thread-safe per POSIX.
 *
 * @param options  Configuration (NULL for defaults)
 */
void aura_syslog_install(const aura_syslog_options_t *options);

/**
 * Remove the syslog log handler and close syslog.
 *
 * Restores the default (no handler) state and calls closelog().
 */
void aura_syslog_remove(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_SYSLOG_H */
