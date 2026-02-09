/**
 * @file log.h
 * @brief Internal logging infrastructure
 *
 * Provides a user-settable log callback so the library never writes
 * directly to stderr.  Default handler is NULL (silent).
 *
 * Log level constants (AURAIO_LOG_ERR, etc.) are defined in the
 * public header <auraio.h>.
 */

#ifndef AURAIO_LOG_H
#define AURAIO_LOG_H

#include "../include/auraio.h"
#include <stdarg.h>

/**
 * Emit a log message through the registered handler (if any).
 *
 * No-op when no handler is registered.
 */
void auraio_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* AURAIO_LOG_H */
