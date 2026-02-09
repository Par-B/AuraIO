/**
 * @file log.h
 * @brief Internal logging infrastructure
 *
 * Provides a user-settable log callback so the library never writes
 * directly to stderr.  Default handler is NULL (silent).
 */

#ifndef AURAIO_LOG_H
#define AURAIO_LOG_H

#include <stdarg.h>

/** Log severity levels (match syslog values). */
typedef enum {
    AURAIO_LOG_ERR = 3,
    AURAIO_LOG_WARN = 4,
} auraio_log_level_t;

/**
 * Emit a log message through the registered handler (if any).
 *
 * No-op when no handler is registered.
 */
void auraio_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* AURAIO_LOG_H */
