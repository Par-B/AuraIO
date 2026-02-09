# AuraIO API Reference

**Version 0.1.0**

AuraIO is a self-tuning async I/O library for Linux built on io_uring. It provides three API surfaces: a C11 core library, C++20 bindings with RAII and coroutine support, and Rust bindings with safe and async wrappers.

---

## Table of Contents

- [C API](#c-api)
  - [Opaque Types](#opaque-types)
  - [Callback Type](#callback-type)
  - [Configuration](#configuration)
  - [Buffer Descriptor](#buffer-descriptor)
  - [Fsync Flags](#fsync-flags)
  - [Statistics Types](#statistics-types)
  - [Constants](#constants)
  - [Functions](#functions)
- [C++ API](#c-api-1)
  - [Error Handling](#error-handling)
  - [Options](#auraiooptions)
  - [Engine](#auraioengine)
  - [Request](#auraiorequest)
  - [BufferRef](#auraiobufferref)
  - [Buffer](#auraiobuffer)
  - [Statistics Classes](#statistics-classes)
  - [Coroutine Support](#coroutine-support)
- [Rust API](#rust-api)
  - [Error Handling](#rust-error-handling)
  - [Options](#options)
  - [Engine](#engine)
  - [Buffer and BufferRef](#buffer-and-bufferref)
  - [RequestHandle](#requesthandle)
  - [Stats](#stats)
  - [Async Support](#async-support)
- [Error Codes Reference](#error-codes-reference)
- [Threading Model](#threading-model)
- [Callback Rules](#callback-rules)

---

## C API

Header: `#include <auraio.h>`

Link: `-lauraio -luring -lpthread`

### Opaque Types

| Type | Description |
|------|-------------|
| `auraio_engine_t` | Engine handle. Created by `auraio_create()`, destroyed by `auraio_destroy()`. Thread-safe for submissions from multiple threads. |
| `auraio_request_t` | Request handle. Returned by I/O submission functions. Valid from submission until the completion callback begins execution. Can be used with `auraio_cancel()` and `auraio_request_pending()` while in-flight. |

### Callback Type

```c
typedef void (*auraio_callback_t)(auraio_request_t *req, ssize_t result, void *user_data);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `req` | `auraio_request_t *` | Request handle (valid only during callback) |
| `result` | `ssize_t` | Bytes transferred on success, negative errno on failure, `-ECANCELED` if cancelled |
| `user_data` | `void *` | User pointer from the submission call |

Passing `NULL` as a callback is permitted -- the I/O executes but no completion notification is delivered.

Callbacks execute in the context of `auraio_poll()` or `auraio_wait()`. Since these functions drain completions from **all** rings, a callback may fire on **any thread** that calls `auraio_wait()` -- not necessarily the thread that submitted the I/O. Do not use thread-local storage to identify the originating context; instead, pass all necessary state through `user_data`. The `user_data` pointer (and any memory it references) must remain valid until the callback has executed. Callbacks may submit new I/O but must not call `auraio_destroy()`.

### Configuration

#### `auraio_options_t`

```c
typedef struct {
  size_t struct_size;
  int queue_depth;
  int ring_count;
  int initial_in_flight;
  int min_in_flight;
  double max_p99_latency_ms;
  size_t buffer_alignment;
  bool disable_adaptive;
  bool enable_sqpoll;
  int sqpoll_idle_ms;
  auraio_ring_select_t ring_select;
} auraio_options_t;
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `struct_size` | `size_t` | (set by init) | Set by `auraio_options_init()`; used for ABI forward-compatibility |
| `queue_depth` | `int` | 256 | Queue depth per io_uring ring |
| `ring_count` | `int` | 0 (auto) | Number of rings. 0 = one per CPU core |
| `initial_in_flight` | `int` | queue_depth/4 | Starting in-flight operation limit |
| `min_in_flight` | `int` | 4 | Floor for AIMD in-flight limit |
| `max_p99_latency_ms` | `double` | 0 (auto) | Target P99 latency threshold. 0 = auto-detect |
| `buffer_alignment` | `size_t` | system page size | Alignment for `auraio_buffer_alloc()` |
| `disable_adaptive` | `bool` | false | Disable AIMD adaptive tuning |
| `enable_sqpoll` | `bool` | false | Use kernel-side submission polling (requires root or `CAP_SYS_NICE`) |
| `sqpoll_idle_ms` | `int` | 1000 | SQPOLL idle timeout before kernel thread sleeps |
| `ring_select` | `auraio_ring_select_t` | `AURAIO_SELECT_ADAPTIVE` | Ring selection mode (see below) |

Always initialize with `auraio_options_init()` before modifying fields.

#### `auraio_ring_select_t`

```c
typedef enum {
  AURAIO_SELECT_ADAPTIVE = 0,
  AURAIO_SELECT_CPU_LOCAL,
  AURAIO_SELECT_ROUND_ROBIN
} auraio_ring_select_t;
```

| Mode | When to use |
|------|-------------|
| `AURAIO_SELECT_ADAPTIVE` | Default. Uses CPU-local ring until congested (>75% of in-flight limit). If local load > 2x average, stays local (outlier). Otherwise, spills via power-of-two random choice to the lighter of two non-local rings. Best for most workloads. |
| `AURAIO_SELECT_CPU_LOCAL` | Strict CPU affinity. Best for NUMA-sensitive workloads where cross-node traffic is expensive. Single-thread throughput is limited to one ring. |
| `AURAIO_SELECT_ROUND_ROBIN` | Always distributes via atomic round-robin. Best for single-thread event loops or benchmarks that need maximum ring utilization from fewer threads. |

### Buffer Descriptor

```c
typedef enum {
  AURAIO_BUF_UNREGISTERED = 0,
  AURAIO_BUF_REGISTERED = 1
} auraio_buf_type_t;

typedef struct {
  auraio_buf_type_t type;
  union {
    void *ptr;
    struct { int index; size_t offset; } fixed;
  } u;
} auraio_buf_t;
```

Create with inline helpers -- do not construct manually:

| Helper | Description |
|--------|-------------|
| `auraio_buf(void *ptr)` | Wrap a regular buffer pointer |
| `auraio_buf_fixed(int index, size_t offset)` | Reference a registered buffer at offset. Returns an invalid (NULL-pointer) descriptor if `index < 0`. |
| `auraio_buf_fixed_idx(int index)` | Reference a registered buffer at offset 0 |

### Fsync Flags

```c
typedef enum {
  AURAIO_FSYNC_DEFAULT  = 0,
  AURAIO_FSYNC_DATASYNC = 1,
} auraio_fsync_flags_t;
```

| Flag | Description |
|------|-------------|
| `AURAIO_FSYNC_DEFAULT` | Full fsync (metadata + data) |
| `AURAIO_FSYNC_DATASYNC` | fdatasync (data only, skip metadata if possible) |

### Statistics Types

#### `auraio_stats_t` -- Aggregate Engine Stats

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed across all rings |
| `bytes_transferred` | `int64_t` | Total bytes read/written across all rings |
| `current_throughput_bps` | `double` | Current aggregate throughput (bytes/sec) |
| `p99_latency_ms` | `double` | 99th percentile latency (ms) |
| `current_in_flight` | `int` | Current total in-flight operations |
| `optimal_in_flight` | `int` | AIMD-tuned aggregate in-flight limit |
| `optimal_batch_size` | `int` | AIMD-tuned aggregate batch size |
| `adaptive_spills` | `uint64_t` | ADAPTIVE mode: count of submissions that spilled to another ring |

#### `auraio_ring_stats_t` -- Per-Ring Stats

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed on this ring |
| `bytes_transferred` | `int64_t` | Total bytes transferred through this ring |
| `pending_count` | `int` | Current in-flight operations |
| `in_flight_limit` | `int` | AIMD-tuned maximum in-flight |
| `batch_threshold` | `int` | AIMD-tuned batch size |
| `p99_latency_ms` | `double` | Current P99 latency estimate (ms) |
| `throughput_bps` | `double` | Current throughput (bytes/sec) |
| `aimd_phase` | `int` | Controller phase (0-5, see constants below) |
| `queue_depth` | `int` | Kernel queue depth for this ring |

#### `auraio_histogram_t` -- Latency Histogram

An approximate snapshot of the active histogram window. Because the snapshot is read from a concurrently-written histogram, individual bucket values are atomic but `total_count` may differ slightly from the sum of buckets + overflow. When adaptive tuning is disabled (`disable_adaptive = true`), the histogram accumulates data indefinitely instead of being periodically reset.

| Field | Type | Description |
|-------|------|-------------|
| `buckets[200]` | `uint32_t[]` | Latency frequency buckets |
| `overflow` | `uint32_t` | Count of operations exceeding `max_tracked_us` |
| `total_count` | `uint32_t` | Total samples in this snapshot |
| `bucket_width_us` | `int` | Width of each bucket in microseconds |
| `max_tracked_us` | `int` | Maximum tracked latency in microseconds |

#### `auraio_buffer_stats_t` -- Buffer Pool Stats

| Field | Type | Description |
|-------|------|-------------|
| `total_allocated_bytes` | `size_t` | Total bytes currently allocated from pool |
| `total_buffers` | `size_t` | Total buffer count currently allocated |
| `shard_count` | `int` | Number of pool shards |

### Constants

#### Version

| Macro | Value | Description |
|-------|-------|-------------|
| `AURAIO_VERSION_MAJOR` | 0 | Major version |
| `AURAIO_VERSION_MINOR` | 1 | Minor version |
| `AURAIO_VERSION_PATCH` | 0 | Patch version |
| `AURAIO_VERSION` | 100 | Combined: `major * 10000 + minor * 100 + patch` |
| `AURAIO_VERSION_STRING` | `"0.1.0"` | Version string |

#### AIMD Phase Constants

Used in `auraio_ring_stats_t.aimd_phase`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `AURAIO_PHASE_BASELINE` | 0 | Collecting baseline latency samples |
| `AURAIO_PHASE_PROBING` | 1 | Additive increase -- testing higher depth |
| `AURAIO_PHASE_STEADY` | 2 | Throughput plateau -- holding position |
| `AURAIO_PHASE_BACKOFF` | 3 | Multiplicative decrease -- P99 exceeded target |
| `AURAIO_PHASE_SETTLING` | 4 | Post-backoff stabilization |
| `AURAIO_PHASE_CONVERGED` | 5 | Optimal depth found |

#### Histogram Constants

| Macro | Value | Description |
|-------|-------|-------------|
| `AURAIO_HISTOGRAM_BUCKETS` | 200 | Number of histogram buckets |
| `AURAIO_HISTOGRAM_BUCKET_WIDTH_US` | 50 | Bucket width in microseconds |

Histogram range: 0 to 10,000 us (10 ms). Operations exceeding 10 ms are counted in `overflow`.

---

### Functions

#### Configuration

##### `auraio_options_init`

```c
void auraio_options_init(auraio_options_t *options);
```

Initialize an options struct with default values. Always call before modifying individual fields.

| Parameter | Type | Description |
|-----------|------|-------------|
| `options` | `auraio_options_t *` | Options struct to initialize |

---

#### Lifecycle

##### `auraio_create`

```c
auraio_engine_t *auraio_create(void);
```

Create a new engine with default options. Detects CPU cores, creates one io_uring ring per core, and initializes AIMD controllers.

**Returns:** Engine handle, or `NULL` on failure (errno set).

---

##### `auraio_create_with_options`

```c
auraio_engine_t *auraio_create_with_options(const auraio_options_t *options);
```

Create a new engine with custom options.

| Parameter | Type | Description |
|-----------|------|-------------|
| `options` | `const auraio_options_t *` | Configuration (must be initialized with `auraio_options_init` first) |

**Returns:** Engine handle, or `NULL` on failure (errno set).

---

##### `auraio_destroy`

```c
void auraio_destroy(auraio_engine_t *engine);
```

Destroy the engine. Signals shutdown (new submissions fail with `ESHUTDOWN`), waits for all pending operations to complete, then frees resources. Safe to call from any thread. `NULL` is a no-op.

The caller must ensure all worker threads have stopped submitting I/O and completed their buffer pool operations before calling this function. The recommended shutdown sequence is:

1. Signal worker threads to stop
2. Join/wait for all worker threads
3. Call `auraio_destroy()`

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine to destroy (may be `NULL`) |

---

#### I/O Operations

All I/O submission functions return a request handle on success, or `NULL` on failure with errno set to one of: `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOENT`, `EOVERFLOW`, `ENOMEM`.

Buffers must remain valid until the callback fires. The callback may be `NULL` for fire-and-forget operations.

---

##### `auraio_read`

```c
auraio_request_t *auraio_read(auraio_engine_t *engine, int fd, auraio_buf_t buf,
                              size_t len, off_t offset,
                              auraio_callback_t callback, void *user_data);
```

Submit an async read. Supports regular and registered buffers.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `buf` | `auraio_buf_t` | Buffer descriptor (use `auraio_buf()` or `auraio_buf_fixed()`) |
| `len` | `size_t` | Bytes to read |
| `offset` | `off_t` | File offset |
| `callback` | `auraio_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOENT`, `EOVERFLOW`, `ENOMEM`

---

##### `auraio_write`

```c
auraio_request_t *auraio_write(auraio_engine_t *engine, int fd, auraio_buf_t buf,
                               size_t len, off_t offset,
                               auraio_callback_t callback, void *user_data);
```

Submit an async write. Same parameters as `auraio_read`.

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOENT`, `EOVERFLOW`, `ENOMEM`

---

##### `auraio_readv`

```c
auraio_request_t *auraio_readv(auraio_engine_t *engine, int fd,
                               const struct iovec *iov, int iovcnt,
                               off_t offset, auraio_callback_t callback,
                               void *user_data);
```

Submit an async scatter read into multiple buffers. The iovec array and all buffers must remain valid until the callback fires.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `iov` | `const struct iovec *` | Array of iovec structures |
| `iovcnt` | `int` | Number of elements in `iov` |
| `offset` | `off_t` | File offset |
| `callback` | `auraio_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `auraio_writev`

```c
auraio_request_t *auraio_writev(auraio_engine_t *engine, int fd,
                                const struct iovec *iov, int iovcnt,
                                off_t offset, auraio_callback_t callback,
                                void *user_data);
```

Submit an async gather write from multiple buffers. Same parameters as `auraio_readv`.

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `auraio_fsync`

```c
auraio_request_t *auraio_fsync(auraio_engine_t *engine, int fd,
                               auraio_fsync_flags_t flags,
                               auraio_callback_t callback, void *user_data);
```

Submit an async fsync. Pass `AURAIO_FSYNC_DEFAULT` for a full fsync (metadata + data) or `AURAIO_FSYNC_DATASYNC` for fdatasync behavior (data only, skip metadata if possible).

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `flags` | `auraio_fsync_flags_t` | `AURAIO_FSYNC_DEFAULT` (0) or `AURAIO_FSYNC_DATASYNC` (1) |
| `callback` | `auraio_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

#### Request Management

##### `auraio_cancel`

```c
int auraio_cancel(auraio_engine_t *engine, auraio_request_t *req);
```

Cancel a pending I/O operation (best-effort). If cancelled, the callback receives `result = -ECANCELED`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `req` | `auraio_request_t *` | Request to cancel (must belong to this engine) |

**Returns:** 0 if cancellation submitted, -1 on error. A return of 0 does not guarantee the operation will actually be cancelled.

**Errors:** `EINVAL`, `EALREADY`, `ESHUTDOWN`, `ENOMEM`

---

##### `auraio_request_pending`

```c
bool auraio_request_pending(const auraio_request_t *req);
```

Check if a request is still in-flight.

**Returns:** `true` if pending, `false` if completed or cancelled.

---

##### `auraio_request_fd`

```c
int auraio_request_fd(const auraio_request_t *req);
```

Get the file descriptor associated with a request.

**Returns:** File descriptor, or -1 if request is invalid.

---

##### `auraio_request_user_data`

```c
void *auraio_request_user_data(const auraio_request_t *req);
```

Get the user data pointer associated with a request.

**Returns:** The `user_data` pointer from the submission call.

---

#### Event Processing

##### `auraio_get_poll_fd`

```c
int auraio_get_poll_fd(const auraio_engine_t *engine);
```

Get a pollable file descriptor for event loop integration. Becomes readable when completions are available. Uses level-triggered semantics: remains readable as long as unprocessed completions exist. Compatible with epoll (`EPOLLIN`), poll (`POLLIN`), and select.

**Returns:** Pollable fd, or -1 on error (errno set).

---

##### `auraio_poll`

```c
int auraio_poll(auraio_engine_t *engine);
```

Process completed operations without blocking. Invokes callbacks for any finished operations.

**Returns:** Number of completions processed.

---

##### `auraio_wait`

```c
int auraio_wait(auraio_engine_t *engine, int timeout_ms);
```

Block until at least one operation completes or timeout expires.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `timeout_ms` | `int` | Max wait: -1 = forever, 0 = non-blocking |

**Returns:** Number of completions processed, or -1 on error.

---

##### `auraio_run`

```c
void auraio_run(auraio_engine_t *engine);
```

Run the event loop until `auraio_stop()` is called. Blocks the calling thread. Useful for dedicating a thread to I/O processing.

---

##### `auraio_stop`

```c
void auraio_stop(auraio_engine_t *engine);
```

Signal the event loop to stop. Thread-safe -- can be called from any thread or from within a callback. `auraio_run()` returns after processing current completions.

---

##### `auraio_drain`

```c
int auraio_drain(auraio_engine_t *engine, int timeout_ms);
```

Wait until all in-flight operations across all rings have completed. Useful for graceful shutdown or synchronization points.

New submissions are **not** blocked during drain; if other threads submit concurrently, drain processes those as well.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `timeout_ms` | `int` | Max wait: -1 = forever, 0 = non-blocking poll |

**Returns:** Total completions processed, or -1 on error/timeout.

**Errors:** `ETIMEDOUT` if deadline exceeded, `EINVAL` if engine is `NULL`.

---

#### Buffer Management

##### `auraio_buffer_alloc`

```c
void *auraio_buffer_alloc(auraio_engine_t *engine, size_t size);
```

Allocate a page-aligned buffer from the engine's pool. Suitable for `O_DIRECT` I/O. Thread-safe; uses per-thread caching for fast allocation. A buffer allocated by one thread may be freed by another.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `size` | `size_t` | Buffer size in bytes |

**Returns:** Aligned buffer pointer, or `NULL` on failure.

---

##### `auraio_buffer_free`

```c
void auraio_buffer_free(auraio_engine_t *engine, void *buf, size_t size);
```

Return a buffer to the engine's pool. Thread-safe.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `buf` | `void *` | Buffer to free (may be `NULL`) |
| `size` | `size_t` | **Must** match the original allocation size exactly; passing a different size causes undefined behavior |

---

#### Registered Buffers

Registered buffers eliminate kernel mapping overhead for small, frequent I/O. After registration, use `auraio_buf_fixed()` with the standard `auraio_read`/`auraio_write`. Best for buffers reused across many I/O operations (1000+) and high-frequency small I/O (< 16KB).

##### `auraio_register_buffers`

```c
int auraio_register_buffers(auraio_engine_t *engine, const struct iovec *iovs, int count);
```

Pre-register buffers with the kernel for zero-copy I/O. Call once at startup. After registration, use `auraio_buf_fixed()` to reference buffers by index in read/write calls.

Fixed-buffer submissions fail with `EBUSY` while a deferred unregister is draining.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `iovs` | `const struct iovec *` | Array of buffer descriptors |
| `count` | `int` | Number of buffers |

**Returns:** 0 on success, -1 on error (errno set).

---

##### `auraio_request_unregister_buffers`

```c
int auraio_request_unregister_buffers(auraio_engine_t *engine);
```

Request deferred unregister of registered buffers. Marks buffers as draining and returns immediately. New fixed-buffer submissions fail with `EBUSY` while draining. Final unregister completes lazily once in-flight fixed-buffer operations reach zero.

**Safe to call from completion callbacks.**

**Returns:** 0 on success, -1 on error.

---

##### `auraio_unregister_buffers`

```c
int auraio_unregister_buffers(auraio_engine_t *engine);
```

Unregister previously registered buffers. For non-callback callers, waits until in-flight fixed-buffer operations drain and unregister completes. If called from within a callback, degrades to `auraio_request_unregister_buffers()` and returns immediately.

**Returns:** 0 on success, -1 on error.

---

#### File Registration

##### `auraio_register_files`

```c
int auraio_register_files(auraio_engine_t *engine, const int *fds, int count);
```

Pre-register file descriptors with the kernel to eliminate lookup overhead.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fds` | `const int *` | Array of file descriptors |
| `count` | `int` | Number of file descriptors |

**Returns:** 0 on success, -1 on error (errno set).

---

##### `auraio_update_file`

```c
int auraio_update_file(auraio_engine_t *engine, int index, int fd);
```

Replace a registered fd at the given index. Pass -1 as `fd` to unregister a slot.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `index` | `int` | Index in registered file array |
| `fd` | `int` | New fd, or -1 to unregister slot |

**Returns:** 0 on success, -1 on error (errno set). On failure, ring state may be inconsistent -- call `auraio_unregister_files()` and re-register.

---

##### `auraio_request_unregister_files`

```c
int auraio_request_unregister_files(auraio_engine_t *engine);
```

Request deferred unregister of registered files. Marks files for unregister and returns immediately.

**Safe to call from completion callbacks.**

**Returns:** 0 on success, -1 on error.

---

##### `auraio_unregister_files`

```c
int auraio_unregister_files(auraio_engine_t *engine);
```

Unregister all previously registered files. For non-callback callers, waits until unregister completes. If called from within a callback, degrades to `auraio_request_unregister_files()` and returns immediately.

**Returns:** 0 on success, -1 on error.

---

#### Statistics

All stats functions are thread-safe and safe to call during active I/O.

##### `auraio_get_stats`

```c
void auraio_get_stats(const auraio_engine_t *engine, auraio_stats_t *stats);
```

Get engine-wide aggregate statistics. If `engine` or `stats` is `NULL`, the call is a no-op (stats zeroed if only engine is `NULL`).

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const auraio_engine_t *` | Engine handle |
| `stats` | `auraio_stats_t *` | Output struct |

---

##### `auraio_get_ring_count`

```c
int auraio_get_ring_count(const auraio_engine_t *engine);
```

**Returns:** Number of io_uring rings, or 0 if engine is `NULL`.

---

##### `auraio_get_ring_stats`

```c
int auraio_get_ring_stats(const auraio_engine_t *engine, int ring_idx,
                          auraio_ring_stats_t *stats);
```

Get per-ring statistics.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const auraio_engine_t *` | Engine handle |
| `ring_idx` | `int` | Ring index (0 to `auraio_get_ring_count()-1`) |
| `stats` | `auraio_ring_stats_t *` | Output struct |

**Returns:** 0 on success. -1 if engine/stats is `NULL` or `ring_idx` is out of range (stats zeroed on invalid index).

---

##### `auraio_get_histogram`

```c
int auraio_get_histogram(const auraio_engine_t *engine, int ring_idx,
                         auraio_histogram_t *hist);
```

Get a latency histogram snapshot for a ring. The snapshot is approximate -- see `auraio_histogram_t` documentation.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const auraio_engine_t *` | Engine handle |
| `ring_idx` | `int` | Ring index (0 to `auraio_get_ring_count()-1`) |
| `hist` | `auraio_histogram_t *` | Output struct |

**Returns:** 0 on success. -1 if engine/hist is `NULL` or `ring_idx` is out of range (hist zeroed on invalid index).

---

##### `auraio_get_buffer_stats`

```c
int auraio_get_buffer_stats(const auraio_engine_t *engine, auraio_buffer_stats_t *stats);
```

Get buffer pool statistics. Lockless -- reads atomic counters.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const auraio_engine_t *` | Engine handle |
| `stats` | `auraio_buffer_stats_t *` | Output struct |

**Returns:** 0 on success, -1 if engine or stats is `NULL`.

---

##### `auraio_phase_name`

```c
const char *auraio_phase_name(int phase);
```

Get a human-readable name for an AIMD phase value.

| Parameter | Type | Description |
|-----------|------|-------------|
| `phase` | `int` | Phase value (0-5) from `auraio_ring_stats_t.aimd_phase` |

**Returns:** Static string: `"BASELINE"`, `"PROBING"`, `"STEADY"`, `"BACKOFF"`, `"SETTLING"`, `"CONVERGED"`, or `"UNKNOWN"` for out-of-range values.

---

#### Version

##### `auraio_version`

```c
const char *auraio_version(void);
```

**Returns:** Version string (e.g., `"0.1.0"`).

---

##### `auraio_version_int`

```c
int auraio_version_int(void);
```

**Returns:** Version as integer: `major * 10000 + minor * 100 + patch` (e.g., 100).

---

## C++ API

Header: `#include <auraio.hpp>` (includes all sub-headers)

Requires: C++20 (`-std=c++20`)

Namespace: `auraio`

### Error Handling

The C++ API throws exceptions on failure. I/O and lifecycle operations throw `auraio::Error`. Per-ring stats/histogram accessors throw `std::out_of_range` on invalid ring index. Exceptions thrown from user callbacks result in `std::terminate()`.

### `auraio::Error`

Defined in `<auraio/error.hpp>`. Inherits from `std::system_error`.

```cpp
class Error : public std::system_error {
public:
    explicit Error(int err, std::string_view context = {});

    int code() const noexcept;          // errno value
    const char* what() const noexcept;  // "context: strerror(err)"

    // Convenience predicates
    bool is_invalid() const noexcept;    // EINVAL
    bool is_again() const noexcept;      // EAGAIN
    bool is_shutdown() const noexcept;   // ESHUTDOWN
    bool is_cancelled() const noexcept;  // ECANCELED
    bool is_busy() const noexcept;       // EBUSY
    bool is_not_found() const noexcept;  // ENOENT
};
```

Free functions:

| Function | Description |
|----------|-------------|
| `void check(bool condition, std::string_view context = {})` | Throw `Error` with current errno if `condition` is false |
| `[[noreturn]] void throw_errno(std::string_view context = {})` | Throw `Error` with current errno |

---

### `auraio::Options`

Defined in `<auraio/options.hpp>`. Builder pattern for engine configuration.

```cpp
auraio::Options opts;
opts.queue_depth(512)
    .ring_count(4)
    .enable_sqpoll(true);

auraio::Engine engine(opts);
```

**Builder methods** (all return `Options&` for chaining, all `noexcept`):

| Method | Parameter | Description |
|--------|-----------|-------------|
| `queue_depth(int)` | depth | Queue depth per ring (default: 256) |
| `ring_count(int)` | count | Number of rings (0 = auto) |
| `initial_in_flight(int)` | limit | Starting in-flight limit |
| `min_in_flight(int)` | limit | Minimum in-flight limit |
| `max_p99_latency_ms(double)` | ms | Target P99 latency (0 = auto) |
| `buffer_alignment(size_t)` | align | Buffer alignment (default: page size) |
| `disable_adaptive(bool)` | disable | Disable AIMD tuning (default param: `true`) |
| `enable_sqpoll(bool)` | enable | Enable SQPOLL mode (default param: `true`) |
| `sqpoll_idle_ms(int)` | ms | SQPOLL idle timeout |
| `ring_select(RingSelect)` | mode | Ring selection mode |

`RingSelect` enum: `auraio::RingSelect::Adaptive`, `CpuLocal`, `RoundRobin`.

**Getters** (all `[[nodiscard]] noexcept`): same names as setters, no arguments.

`c_options()` returns `const auraio_options_t&` for access to the underlying C struct.

---

### `auraio::Engine`

Defined in `<auraio/engine.hpp>`. Main engine class. Move-only (non-copyable).

#### Constructors and Destructor

| Signature | Description |
|-----------|-------------|
| `Engine()` | Create with default options. Throws `Error`. |
| `explicit Engine(const Options& opts)` | Create with custom options. Throws `Error`. |
| `Engine(Engine&&) noexcept` | Move constructor |
| `Engine& operator=(Engine&&) noexcept` | Move assignment |
| `~Engine()` | Waits for pending I/O, then destroys |

#### I/O Operations (Callback)

All template on `Callback` concept: `std::invocable<F, Request&, ssize_t>`.

All throw `Error` on submission failure. All return `Request` (marked `[[nodiscard]]`).

| Method | Description |
|--------|-------------|
| `read(int fd, BufferRef buf, size_t len, off_t offset, F&& cb)` | Async read |
| `write(int fd, BufferRef buf, size_t len, off_t offset, F&& cb)` | Async write |
| `readv(int fd, std::span<const iovec> iov, off_t offset, F&& cb)` | Scatter read |
| `writev(int fd, std::span<const iovec> iov, off_t offset, F&& cb)` | Gather write |
| `fsync(int fd, F&& cb)` | Async fsync |
| `fdatasync(int fd, F&& cb)` | Async fdatasync |

The iovec array and all buffers referenced by iov must remain valid until the callback fires.

#### I/O Operations (Coroutine)

| Method | Returns | Description |
|--------|---------|-------------|
| `async_read(int fd, BufferRef buf, size_t len, off_t offset)` | `IoAwaitable` | `co_await` yields `ssize_t` |
| `async_write(int fd, BufferRef buf, size_t len, off_t offset)` | `IoAwaitable` | `co_await` yields `ssize_t` |
| `async_fsync(int fd)` | `FsyncAwaitable` | `co_await` completes on fsync |
| `async_fdatasync(int fd)` | `FsyncAwaitable` | `co_await` completes on fdatasync |

Coroutine awaitables throw `Error` on negative results when resumed.

#### Cancellation

| Method | Returns | Description |
|--------|---------|-------------|
| `cancel(Request& req) noexcept` | `bool` | `true` if cancellation submitted |

#### Event Processing

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `poll_fd() const` | `int` | `Error` | Get fd for epoll integration |
| `poll()` | `int` | `Error` | Process completions (non-blocking). Serialized with internal mutex. |
| `wait(int timeout_ms = -1)` | `int` | `Error` | Block until completion or timeout. Serialized with internal mutex. |
| `run()` | `void` | -- | Run event loop until `stop()`. Serialized with internal mutex. |
| `stop() noexcept` | `void` | -- | Signal event loop to stop (thread-safe) |
| `drain(int timeout_ms = -1)` | `int` | `Error` | Wait until all in-flight ops complete |

#### Buffer Management

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `allocate_buffer(size_t size)` | `Buffer` | `Error` | Allocate RAII buffer from pool |
| `register_buffers(std::span<const iovec>)` | `void` | `Error` | Register buffers with kernel |
| `unregister_buffers()` | `void` | `Error` | Unregister buffers (synchronous; degrades to deferred if called from callback) |
| `request_unregister_buffers()` | `void` | `Error` | Request deferred buffer unregister (callback-safe) |

#### File Registration

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `register_files(std::span<const int>)` | `void` | `Error` | Register fds with kernel |
| `update_file(int index, int fd)` | `void` | `Error` | Replace registered fd |
| `unregister_files()` | `void` | `Error` | Unregister all fds (synchronous; degrades to deferred if called from callback) |
| `request_unregister_files()` | `void` | `Error` | Request deferred file unregister (callback-safe) |

#### Statistics

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `get_stats() const` | `Stats` | -- | Aggregate engine stats |
| `ring_count() const noexcept` | `int` | -- | Number of rings |
| `get_ring_stats(int ring_idx) const` | `RingStats` | `std::out_of_range` | Per-ring stats |
| `get_histogram(int ring_idx) const` | `Histogram` | `std::out_of_range` | Latency histogram |
| `get_buffer_stats() const` | `BufferStats` | -- | Buffer pool stats |

#### Raw Access

| Method | Returns | Description |
|--------|---------|-------------|
| `handle() noexcept` | `auraio_engine_t*` | Underlying C handle |
| `handle() const noexcept` | `const auraio_engine_t*` | Underlying C handle (const) |
| `operator bool() const noexcept` | `bool` | `true` if valid |

---

### `auraio::Request`

Defined in `<auraio/request.hpp>`. Non-owning reference to an in-flight request. Valid from submission until callback begins.

| Method | Returns | Description |
|--------|---------|-------------|
| `pending() const noexcept` | `bool` | `true` if still in-flight |
| `fd() const noexcept` | `int` | Associated fd, or -1 |
| `handle() noexcept` | `auraio_request_t*` | Underlying C handle |
| `handle() const noexcept` | `const auraio_request_t*` | Underlying C handle (const) |
| `operator bool() const noexcept` | `bool` | `true` if handle is valid |

---

### `auraio::BufferRef`

Defined in `<auraio/buffer.hpp>`. Lightweight buffer descriptor (value type, no ownership).

| Constructor / Factory | Description |
|-----------------------|-------------|
| `BufferRef(void* ptr)` | Wrap unregistered buffer pointer |
| `explicit BufferRef(const void* ptr)` | Wrap const unregistered buffer pointer (for write operations only; explicit to prevent accidental use with reads) |
| `static BufferRef fixed(int index, size_t offset = 0)` | Reference a registered buffer |

| Method | Returns | Description |
|--------|---------|-------------|
| `is_registered() const noexcept` | `bool` | `true` if registered buffer |
| `c_buf() const noexcept` | `auraio_buf_t` | Underlying C descriptor |

Free functions: `buf(void* ptr)`, `buf_fixed(int index, size_t offset = 0)`.

---

### `auraio::Buffer`

Defined in `<auraio/buffer.hpp>`. RAII buffer from engine pool. Move-only. Automatically freed on destruction. If a `Buffer` outlives its `Engine`, AuraIO falls back to `free()` instead of touching destroyed engine state.

Created via `Engine::allocate_buffer(size)`.

| Method | Returns | Description |
|--------|---------|-------------|
| `data() noexcept` | `void*` | Buffer pointer |
| `data() const noexcept` | `const void*` | Const buffer pointer |
| `size() const noexcept` | `size_t` | Buffer size in bytes |
| `span()` | `std::span<std::byte>` | Buffer as byte span (throws `Error` if null) |
| `span() const` | `std::span<const std::byte>` | Const byte span (throws `Error` if null) |
| `as<T>()` | `std::span<T>` | Buffer as typed span (requires trivially copyable `T`; throws `Error` if null or misaligned) |
| `as<T>() const` | `std::span<const T>` | Const typed span |
| `ref() noexcept` | `BufferRef` | Convert to BufferRef (mutable) |
| `ref() const noexcept` | `BufferRef` | Convert to BufferRef (const, for writes only) |
| `operator BufferRef() noexcept` | `BufferRef` | Implicit conversion (mutable only; const conversion is deleted) |
| `operator bool() const noexcept` | `bool` | `true` if valid |
| `owned() const noexcept` | `bool` | `true` if buffer will be freed on destruction |
| `release() noexcept` | `ReleasedBuffer` | Release ownership; returns `{data, size, engine}` for manual freeing |
| `static wrap(void*, size_t) noexcept` | `Buffer` | Non-owning wrapper |

---

### Statistics Classes

All defined in `<auraio/stats.hpp>`. All getters are `[[nodiscard]] noexcept`.

#### `auraio::Stats`

| Method | Returns | C Field |
|--------|---------|---------|
| `ops_completed()` | `int64_t` | `ops_completed` |
| `bytes_transferred()` | `int64_t` | `bytes_transferred` |
| `throughput_bps()` | `double` | `current_throughput_bps` |
| `p99_latency_ms()` | `double` | `p99_latency_ms` |
| `current_in_flight()` | `int` | `current_in_flight` |
| `optimal_in_flight()` | `int` | `optimal_in_flight` |
| `optimal_batch_size()` | `int` | `optimal_batch_size` |
| `adaptive_spills()` | `uint64_t` | `adaptive_spills` |
| `c_stats()` | `const auraio_stats_t&` | Full C struct |

#### `auraio::RingStats`

| Method | Returns | C Field |
|--------|---------|---------|
| `ops_completed()` | `int64_t` | `ops_completed` |
| `bytes_transferred()` | `int64_t` | `bytes_transferred` |
| `pending_count()` | `int` | `pending_count` |
| `in_flight_limit()` | `int` | `in_flight_limit` |
| `batch_threshold()` | `int` | `batch_threshold` |
| `p99_latency_ms()` | `double` | `p99_latency_ms` |
| `throughput_bps()` | `double` | `throughput_bps` |
| `aimd_phase()` | `int` | `aimd_phase` |
| `aimd_phase_name()` | `const char*` | via `auraio_phase_name()` |
| `queue_depth()` | `int` | `queue_depth` |
| `ring_index()` | `int` | (set by Engine) |
| `c_stats()` | `const auraio_ring_stats_t&` | Full C struct |

#### `auraio::Histogram`

**Constants:**

| Constant | Value |
|----------|-------|
| `Histogram::bucket_count` | 200 |
| `Histogram::bucket_width_us` | 50 |

**Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `bucket(int idx)` | `uint32_t` | Count at bucket index (0 for out-of-range) |
| `overflow()` | `uint32_t` | Count exceeding max tracked latency |
| `total_count()` | `uint32_t` | Total samples |
| `max_tracked_us()` | `int` | Maximum tracked latency (us) |
| `bucket_lower_us(int idx)` | `int` | Lower bound of bucket (us). 0 for out-of-range |
| `bucket_upper_us(int idx)` | `int` | Upper bound of bucket (us). 0 for out-of-range |
| `c_histogram()` | `const auraio_histogram_t&` | Full C struct |

#### `auraio::BufferStats`

| Method | Returns | C Field |
|--------|---------|---------|
| `total_allocated_bytes()` | `size_t` | `total_allocated_bytes` |
| `total_buffers()` | `size_t` | `total_buffers` |
| `shard_count()` | `int` | `shard_count` |
| `c_stats()` | `const auraio_buffer_stats_t&` | Full C struct |

---

### Coroutine Support

Defined in `<auraio/coro.hpp>`. Requires C++20 coroutines.

#### `auraio::Task<T>`

Lazy coroutine producing a value of type `T`. Use `Task<void>` (or `Task<>`) for coroutines that do not return a value. Move-only.

The `Task` object **must remain alive** until the coroutine completes. Destroying a Task while an async I/O operation is pending calls `std::terminate()`.

```cpp
Task<ssize_t> read_file(Engine& engine, int fd) {
    auto buf = engine.allocate_buffer(4096);
    ssize_t n = co_await engine.async_read(fd, buf, 4096, 0);
    co_return n;
}

// Drive the coroutine:
auto task = read_file(engine, fd);
while (!task.done()) {
    engine.wait();
    task.resume();
}
ssize_t result = task.get();
```

| Method | Returns | Description |
|--------|---------|-------------|
| `resume()` | `bool` | Resume coroutine. `true` if still running |
| `done() const noexcept` | `bool` | `true` if coroutine finished |
| `get()` | `T` | Get result (destructive move). Throws if coroutine threw, is not done, or result was already consumed |

Tasks can be composed with `co_await`: `T result = co_await some_task;`

#### `auraio::IoAwaitable`

Returned by `Engine::async_read` and `Engine::async_write`. Not constructed directly.

When `co_await`ed, submits the I/O operation and suspends the coroutine. Resumes with `ssize_t` (bytes transferred). Throws `Error` if the result is negative.

#### `auraio::FsyncAwaitable`

Returned by `Engine::async_fsync` and `Engine::async_fdatasync`. Not constructed directly.

When `co_await`ed, submits the fsync and suspends. Resumes with `void`. Throws `Error` if the result is negative.

---

## Rust API

Crate: `auraio` (safe bindings) + `auraio-sys` (raw FFI)

Requires: Rust edition 2021, `libauraio` and `liburing` linked

### Rust Error Handling

The Rust API uses `Result<T, auraio::Error>` (aliased as `auraio::Result<T>`) instead of exceptions.

#### `auraio::Error`

```rust
pub enum Error {
    /// I/O error from the kernel
    Io(std::io::Error),
    /// Engine creation failed
    EngineCreate(std::io::Error),
    /// Buffer allocation failed
    BufferAlloc(std::io::Error),
    /// Submission failed (queue full or shutdown)
    Submission(std::io::Error),
    /// Operation was cancelled (maps from ECANCELED)
    Cancelled,
    /// Invalid argument
    InvalidArgument(&'static str),
}
```

The `Error::from_raw_os_error(code)` constructor automatically maps `ECANCELED` to `Error::Cancelled`; all other errno values become `Error::Io`.

`Error` implements `std::error::Error`, `Send`, and `Sync`.

`auraio::Result<T>` is a type alias for `std::result::Result<T, Error>`.

---

### Options

```rust
use auraio::Options;

let opts = Options::new()
    .queue_depth(512)
    .ring_count(4)
    .enable_sqpoll(true);
```

`Options` implements `Clone`, `Debug`, and `Default`.

**Builder methods** (all consume and return `Self`):

| Method | Parameter | Description |
|--------|-----------|-------------|
| `queue_depth(i32)` | depth | Queue depth per ring (default: 256) |
| `ring_count(i32)` | count | Number of rings (0 = auto) |
| `initial_in_flight(i32)` | limit | Starting in-flight limit |
| `min_in_flight(i32)` | limit | Minimum in-flight limit |
| `max_p99_latency_ms(f64)` | latency | Target P99 latency (0 = auto) |
| `buffer_alignment(usize)` | alignment | Buffer alignment (default: 4096) |
| `disable_adaptive(bool)` | disable | Disable AIMD tuning |
| `enable_sqpoll(bool)` | enable | Enable SQPOLL mode |
| `sqpoll_idle_ms(i32)` | timeout | SQPOLL idle timeout |
| `ring_select(RingSelect)` | mode | Ring selection mode |

#### `auraio::RingSelect`

Not re-exported at crate root; defined in `auraio::options`. The `Options::ring_select()` method accepts this enum.

```rust
pub enum RingSelect {
    Adaptive,
    CpuLocal,
    RoundRobin,
}
```

---

### Engine

```rust
use auraio::Engine;

let engine = Engine::new()?;
// or
let engine = Engine::with_options(&opts)?;
```

`Engine` is `Send + Sync`. Multiple threads can submit I/O concurrently. Drop calls `auraio_destroy()` to wait for pending I/O and free resources.

#### Lifecycle

| Method | Returns | Description |
|--------|---------|-------------|
| `Engine::new()` | `Result<Engine>` | Create with default options |
| `Engine::with_options(&Options)` | `Result<Engine>` | Create with custom options |
| `as_ptr()` | `*mut auraio_engine_t` | Raw C handle |

#### Buffer Management

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `allocate_buffer(usize)` | `Result<Buffer>` | safe | Allocate page-aligned RAII buffer |
| `register_buffers(&[&mut [u8]])` | `Result<()>` | **unsafe** | Register buffers with kernel for zero-copy I/O. Buffers must remain valid at stable addresses until `unregister_buffers()`. |
| `unregister_buffers()` | `Result<()>` | safe | Unregister previously registered buffers |
| `request_unregister_buffers()` | `Result<()>` | safe | Deferred unregister (callback-safe) |

#### File Registration

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `register_files(&[RawFd])` | `Result<()>` | safe | Register fds with kernel |
| `update_file(i32, RawFd)` | `Result<()>` | safe | Replace a registered fd |
| `unregister_files()` | `Result<()>` | safe | Unregister all fds |
| `request_unregister_files()` | `Result<()>` | safe | Deferred unregister (callback-safe) |

#### I/O Operations

The callback type is `FnOnce(Result<usize>) + Send + 'static`. The callback receives `Ok(bytes_transferred)` on success, `Err(Error::Cancelled)` on cancellation, or `Err(Error::Io(...))` on other failures.

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `read(fd, BufferRef, len, offset, callback)` | `Result<RequestHandle>` | **unsafe** | Async read. Buffer memory referenced by `BufferRef` must remain valid until callback fires. |
| `write(fd, BufferRef, len, offset, callback)` | `Result<RequestHandle>` | **unsafe** | Async write. Same buffer lifetime requirement. |
| `readv(fd, &[iovec], offset, callback)` | `Result<RequestHandle>` | **unsafe** | Scatter read. Both the iovec array and the buffers it references must remain valid until callback fires. |
| `writev(fd, &[iovec], offset, callback)` | `Result<RequestHandle>` | **unsafe** | Gather write. Same lifetime requirement as `readv`. |
| `fsync(fd, callback)` | `Result<RequestHandle>` | safe | Async fsync |
| `fdatasync(fd, callback)` | `Result<RequestHandle>` | safe | Async fdatasync |

**Why `unsafe`?** The `read`, `write`, `readv`, and `writev` methods are `unsafe` because `BufferRef` is `Copy` and carries no lifetime parameter. The compiler cannot verify that the referenced memory outlives the I/O operation. The caller must manually ensure buffers remain valid until the callback fires.

`fsync` and `fdatasync` are safe because they do not reference user buffers.

#### Cancellation

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `cancel(&RequestHandle)` | `Result<()>` | **unsafe** | Cancel pending operation (best-effort). Caller must ensure handle is still valid. |

#### Event Processing

| Method | Returns | Description |
|--------|---------|-------------|
| `poll_fd()` | `Result<RawFd>` | Get pollable fd for event loop integration |
| `poll()` | `Result<usize>` | Process completions (non-blocking). Serialized with internal `Mutex`. |
| `wait(i32)` | `Result<usize>` | Block until completion or timeout (-1 = forever). Serialized with internal `Mutex`. |
| `run()` | `()` | Run event loop until `stop()`. Serialized with internal `Mutex`. |
| `stop()` | `()` | Signal event loop to stop (thread-safe) |

#### Statistics

| Method | Returns | Description |
|--------|---------|-------------|
| `stats()` | `Stats` | Get aggregate engine statistics |

#### Free Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `auraio::version()` | `&'static str` | Library version string |
| `auraio::version_int()` | `i32` | Library version as integer |

---

### Buffer and BufferRef

#### `auraio::Buffer`

RAII buffer allocated from the engine's pool. Automatically freed on drop. `Send` but not `Sync`. If a `Buffer` outlives its `Engine`, the buffer is still freed safely (the engine's inner handle is reference-counted with `Arc`).

| Method | Returns | Description |
|--------|---------|-------------|
| `as_slice()` | `&[u8]` | Buffer as byte slice |
| `as_mut_slice()` | `&mut [u8]` | Buffer as mutable byte slice |
| `as_ptr()` | `*const u8` | Raw pointer (page-aligned) |
| `as_mut_ptr()` | `*mut u8` | Raw mutable pointer |
| `len()` | `usize` | Buffer size in bytes |
| `is_empty()` | `bool` | True if zero-length |
| `to_ref()` | `BufferRef` | Convert to BufferRef |

`Buffer` implements `AsRef<[u8]>` and `AsMut<[u8]>`.

`From<&Buffer>` and `From<&mut Buffer>` are implemented for `BufferRef`, allowing `(&buf).into()` to create a `BufferRef`.

#### `auraio::BufferRef`

Lightweight buffer descriptor. `Copy + Clone + Debug`. Can represent either an unregistered buffer pointer or a registered buffer index.

**Lifetime warning:** `BufferRef` carries no lifetime parameter. The compiler cannot enforce that the underlying memory outlives the I/O operation. Callers must manually ensure buffers remain valid until the completion callback fires.

| Constructor | Safety | Description |
|-------------|--------|-------------|
| `BufferRef::from_ptr(*mut c_void)` | **unsafe** | From raw pointer |
| `BufferRef::from_slice(&[u8])` | **unsafe** | From byte slice |
| `BufferRef::from_mut_slice(&mut [u8])` | **unsafe** | From mutable byte slice |
| `BufferRef::fixed(i32, usize)` | safe | Reference registered buffer at index + offset |
| `BufferRef::fixed_index(i32)` | safe | Reference registered buffer at offset 0 |
| `From<&Buffer>` | safe | From Buffer reference |
| `From<&mut Buffer>` | safe | From mutable Buffer reference |

---

### RequestHandle

Handle to a pending I/O request. `Send` but not `Sync`. `Debug`.

**Lifetime warning:** The handle becomes invalid the moment the completion callback begins. Using it after that point is undefined behavior.

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `is_pending()` | `bool` | **unsafe** | True if still in-flight. Caller must ensure handle is valid. |
| `fd()` | `RawFd` | **unsafe** | Associated file descriptor. Caller must ensure handle is valid. |

---

### Stats

Engine statistics snapshot. `Clone + Debug + Default`.

| Method | Returns | Description |
|--------|---------|-------------|
| `ops_completed()` | `i64` | Total operations completed |
| `bytes_transferred()` | `i64` | Total bytes transferred |
| `throughput_bps()` | `f64` | Current throughput (bytes/sec) |
| `p99_latency_ms()` | `f64` | 99th percentile latency (ms) |
| `current_in_flight()` | `i32` | Current in-flight operations |
| `optimal_in_flight()` | `i32` | AIMD-tuned in-flight limit |
| `optimal_batch_size()` | `i32` | AIMD-tuned batch size |
| `adaptive_spills()` | `u64` | ADAPTIVE mode spill count |

---

### Async Support

Feature-gated behind `async` feature flag. Provides `Future`-based async I/O that integrates with any async runtime (tokio, async-std, smol, etc.).

#### `AsyncEngine` trait

Automatically implemented for `Engine` when the `async` feature is enabled.

```rust
use auraio::{Engine, async_io::AsyncEngine};

// Submit I/O and get a Future:
let future = unsafe { engine.async_read(fd, &buf, 4096, 0) }?;
let bytes_read = future.await?;
```

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `async_read(fd, impl Into<BufferRef>, len, offset)` | `Result<IoFuture>` | **unsafe** | Async read returning a Future |
| `async_write(fd, impl Into<BufferRef>, len, offset)` | `Result<IoFuture>` | **unsafe** | Async write returning a Future |
| `async_fsync(fd)` | `Result<IoFuture>` | safe | Async fsync returning a Future |
| `async_fdatasync(fd)` | `Result<IoFuture>` | safe | Async fdatasync returning a Future |
| `async_readv(fd, &[iovec], offset)` | `Result<IoFuture>` | **unsafe** | Async scatter read returning a Future |
| `async_writev(fd, &[iovec], offset)` | `Result<IoFuture>` | **unsafe** | Async gather write returning a Future |

#### `IoFuture`

A `Future` that completes when the I/O operation finishes. `Send`. Resolves to `Result<usize>`.

**Cancellation warning:** Dropping an `IoFuture` does **not** cancel the underlying kernel I/O operation. The callback will still fire. If the buffer is freed before the operation completes, the kernel will write into freed memory. Callers using `select!` or similar patterns must ensure buffers outlive the I/O operation, not just the future.

**Runtime integration:** The futures require the engine to be polled for completions. Either spawn a background task calling `engine.wait()` in a loop, or integrate with your runtime using `engine.poll_fd()`.

---

## Error Codes Reference

Error codes used across the C, C++, and Rust APIs:

| errno | C API | C++ API | Rust API | When |
|-------|-------|---------|----------|------|
| `EINVAL` | errno set, returns `NULL`/-1 | throws `Error` (`.is_invalid()`) | `Error::Io` / `Error::InvalidArgument` | Invalid parameters (NULL engine, NULL buffer, invalid ring index) |
| `EAGAIN` | errno set, returns `NULL` | throws `Error` (`.is_again()`) | `Error::Submission` | Ring is at capacity; retry later |
| `ESHUTDOWN` | errno set, returns `NULL`/-1 | throws `Error` (`.is_shutdown()`) | `Error::Submission` | Engine is shutting down (after `auraio_destroy()` called) |
| `ENOENT` | errno set, returns `NULL` | throws `Error` (`.is_not_found()`) | `Error::Submission` | No ring available for the operation |
| `EOVERFLOW` | errno set, returns `NULL` | throws `Error` | `Error::Submission` | Queue overflow |
| `ENOMEM` | errno set, returns `NULL`/-1 | throws `Error` | `Error::Submission` / `Error::BufferAlloc` | Memory allocation failed |
| `EBUSY` | errno set, returns `NULL` | throws `Error` (`.is_busy()`) | `Error::Io` | Fixed-buffer submission while deferred unregister is draining |
| `ECANCELED` | callback `result = -ECANCELED` | `Error` (`.is_cancelled()`) | `Error::Cancelled` | Operation was cancelled via `cancel()` |
| `ETIMEDOUT` | errno set, returns -1 | throws `Error` | `Error::Io` | `drain()` timeout exceeded |
| `EALREADY` | errno set, returns -1 | (cancel returns `false`) | `Error::Io` | Cancel called on already-completed request |

---

## Threading Model

AuraIO uses a **multi-submitter, single-poller** threading model:

- **Submissions are thread-safe.** Any number of threads may call `auraio_read()`, `auraio_write()`, `auraio_fsync()`, etc. concurrently. Each ring has its own submission lock (`lock`) to protect the submission queue and request pool.

- **Event processing is single-threaded.** `auraio_poll()`, `auraio_wait()`, and `auraio_run()` must not be called concurrently on the same engine. In the C++ and Rust bindings, this is enforced by an internal `Mutex` that serializes concurrent poll/wait/run calls (they block rather than race). In the C API, concurrent event processing is undefined behavior.

- **Statistics are thread-safe.** All stats functions (`auraio_get_stats()`, `auraio_get_ring_stats()`, etc.) are safe to call from any thread at any time.

- **Callbacks execute in the polling thread.** Callbacks fire during `auraio_poll()` or `auraio_wait()`. A callback may fire on any thread that polls the engine, not necessarily the thread that submitted the I/O.

- **Buffer pool is thread-safe.** `auraio_buffer_alloc()` and `auraio_buffer_free()` can be called from any thread. The pool uses per-thread caching for performance.

- **`auraio_stop()` is thread-safe.** Can be called from any thread or from within a callback.

- **`auraio_destroy()` should be called after joining worker threads.** While it handles in-flight I/O gracefully, the caller must ensure no other thread is actively submitting I/O or using the buffer pool.

---

## Callback Rules

Rules that apply to completion callbacks across all API surfaces:

1. **Callbacks may submit new I/O.** It is safe to call `auraio_read()`, `auraio_write()`, `auraio_fsync()`, etc. from within a callback.

2. **Callbacks must not call `auraio_destroy()`.** This causes undefined behavior.

3. **Callbacks must not call `auraio_poll()`/`auraio_wait()`/`auraio_run()`.** These are already running on the call stack and must not be re-entered. In C++ and Rust, attempting this would deadlock on the internal mutex.

4. **Callbacks should complete quickly.** Long-running callbacks block other completions from being processed.

5. **The request handle is valid only during the callback.** After the callback returns, the handle is recycled and must not be used.

6. **`user_data` and buffer memory must remain valid until callback fires.** The library does not copy buffers or user data.

7. **Deferred unregister is safe from callbacks.** Use `auraio_request_unregister_buffers()` or `auraio_request_unregister_files()` instead of the synchronous variants when calling from within a callback. The synchronous variants (`auraio_unregister_buffers()`, `auraio_unregister_files()`) automatically degrade to the deferred behavior when called from a callback context.

8. **C++ callbacks must not throw.** Exceptions from callbacks trigger `std::terminate()` because they cannot propagate through the C callback trampoline.

9. **Rust callbacks are `FnOnce`.** Each callback is invoked exactly once. Captured values are dropped after invocation.
