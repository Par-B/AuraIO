# Async Lifecycle Semantics

This page clarifies AuraIO's asynchronous operation lifecycle for external users.

## Two Error Phases

1. **Submission phase**: `aura_read/write/...` returns immediately.
   - Success: non-NULL `aura_request_t*`
   - Failure: `NULL`, with `errno` set (see [Error Codes](#submission-error-codes))

2. **Completion phase**: callback receives final operation result.
   - Success: non-negative `result` (bytes transferred)
   - Failure: negative errno-style value (`-E...`)

Submission success does not guarantee I/O success; final outcome is delivered at completion.

### Submission Error Codes

| errno | Meaning |
|-------|---------|
| `EINVAL` | Invalid argument (NULL engine, bad fd, zero length, invalid buffer type, index out of range) |
| `EAGAIN` | Ring is full, retry after processing completions |
| `ESHUTDOWN` | Engine is shutting down |
| `ENOENT` | No buffers/files registered (for fixed-buffer/file operations) |
| `EOVERFLOW` | Fixed buffer offset+len exceeds registered buffer size |
| `ENOMEM` | Out of SQEs or request slots |
| `EBUSY` | Deferred unregister is draining (for fixed-buffer submissions) |

## Request Handle Lifetime

`aura_request_t*` is valid from submission through the end of the callback body. The slot is recycled after the callback returns.

```
submit() ──> [handle valid] ──> callback starts ──> [callback body] ──> callback returns ──> [INVALID]
             │                  │                    │                   │
             ├─ cancel()        ├─ request_fd()      ├─ request_fd()     └─ slot may be reused
             ├─ request_pending()├─ request_user_data()├─ request_user_data()
             └─ request_fd()    └─ request_pending() └─ cancel() (no-op, already complete)
```

1. Valid for `aura_cancel()` / `aura_request_pending()` while pending.
2. Introspection functions (`aura_request_fd()`, `aura_request_user_data()`, etc.) are safe to call inside the callback.
3. Invalid after the callback returns — the request slot may be reused immediately.
4. Must not be stored or accessed beyond the callback return.

## Polling and Waiting

| Function | Blocking | Returns |
|----------|----------|---------|
| `aura_poll(engine)` | No | Completions processed |
| `aura_wait(engine, timeout_ms)` | Yes (up to timeout) | Completions processed, or -1 on error |
| `aura_drain(engine, timeout_ms)` | Yes (until all complete) | Total completions, or -1 on timeout/error |
| `aura_run(engine)` | Yes (until stop) | — |

- `poll`, `wait`, and `run` must NOT be called concurrently on the same engine.
- Multiple threads may submit I/O concurrently while one thread polls/waits.
- `aura_stop()` is safe to call from any thread, including callbacks.

## Registered Buffer Lifecycle

Registered buffers enable zero-copy I/O by pinning memory in the kernel.

### Registration

```c
struct iovec iovs[2] = {{buf1, 4096}, {buf2, 4096}};
aura_register_buffers(engine, iovs, 2);

// Use registered buffers by index
aura_read(engine, fd, aura_buf_fixed(0, 0), 4096, offset, cb, ud);
```

### Deferred Unregister (callback-safe)

Unregistering buffers while I/O is in-flight requires the deferred pattern:

```c
// From a callback or any context:
aura_request_unregister(engine, AURA_REG_BUFFERS);
// Returns immediately. New fixed-buffer submissions fail with EBUSY.
// Note: if no fixed-buffer operations are in-flight, unregistration completes
// synchronously and may block briefly for the kernel call.
// Actual unregister happens when all in-flight fixed-buffer ops complete.
// After finalization, fixed-buffer submissions fail with ENOENT.
```

### Synchronous Unregister

```c
// From a non-callback context:
aura_unregister(engine, AURA_REG_BUFFERS);
// Blocks until all in-flight fixed-buffer ops complete and unregister finishes.
// If called from a callback, degrades to deferred mode automatically.
// Timeout: if in-flight operations do not complete within 10 seconds,
// returns -1 with errno=ETIMEDOUT. The buffers remain registered in that case;
// callers should treat ETIMEDOUT as a failure and decide whether to retry or
// proceed to engine teardown.
```

### State Transitions

```
UNREGISTERED ──register──> REGISTERED ──aura_request_unregister()──> DRAINING ──all complete──> UNREGISTERED
                                       └──aura_unregister() (sync)──> blocks until UNREGISTERED
```

During DRAINING:
- New `aura_buf_fixed()` submissions fail with `EBUSY`
- In-flight fixed-buffer operations complete normally
- Regular (non-fixed) I/O is unaffected

## Registered File Lifecycle

File descriptor registration follows the same pattern as buffer registration, with `aura_register_files()`, `aura_request_unregister(engine, AURA_REG_FILES)`, and `aura_unregister(engine, AURA_REG_FILES)`.

## Recommended Pattern

1. Submit operations and handle immediate errors (NULL return + errno).
2. Use `aura_pending_count()` to drive wait loops — it returns the number of in-flight operations across all rings, making termination conditions explicit:
   ```c
   while (aura_pending_count(engine) > 0)
       aura_wait(engine, 100);
   ```
3. Treat callback result as source of truth for operation success/failure.
4. For shutdown: `aura_drain(engine, -1)` ensures all pending I/O completes.
5. For registered buffers: use deferred unregister from callbacks, synchronous from main thread.

---

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for details.
