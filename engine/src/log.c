// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


/**
 * @file log.c
 * @brief Internal logging implementation
 */

#include "../include/aura.h"
#include "log.h"

#include <stdatomic.h>
#include <stdio.h>

static _Atomic(aura_log_fn) log_handler = NULL;
static _Atomic(void *) log_userdata = NULL;

void aura_set_log_handler(aura_log_fn handler, void *userdata) {
    atomic_store_explicit(&log_userdata, userdata, memory_order_seq_cst);
    atomic_store_explicit(&log_handler, handler, memory_order_seq_cst);
}

/* Shared helper: format + dispatch to handler (if registered). */
static void log_dispatch(int level, const char *fmt, va_list ap) {
    aura_log_fn fn = atomic_load_explicit(&log_handler, memory_order_seq_cst);
    if (!fn) {
        return;
    }

    /* Note: two independent atomics cannot provide pairwise atomicity.
     * A concurrent aura_set_log_handler() call could change the pair
     * between our two loads.  In practice this is benign: the handler is
     * set once at startup and cleared once at shutdown. */
    void *ud = atomic_load_explicit(&log_userdata, memory_order_seq_cst);

    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    fn(level, buf, ud);
}

/* Internal entry point (used by library code). */
void aura_log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_dispatch(level, fmt, ap);
    va_end(ap);
}

/* Public entry point (AURA_API visibility). */
void aura_log_emit(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_dispatch(level, fmt, ap);
    va_end(ap);
}
