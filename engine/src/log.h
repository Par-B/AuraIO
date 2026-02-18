// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file log.h
 * @brief Internal logging infrastructure
 *
 * Provides a user-settable log callback so the library never writes
 * directly to stderr.  Default handler is NULL (silent).
 *
 * Log level constants (AURA_LOG_ERR, etc.) are defined in the
 * public header <aura.h>.
 */

#ifndef AURA_LOG_H
#define AURA_LOG_H

#include "../include/aura.h"
#include <stdarg.h>

/**
 * Emit a log message through the registered handler (if any).
 *
 * No-op when no handler is registered.
 */
void aura_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#endif /* AURA_LOG_H */
