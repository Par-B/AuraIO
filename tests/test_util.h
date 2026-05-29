/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 AuraIO Contributors */
#ifndef AURA_TEST_UTIL_H
#define AURA_TEST_UTIL_H

/* Shared test helpers. Include AFTER aura.h (these wrappers use its types). */

/*
 * The event-loop pump functions (aura_poll/aura_wait/aura_drain) are marked
 * warn_unused_result so that production callers handle errors. Many tests pump
 * the loop in a `while (!done)` style and rely on a completion flag rather than
 * the return code. These thin wrappers consume the result so that intent is
 * explicit and the test build stays warning-clean, while the attribute keeps
 * guarding non-test code.
 */
static inline void tu_poll(aura_engine_t *engine) {
    int rc = aura_poll(engine);
    (void)rc;
}

static inline void tu_wait(aura_engine_t *engine, int timeout_ms) {
    int rc = aura_wait(engine, timeout_ms);
    (void)rc;
}

static inline void tu_drain(aura_engine_t *engine, int timeout_ms) {
    int rc = aura_drain(engine, timeout_ms);
    (void)rc;
}

#endif /* AURA_TEST_UTIL_H */
