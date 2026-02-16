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

`aura_request_t*` is valid only from submission until callback start.

```
submit() ──────> [handle valid] ──────> callback starts ──────> [INVALID]
                 │                      │
                 ├─ cancel()            ├─ handle must not be
                 ├─ request_pending()   │  accessed after this
                 └─ request_fd()        └─ slot may be reused
```

1. Valid for `aura_cancel()` / `aura_request_pending()` while pending.
2. Invalid once callback begins — the request slot may be reused immediately.
3. Must not be stored beyond the callback.

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
aura_request_unregister_buffers(engine);
// Returns immediately. New fixed-buffer submissions fail with EBUSY.
// Actual unregister happens when all in-flight fixed-buffer ops complete.
// After finalization, fixed-buffer submissions fail with ENOENT.
```

### Synchronous Unregister

```c
// From a non-callback context:
aura_unregister_buffers(engine);
// Blocks until all in-flight fixed-buffer ops complete and unregister finishes.
// If called from a callback, degrades to deferred mode automatically.
```

### State Transitions

```
UNREGISTERED ──register──> REGISTERED ──request_unregister──> DRAINING ──all complete──> UNREGISTERED
                                       └──unregister (sync)──> blocks until UNREGISTERED
```

During DRAINING:
- New `aura_buf_fixed()` submissions fail with `EBUSY`
- In-flight fixed-buffer operations complete normally
- Regular (non-fixed) I/O is unaffected

## Registered File Lifecycle

File descriptor registration follows the same pattern as buffer registration, with `aura_register_files()`, `aura_request_unregister_files()`, and `aura_unregister_files()`.

## Recommended Pattern

1. Submit operations and handle immediate errors (NULL return + errno).
2. Use `poll`/`wait` loop to process completions.
3. Treat callback result as source of truth for operation success/failure.
4. For shutdown: `aura_drain(engine, -1)` ensures all pending I/O completes.
5. For registered buffers: use deferred unregister from callbacks, synchronous from main thread.
