/**
 * @file log.c
 * @brief Internal logging implementation
 */

#include "../include/auraio.h"
#include "log.h"

#include <stdatomic.h>
#include <stdio.h>

static _Atomic(auraio_log_fn) log_handler = NULL;
static _Atomic(void *) log_userdata = NULL;

void auraio_set_log_handler(auraio_log_fn handler, void *userdata) {
    atomic_store_explicit(&log_userdata, userdata, memory_order_seq_cst);
    atomic_store_explicit(&log_handler, handler, memory_order_seq_cst);
}

void auraio_log(int level, const char *fmt, ...) {
    auraio_log_fn fn = atomic_load_explicit(&log_handler, memory_order_seq_cst);
    if (!fn) {
        return;
    }

    /* seq_cst on both handler and userdata guarantees a consistent pair
     * across independent atomics (acquire/release alone doesn't provide
     * cross-variable ordering on weakly-ordered architectures). */
    void *ud = atomic_load_explicit(&log_userdata, memory_order_seq_cst);

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fn(level, buf, ud);
}
