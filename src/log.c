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
    atomic_store_explicit(&log_userdata, userdata, memory_order_release);
    atomic_store_explicit(&log_handler, handler, memory_order_release);
}

void auraio_log(int level, const char *fmt, ...) {
    auraio_log_fn fn = atomic_load_explicit(&log_handler, memory_order_acquire);
    if (!fn) {
        return;
    }

    /* Load userdata immediately after handler to get a consistent pair.
     * set_log_handler stores userdata before handler (release order),
     * so an acquire load of handler that sees the new fn also sees the
     * matching userdata. */
    void *ud = atomic_load_explicit(&log_userdata, memory_order_acquire);

    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    fn(level, buf, ud);
}
