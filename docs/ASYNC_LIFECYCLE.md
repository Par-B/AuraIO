# Async Lifecycle Semantics

This page clarifies AuraIO's asynchronous operation lifecycle for external users.

## Two Error Phases

1. **Submission phase**: `auraio_read/write/...` returns immediately.
   - Success: non-NULL `auraio_request_t*`
   - Failure: `NULL`, with `errno` set

2. **Completion phase**: callback receives final operation result.
   - Success: non-negative `result` (bytes transferred)
   - Failure: negative errno-style value (`-E...`)

Submission success does not guarantee I/O success; final outcome is delivered at completion.

## Request Handle Lifetime

`auraio_request_t*` is valid only from submission until callback start.

1. Valid for `auraio_cancel()` / `auraio_request_pending()` while pending.
2. Invalid once callback begins.
3. Must not be accessed after callback starts.

## Polling and Waiting

1. `auraio_poll(engine)` is non-blocking and processes available completions.
2. `auraio_wait(engine, timeout_ms)` blocks (up to timeout) and processes completions.
3. Both return number of completions processed (or `-1` on `wait` error).

## Recommended Pattern

1. Submit operations and handle immediate errors.
2. Use `poll`/`wait` loop.
3. Treat callback result as source of truth for operation success/failure.
