# AuraIO API Reference

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

Passing `NULL` as a callback is permitted — the I/O executes but no completion notification is delivered.

Callbacks execute in the context of `auraio_poll()` or `auraio_wait()`. Since these functions drain completions from **all** rings, a callback may fire on **any thread** that calls `auraio_wait()` — not necessarily the thread that submitted the I/O. Do not use thread-local storage to identify the originating context; instead, pass all necessary state through `user_data`. The `user_data` pointer (and any memory it references) must remain valid until the callback has executed. Callbacks may submit new I/O but must not call `auraio_destroy()`.

### Configuration

#### `auraio_options_t`

```c
typedef struct {
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

#### `auraio_ring_select_t`

```c
typedef enum {
  AURAIO_SELECT_ADAPTIVE = 0,   /* CPU-local with overflow spilling (default) */
  AURAIO_SELECT_CPU_LOCAL,      /* CPU-affinity only (best NUMA locality) */
  AURAIO_SELECT_ROUND_ROBIN     /* Atomic round-robin across all rings */
} auraio_ring_select_t;
```

| Mode | When to use |
|------|-------------|
| `AURAIO_SELECT_ADAPTIVE` | Default. Uses CPU-local ring until congested (>75% of in-flight limit). If local load > 2x average, stays local (outlier). Otherwise, spills via power-of-two random choice to the lighter of two non-local rings. Best for most workloads. |
| `AURAIO_SELECT_CPU_LOCAL` | Strict CPU affinity. Best for NUMA-sensitive workloads where cross-node traffic is expensive. Single-thread throughput is limited to one ring. |
| `AURAIO_SELECT_ROUND_ROBIN` | Always distributes via atomic round-robin. Best for single-thread event loops or benchmarks that need maximum ring utilization from fewer threads. |

Always initialize with `auraio_options_init()` before modifying fields.

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

Create with inline helpers — do not construct manually:

| Helper | Description |
|--------|-------------|
| `auraio_buf(void *ptr)` | Wrap a regular buffer pointer |
| `auraio_buf_fixed(int index, size_t offset)` | Reference a registered buffer at offset |
| `auraio_buf_fixed_idx(int index)` | Reference a registered buffer at offset 0 |

### Fsync Flags

```c
typedef enum {
  AURAIO_FSYNC_DEFAULT  = 0,   /* Full fsync (metadata + data) */
  AURAIO_FSYNC_DATASYNC = 1,   /* fdatasync (data only) */
} auraio_fsync_flags_t;
```

### Statistics Types

#### `auraio_stats_t` — Aggregate Engine Stats

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

#### `auraio_ring_stats_t` — Per-Ring Stats

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

#### `auraio_histogram_t` — Latency Histogram

An approximate snapshot of the active histogram window. Because the snapshot is read from a concurrently-written histogram, individual bucket values are atomic but `total_count` may differ slightly from the sum of buckets + overflow. See [Observability Guide](observability.md) for details.

| Field | Type | Description |
|-------|------|-------------|
| `buckets[200]` | `uint32_t[]` | Latency frequency buckets |
| `overflow` | `uint32_t` | Count of operations exceeding `max_tracked_us` |
| `total_count` | `uint32_t` | Total samples in this snapshot |
| `bucket_width_us` | `int` | Width of each bucket in microseconds |
| `max_tracked_us` | `int` | Maximum tracked latency in microseconds |

#### `auraio_buffer_stats_t` — Buffer Pool Stats

| Field | Type | Description |
|-------|------|-------------|
| `total_allocated_bytes` | `size_t` | Total bytes currently allocated from pool |
| `total_buffers` | `size_t` | Total buffer count currently allocated |
| `shard_count` | `int` | Number of pool shards |

### Constants

#### Version

| Macro | Value | Description |
|-------|-------|-------------|
| `AURAIO_VERSION_MAJOR` | 1 | Major version |
| `AURAIO_VERSION_MINOR` | 0 | Minor version |
| `AURAIO_VERSION_PATCH` | 1 | Patch version |
| `AURAIO_VERSION` | 10001 | Combined: `major * 10000 + minor * 100 + patch` |
| `AURAIO_VERSION_STRING` | `"1.0.1"` | Version string |

#### AIMD Phase Constants

Used in `auraio_ring_stats_t.aimd_phase`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `AURAIO_PHASE_BASELINE` | 0 | Collecting baseline latency samples |
| `AURAIO_PHASE_PROBING` | 1 | Additive increase — testing higher depth |
| `AURAIO_PHASE_STEADY` | 2 | Throughput plateau — holding position |
| `AURAIO_PHASE_BACKOFF` | 3 | Multiplicative decrease — P99 exceeded target |
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

---

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

---

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

---

##### `auraio_write`

```c
auraio_request_t *auraio_write(auraio_engine_t *engine, int fd, auraio_buf_t buf,
                               size_t len, off_t offset,
                               auraio_callback_t callback, void *user_data);
```

Submit an async write. Same parameters as `auraio_read`.

---

##### `auraio_readv`

```c
auraio_request_t *auraio_readv(auraio_engine_t *engine, int fd,
                               const struct iovec *iov, int iovcnt,
                               off_t offset, auraio_callback_t callback,
                               void *user_data);
```

Submit an async scatter read into multiple buffers.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `iov` | `const struct iovec *` | Array of iovec structures |
| `iovcnt` | `int` | Number of elements in `iov` |
| `offset` | `off_t` | File offset |
| `callback` | `auraio_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

---

##### `auraio_writev`

```c
auraio_request_t *auraio_writev(auraio_engine_t *engine, int fd,
                                const struct iovec *iov, int iovcnt,
                                off_t offset, auraio_callback_t callback,
                                void *user_data);
```

Submit an async gather write from multiple buffers. Same parameters as `auraio_readv`.

---

##### `auraio_fsync`

```c
auraio_request_t *auraio_fsync(auraio_engine_t *engine, int fd,
                               auraio_callback_t callback, void *user_data);
```

Submit an async fsync (flushes all previous writes to storage).

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `callback` | `auraio_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

---

##### `auraio_fsync_ex`

```c
auraio_request_t *auraio_fsync_ex(auraio_engine_t *engine, int fd,
                                  auraio_fsync_flags_t flags,
                                  auraio_callback_t callback, void *user_data);
```

Submit an async fsync with flags. Use `AURAIO_FSYNC_DATASYNC` for fdatasync behavior.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `flags` | `auraio_fsync_flags_t` | `AURAIO_FSYNC_DEFAULT` or `AURAIO_FSYNC_DATASYNC` |
| `callback` | `auraio_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

---

#### Request Management

---

##### `auraio_cancel`

```c
int auraio_cancel(auraio_engine_t *engine, auraio_request_t *req);
```

Cancel a pending I/O operation (best-effort). If cancelled, the callback receives `result = -ECANCELED`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `req` | `auraio_request_t *` | Request to cancel |

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

---

##### `auraio_get_poll_fd`

```c
int auraio_get_poll_fd(auraio_engine_t *engine);
```

Get a pollable file descriptor for event loop integration. Becomes readable when completions are available. Add to epoll/select, then call `auraio_poll()` when readable.

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

Signal the event loop to stop. Thread-safe — can be called from any thread or from within a callback. `auraio_run()` returns after processing current completions.

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

---

##### `auraio_buffer_alloc`

```c
void *auraio_buffer_alloc(auraio_engine_t *engine, size_t size);
```

Allocate a page-aligned buffer from the engine's pool. Suitable for `O_DIRECT` I/O.

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

Return a buffer to the engine's pool. `size` must match the original allocation. `buf` may be `NULL`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `buf` | `void *` | Buffer to free (may be `NULL`) |
| `size` | `size_t` | Must match original allocation size |

---

##### `auraio_register_buffers`

```c
int auraio_register_buffers(auraio_engine_t *engine, const struct iovec *iovs, int count);
```

Pre-register buffers with the kernel for zero-copy I/O. Call once at startup. After registration, use `auraio_buf_fixed()` to reference buffers by index in read/write calls.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `iovs` | `const struct iovec *` | Array of buffer descriptors |
| `count` | `int` | Number of buffers |

**Returns:** 0 on success, -1 on error (errno set).

---

##### `auraio_unregister_buffers`

```c
int auraio_unregister_buffers(auraio_engine_t *engine);
```

Unregister previously registered buffers. No in-flight operations may be using registered buffers when this is called.

**Returns:** 0 on success, -1 on error.

---

#### File Registration

---

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

**Returns:** 0 on success, -1 on error (errno set). On failure, ring state may be inconsistent — call `auraio_unregister_files()` and re-register.

---

##### `auraio_unregister_files`

```c
int auraio_unregister_files(auraio_engine_t *engine);
```

Unregister all previously registered files.

**Returns:** 0 on success, -1 on error.

---

#### Statistics

All stats functions are thread-safe and safe to call during active I/O. See [Observability Guide](observability.md) for monitoring patterns, polling rate recommendations, and Prometheus integration.

---

##### `auraio_get_stats`

```c
void auraio_get_stats(auraio_engine_t *engine, auraio_stats_t *stats);
```

Get engine-wide aggregate statistics.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `stats` | `auraio_stats_t *` | Output struct |

---

##### `auraio_get_ring_count`

```c
int auraio_get_ring_count(auraio_engine_t *engine);
```

**Returns:** Number of io_uring rings, or 0 if engine is `NULL`.

---

##### `auraio_get_ring_stats`

```c
int auraio_get_ring_stats(auraio_engine_t *engine, int ring_idx,
                          auraio_ring_stats_t *stats);
```

Get per-ring statistics.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `ring_idx` | `int` | Ring index (0 to `auraio_get_ring_count()-1`) |
| `stats` | `auraio_ring_stats_t *` | Output struct |

**Returns:** 0 on success. -1 if engine/stats is `NULL` or `ring_idx` is out of range (stats zeroed on invalid index).

---

##### `auraio_get_histogram`

```c
int auraio_get_histogram(auraio_engine_t *engine, int ring_idx,
                         auraio_histogram_t *hist);
```

Get a latency histogram snapshot for a ring. The snapshot is approximate — see `auraio_histogram_t` documentation.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
| `ring_idx` | `int` | Ring index (0 to `auraio_get_ring_count()-1`) |
| `hist` | `auraio_histogram_t *` | Output struct |

**Returns:** 0 on success. -1 if engine/hist is `NULL` or `ring_idx` is out of range (hist zeroed on invalid index).

---

##### `auraio_get_buffer_stats`

```c
int auraio_get_buffer_stats(auraio_engine_t *engine, auraio_buffer_stats_t *stats);
```

Get buffer pool statistics. Lockless — reads atomic counters.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `auraio_engine_t *` | Engine handle |
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

---

##### `auraio_version`

```c
const char *auraio_version(void);
```

**Returns:** Version string (e.g., `"1.0.1"`).

---

##### `auraio_version_int`

```c
int auraio_version_int(void);
```

**Returns:** Version as integer: `major * 10000 + minor * 100 + patch` (e.g., 10001).

---

## C++ API

Header: `#include <auraio.hpp>` (includes all sub-headers)

Requires: C++20 (`-std=c++20`)

Namespace: `auraio`

### Error Handling

The C++ API uses exceptions. All I/O and lifecycle operations throw `auraio::Error` on failure. Per-ring stats/histogram accessors throw `std::out_of_range` on invalid ring index.

### `auraio::Error`

Defined in `<auraio/error.hpp>`. Inherits `std::exception`.

```cpp
class Error : public std::exception {
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
| `disable_adaptive(bool)` | disable | Disable AIMD tuning |
| `enable_sqpoll(bool)` | enable | Enable SQPOLL mode |
| `sqpoll_idle_ms(int)` | ms | SQPOLL idle timeout |
| `ring_select(RingSelect)` | mode | Ring selection mode |

`RingSelect` enum: `auraio::RingSelect::Adaptive`, `CpuLocal`, `RoundRobin`.

**Getters** (all `[[nodiscard]] noexcept`): same names as setters, no arguments.

---

### `auraio::Engine`

Defined in `<auraio/engine.hpp>`. Main engine class. Move-only.

#### Constructors

| Signature | Description |
|-----------|-------------|
| `Engine()` | Create with default options. Throws `Error`. |
| `explicit Engine(const Options& opts)` | Create with custom options. Throws `Error`. |
| `Engine(Engine&&) noexcept` | Move constructor |
| `~Engine()` | Waits for pending I/O, then destroys |

Non-copyable.

#### I/O Operations (Callback)

All template on `Callback` concept: `std::invocable<F, Request&, ssize_t>`.

All throw `Error` on submission failure.

| Method | Returns | Description |
|--------|---------|-------------|
| `read(int fd, BufferRef buf, size_t len, off_t offset, F&& cb)` | `Request*` | Async read |
| `write(int fd, BufferRef buf, size_t len, off_t offset, F&& cb)` | `Request*` | Async write |
| `readv(int fd, std::span<const iovec> iov, off_t offset, F&& cb)` | `Request*` | Scatter read |
| `writev(int fd, std::span<const iovec> iov, off_t offset, F&& cb)` | `Request*` | Gather write |
| `fsync(int fd, F&& cb)` | `Request*` | Async fsync |
| `fdatasync(int fd, F&& cb)` | `Request*` | Async fdatasync |

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
| `cancel(Request* req) noexcept` | `bool` | `true` if cancellation submitted |

#### Event Processing

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `poll_fd() const` | `int` | `Error` | Get fd for epoll integration |
| `poll()` | `int` | — | Process completions (non-blocking) |
| `wait(int timeout_ms = -1)` | `int` | `Error` | Block until completion or timeout |
| `run()` | `void` | — | Run event loop until `stop()` |
| `stop() noexcept` | `void` | — | Signal event loop to stop (thread-safe) |
| `drain(int timeout_ms = -1)` | `int` | `Error` | Wait until all in-flight ops complete |

#### Buffer Management

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `allocate_buffer(size_t size)` | `Buffer` | `Error` | Allocate RAII buffer from pool |
| `register_buffers(std::span<const iovec>)` | `void` | `Error` | Register buffers with kernel |
| `unregister_buffers()` | `void` | `Error` | Unregister buffers |

#### File Registration

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `register_files(std::span<const int>)` | `void` | `Error` | Register fds with kernel |
| `update_file(int index, int fd)` | `void` | `Error` | Replace registered fd |
| `unregister_files()` | `void` | `Error` | Unregister all fds |

#### Statistics

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `get_stats() const` | `Stats` | — | Aggregate engine stats |
| `ring_count() const noexcept` | `int` | — | Number of rings |
| `get_ring_stats(int ring_idx) const` | `RingStats` | `std::out_of_range` | Per-ring stats |
| `get_histogram(int ring_idx) const` | `Histogram` | `std::out_of_range` | Latency histogram |
| `get_buffer_stats() const` | `BufferStats` | — | Buffer pool stats |

#### Raw Access

| Method | Returns | Description |
|--------|---------|-------------|
| `handle() noexcept` | `auraio_engine_t*` | Underlying C handle |
| `operator bool() const noexcept` | `bool` | `true` if valid |

---

### `auraio::Request`

Defined in `<auraio/request.hpp>`. Non-owning reference to an in-flight request. Valid from submission until callback begins.

| Method | Returns | Description |
|--------|---------|-------------|
| `pending() const noexcept` | `bool` | `true` if still in-flight |
| `fd() const noexcept` | `int` | Associated fd, or -1 |
| `handle() noexcept` | `auraio_request_t*` | Underlying C handle |
| `operator bool() const noexcept` | `bool` | `true` if handle is valid |

---

### `auraio::BufferRef`

Defined in `<auraio/buffer.hpp>`. Lightweight buffer descriptor (value type, no ownership).

| Constructor / Factory | Description |
|-----------------------|-------------|
| `BufferRef(void* ptr)` | Wrap unregistered buffer pointer |
| `BufferRef(const void* ptr)` | Wrap const unregistered buffer pointer |
| `static BufferRef fixed(int index, size_t offset = 0)` | Reference a registered buffer |

| Method | Returns | Description |
|--------|---------|-------------|
| `is_registered() const noexcept` | `bool` | `true` if registered buffer |
| `c_buf() const noexcept` | `auraio_buf_t` | Underlying C descriptor |

Free functions: `buf(void* ptr)`, `buf_fixed(int index, size_t offset = 0)`.

---

### `auraio::Buffer`

Defined in `<auraio/buffer.hpp>`. RAII buffer from engine pool. Move-only. Automatically freed on destruction.

Created via `Engine::allocate_buffer(size)`.

| Method | Returns | Description |
|--------|---------|-------------|
| `data() noexcept` | `void*` | Buffer pointer |
| `data() const noexcept` | `const void*` | Const buffer pointer |
| `size() const noexcept` | `size_t` | Buffer size in bytes |
| `span() noexcept` | `std::span<std::byte>` | Buffer as byte span |
| `span() const noexcept` | `std::span<const std::byte>` | Const byte span |
| `as<T>() noexcept` | `std::span<T>` | Buffer as typed span |
| `as<T>() const noexcept` | `std::span<const T>` | Const typed span |
| `ref() const noexcept` | `BufferRef` | Convert to BufferRef |
| `operator BufferRef() const noexcept` | `BufferRef` | Implicit conversion |
| `operator bool() const noexcept` | `bool` | `true` if valid |
| `owned() const noexcept` | `bool` | `true` if buffer will be freed on destruction |
| `release() noexcept` | `void*` | Release ownership (caller must free) |
| `static wrap(void*, size_t) noexcept` | `Buffer` | Non-owning wrapper |

---

### `auraio::Stats`

Defined in `<auraio/stats.hpp>`. Aggregate engine stats snapshot. All getters `[[nodiscard]] noexcept`.

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

---

### `auraio::RingStats`

Defined in `<auraio/stats.hpp>`. Per-ring stats snapshot. All getters `[[nodiscard]] noexcept`.

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

---

### `auraio::Histogram`

Defined in `<auraio/stats.hpp>`. Latency histogram snapshot. All getters `[[nodiscard]] noexcept`.

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

---

### `auraio::BufferStats`

Defined in `<auraio/stats.hpp>`. Buffer pool stats snapshot. All getters `[[nodiscard]] noexcept`.

| Method | Returns | C Field |
|--------|---------|---------|
| `total_allocated_bytes()` | `size_t` | `total_allocated_bytes` |
| `total_buffers()` | `size_t` | `total_buffers` |
| `shard_count()` | `int` | `shard_count` |
| `c_stats()` | `const auraio_buffer_stats_t&` | Full C struct |

---

### `auraio::Task<T>`

Defined in `<auraio/coro.hpp>`. Lazy coroutine producing a value of type `T`. Use `Task<void>` (or `Task<>`) for coroutines that don't return a value. Move-only.

The `Task` object **must remain alive** until the coroutine completes. Destroying a Task while an async I/O operation is pending is undefined behavior.

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
| `get()` | `T` | Get result. Throws if coroutine threw or is not done |

---

### `auraio::IoAwaitable`

Defined in `<auraio/coro.hpp>`. Returned by `Engine::async_read` and `Engine::async_write`. Not constructed directly.

When `co_await`ed, submits the I/O operation and suspends the coroutine. Resumes with `ssize_t` (bytes transferred). Throws `Error` if the result is negative.

---

### `auraio::FsyncAwaitable`

Defined in `<auraio/coro.hpp>`. Returned by `Engine::async_fsync` and `Engine::async_fdatasync`. Not constructed directly.

When `co_await`ed, submits the fsync and suspends. Resumes with `void`. Throws `Error` if the result is negative.
