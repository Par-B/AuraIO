# AuraIO API Reference

**Version 0.5.0**

AuraIO is a self-tuning async I/O library for Linux built on io_uring. It provides three API surfaces: a C11 core library, C++20 bindings with RAII and coroutine support, and Rust bindings with safe and async wrappers.

---

## Table of Contents

- [C API](#c-api)
  - [Opaque Types](#opaque-types)
  - [Callback Type](#callback-type)
  - [Configuration](#configuration)
  - [Buffer Descriptor](#buffer-descriptor)
  - [Fsync Flags](#fsync-flags)
  - [Flag Constants](#flag-constants)
  - [Enumerations](#enumerations)
  - [Statistics Types](#statistics-types)
  - [Constants](#constants)
  - [Functions](#functions)
- [C++ API](#c-api-1)
  - [Error Handling](#error-handling)
  - [Options](#auraoptions)
  - [Engine](#auraengine)
  - [Request](#aurarequest)
  - [BufferRef](#aurabufferref)
  - [Buffer](#aurabuffer)
  - [Statistics Classes](#statistics-classes)
  - [Coroutine Support](#coroutine-support)
  - [Free Functions](#free-functions)
- [Rust API](#rust-api)
  - [Error Handling](#rust-error-handling)
  - [Options](#options)
  - [Engine](#engine)
  - [Buffer and BufferRef](#buffer-and-bufferref)
  - [RequestHandle](#requesthandle)
  - [Stats](#stats)
  - [Logging](#logging)
  - [Async Support](#async-support)
- [Error Codes Reference](#error-codes-reference)
- [Threading Model](#threading-model)
- [Callback Rules](#callback-rules)

---

## C API

Header: `#include <aura.h>`

Link: `-laura -luring -lpthread`

### Opaque Types

| Type | Description |
|------|-------------|
| `aura_engine_t` | Engine handle. Created by `aura_create()`, destroyed by `aura_destroy()`. Thread-safe for submissions from multiple threads. |
| `aura_request_t` | Request handle. Returned by I/O submission functions. Valid from submission until the completion callback begins execution. Can be used with `aura_cancel()` and `aura_request_pending()` while in-flight. |

### Callback Type

```c
typedef void (*aura_callback_t)(aura_request_t *req, ssize_t result, void *user_data);
```

| Parameter | Type | Description |
|-----------|------|-------------|
| `req` | `aura_request_t *` | Request handle (valid only during callback) |
| `result` | `ssize_t` | Bytes transferred on success, negative errno on failure, `-ECANCELED` if cancelled |
| `user_data` | `void *` | User pointer from the submission call |

Passing `NULL` as a callback is permitted -- the I/O executes but no completion notification is delivered.

Callbacks execute in the context of `aura_poll()` or `aura_wait()`. Since these functions drain completions from **all** rings, a callback may fire on **any thread** that calls `aura_wait()` -- not necessarily the thread that submitted the I/O. Do not use thread-local storage to identify the originating context; instead, pass all necessary state through `user_data`. The `user_data` pointer (and any memory it references) must remain valid until the callback has executed. Callbacks may submit new I/O but must not call `aura_destroy()`.

### Configuration

#### `aura_options_t`

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
  aura_ring_select_t ring_select;
  bool single_thread;
  uint32_t _reserved[7];
} aura_options_t;
```

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `struct_size` | `size_t` | (set by init) | Set by `aura_options_init()`; used for ABI forward-compatibility |
| `queue_depth` | `int` | 1024 | Queue depth per io_uring ring |
| `ring_count` | `int` | 0 (auto) | Number of rings. 0 = one per CPU core |
| `initial_in_flight` | `int` | queue_depth/4 | Starting in-flight operation limit |
| `min_in_flight` | `int` | 4 | Floor for AIMD in-flight limit |
| `max_p99_latency_ms` | `double` | 0 (auto) | Target P99 latency threshold. When 0, AIMD derives the backoff threshold automatically: it measures the baseline P99 (sliding minimum over 500ms), then triggers backoff at 10x the baseline. A 10ms hard ceiling is used before the baseline is established. When set to a positive value, that value becomes the hard ceiling and AIMD backs off whenever P99 exceeds it. |
| `buffer_alignment` | `size_t` | system page size | Alignment for `aura_buffer_alloc()` |
| `disable_adaptive` | `bool` | false | Disable AIMD adaptive tuning |
| `enable_sqpoll` | `bool` | false | Use kernel-side submission polling (requires root or `CAP_SYS_NICE`) |
| `sqpoll_idle_ms` | `int` | 1000 | SQPOLL idle timeout before kernel thread sleeps |
| `ring_select` | `aura_ring_select_t` | `AURA_SELECT_ADAPTIVE` | Ring selection mode (see below) |
| `single_thread` | `bool` | false | Skip ring mutexes (caller guarantees single-thread access) |

Always initialize with `aura_options_init()` before modifying fields.

#### `aura_ring_select_t`

```c
typedef enum {
  AURA_SELECT_ADAPTIVE = 0,
  AURA_SELECT_CPU_LOCAL,
  AURA_SELECT_ROUND_ROBIN
} aura_ring_select_t;
```

| Mode | When to use |
|------|-------------|
| `AURA_SELECT_ADAPTIVE` | Default. Uses CPU-local ring until congested (>75% of in-flight limit). If local load > 2x average, stays local (outlier). Otherwise, spills via power-of-two random choice to the lighter of two non-local rings. Best for most workloads. |
| `AURA_SELECT_CPU_LOCAL` | Strict CPU affinity. Best for NUMA-sensitive workloads where cross-node traffic is expensive. Single-thread throughput is limited to one ring. |
| `AURA_SELECT_ROUND_ROBIN` | Always distributes via atomic round-robin. Best for single-thread event loops or benchmarks that need maximum ring utilization from fewer threads. |

### Buffer Descriptor

```c
typedef enum {
  AURA_BUF_UNREGISTERED = 0,
  AURA_BUF_REGISTERED = 1
} aura_buf_type_t;

typedef struct {
  aura_buf_type_t type;
  union {
    void *ptr;
    struct { int index; size_t offset; } fixed;
  } u;
} aura_buf_t;
```

Create with inline helpers -- do not construct manually:

| Helper | Description |
|--------|-------------|
| `aura_buf(void *ptr)` | Wrap a regular buffer pointer |
| `aura_buf_fixed(int index, size_t offset)` | Reference a registered buffer at offset. Returns an invalid (NULL-pointer) descriptor if `index < 0`. |
| `aura_buf_fixed_idx(int index)` | Reference a registered buffer at offset 0 |

### Fsync Flags

```c
#define AURA_FSYNC_DEFAULT  0
#define AURA_FSYNC_DATASYNC 1
```

| Flag | Description |
|------|-------------|
| `AURA_FSYNC_DEFAULT` | Full fsync (metadata + data) |
| `AURA_FSYNC_DATASYNC` | fdatasync (data only, skip metadata if possible) |

### Flag Constants

#### Fallocate Modes (for `aura_fallocate`)

| Constant | Value | Description |
|----------|-------|-------------|
| `AURA_FALLOC_DEFAULT` | `0x00` | Default: allocate and extend size |
| `AURA_FALLOC_KEEP_SIZE` | `0x01` | Allocate space without changing file size |
| `AURA_FALLOC_PUNCH_HOLE` | `0x02` | Deallocate space (must combine with `KEEP_SIZE`) |
| `AURA_FALLOC_COLLAPSE` | `0x08` | Remove a range and collapse file |
| `AURA_FALLOC_ZERO` | `0x10` | Zero a range without deallocating |
| `AURA_FALLOC_INSERT` | `0x20` | Insert space, shifting existing data |

#### sync_file_range Flags (for `aura_sync_file_range`)

| Constant | Value | Description |
|----------|-------|-------------|
| `AURA_SYNC_RANGE_WAIT_BEFORE` | `0x01` | Wait for prior writeout to complete |
| `AURA_SYNC_RANGE_WRITE` | `0x02` | Initiate writeout of dirty pages |
| `AURA_SYNC_RANGE_WAIT_AFTER` | `0x04` | Wait for writeout to complete |

#### Statx Lookup Flags (for `aura_statx` flags parameter)

| Constant | Value | Description |
|----------|-------|-------------|
| `AURA_AT_SYMLINK_NOFOLLOW` | `0x100` | Don't follow symlinks |
| `AURA_AT_EMPTY_PATH` | `0x1000` | Operate on fd itself (pathname="") |
| `AURA_STATX_SYMLINK_NOFOLLOW` | `0x100` | Deprecated alias for `AURA_AT_SYMLINK_NOFOLLOW` |
| `AURA_STATX_EMPTY_PATH` | `0x1000` | Deprecated alias for `AURA_AT_EMPTY_PATH` |

#### Statx Field Mask (for `aura_statx` mask parameter)

| Constant | Value | Description |
|----------|-------|-------------|
| `AURA_STATX_MODE` | `0x02` | Request `stx_mode` |
| `AURA_STATX_NLINK` | `0x04` | Request `stx_nlink` |
| `AURA_STATX_UID` | `0x08` | Request `stx_uid` |
| `AURA_STATX_GID` | `0x10` | Request `stx_gid` |
| `AURA_STATX_ATIME` | `0x20` | Request `stx_atime` |
| `AURA_STATX_MTIME` | `0x40` | Request `stx_mtime` |
| `AURA_STATX_CTIME` | `0x80` | Request `stx_ctime` |
| `AURA_STATX_SIZE` | `0x200` | Request `stx_size` |
| `AURA_STATX_BLOCKS` | `0x400` | Request `stx_blocks` |
| `AURA_STATX_ALL` | `0xFFF` | Request all basic fields |

#### Open Flags (for `aura_openat` flags parameter)

| Constant | Value | Description |
|----------|-------|-------------|
| `AURA_O_RDONLY` | `0x0000` | Open for reading only |
| `AURA_O_WRONLY` | `0x0001` | Open for writing only |
| `AURA_O_RDWR` | `0x0002` | Open for reading and writing |
| `AURA_O_CREAT` | `0x0040` | Create file if it doesn't exist |
| `AURA_O_TRUNC` | `0x0200` | Truncate file to zero length |
| `AURA_O_APPEND` | `0x0400` | Append to end of file |
| `AURA_O_DIRECT` | `0x10000` | Direct I/O (bypass page cache) |

#### Log Levels (for `aura_set_log_handler` / `aura_log_emit`)

| Constant | Value | Description |
|----------|-------|-------------|
| `AURA_LOG_ERR` | 3 | Error (matches syslog `LOG_ERR`) |
| `AURA_LOG_WARN` | 4 | Warning (matches syslog `LOG_WARNING`) |
| `AURA_LOG_NOTICE` | 5 | Notice (matches syslog `LOG_NOTICE`) |
| `AURA_LOG_INFO` | 6 | Informational (matches syslog `LOG_INFO`) |
| `AURA_LOG_DEBUG` | 7 | Debug (matches syslog `LOG_DEBUG`) |

### Enumerations

#### `aura_op_type_t` -- Operation Type

Returned by `aura_request_op_type()` for dispatch in generic completion handlers.

```c
typedef enum {
    AURA_OP_READ           = 0,
    AURA_OP_WRITE          = 1,
    AURA_OP_READV          = 2,
    AURA_OP_WRITEV         = 3,
    AURA_OP_FSYNC          = 4,
    AURA_OP_FDATASYNC      = 5,
    AURA_OP_CANCEL         = 6,
    AURA_OP_READ_FIXED     = 7,   // Read using registered buffer
    AURA_OP_WRITE_FIXED    = 8,   // Write using registered buffer
    AURA_OP_OPENAT         = 9,   // Result is new fd (>= 0)
    AURA_OP_CLOSE          = 10,
    AURA_OP_STATX          = 11,
    AURA_OP_FALLOCATE      = 12,
    AURA_OP_FTRUNCATE      = 13,
    AURA_OP_SYNC_FILE_RANGE = 14,
} aura_op_type_t;
```

> **Note:** Values 15 and 16 are reserved for future operations. Switch statements over `aura_op_type_t` should always include a `default` case to handle forward-compatible additions without undefined behavior.

For read/write ops, the callback result is bytes transferred. For `AURA_OP_OPENAT`, the result is the new file descriptor. For other ops, the result is 0 on success.

#### `aura_reg_type_t` -- Registration Resource Type

Used with `aura_unregister()` and `aura_request_unregister()` to specify which registered resource to unregister.

```c
typedef enum {
    AURA_REG_BUFFERS = 0,
    AURA_REG_FILES   = 1
} aura_reg_type_t;
```

### Statistics Types

#### `aura_stats_t` -- Aggregate Engine Stats

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed across all rings |
| `bytes_transferred` | `int64_t` | Total bytes read/written across all rings |
| `current_throughput_bps` | `double` | Current aggregate throughput (bytes/sec) |
| `p99_latency_ms` | `double` | 99th percentile latency (ms) |
| `current_in_flight` | `int` | Current total in-flight operations |
| `optimal_in_flight` | `int` | AIMD-tuned aggregate in-flight limit |
| `peak_in_flight` | `int` | Observed peak in-flight across all rings (high-water mark) |
| `optimal_batch_size` | `int` | AIMD-tuned aggregate batch size |
| `adaptive_spills` | `uint64_t` | ADAPTIVE mode: count of submissions that spilled to another ring |

#### `aura_ring_stats_t` -- Per-Ring Stats

| Field | Type | Description |
|-------|------|-------------|
| `ops_completed` | `int64_t` | Total operations completed on this ring |
| `bytes_transferred` | `int64_t` | Total bytes transferred through this ring |
| `pending_count` | `int` | Current in-flight operations |
| `peak_in_flight` | `int` | Observed peak in-flight for this ring (high-water mark) |
| `in_flight_limit` | `int` | AIMD-tuned maximum in-flight |
| `batch_threshold` | `int` | AIMD-tuned batch size |
| `p99_latency_ms` | `double` | Current P99 latency estimate (ms) |
| `throughput_bps` | `double` | Current throughput (bytes/sec) |
| `aimd_phase` | `int` | Controller phase (0-5, see constants below) |
| `queue_depth` | `int` | Kernel queue depth for this ring |

#### `aura_histogram_t` -- Latency Histogram

An approximate snapshot of the active histogram window. Because the snapshot is read from a concurrently-written histogram, individual bucket values are atomic but `total_count` may differ slightly from the sum of buckets + overflow. When adaptive tuning is disabled (`disable_adaptive = true`), the histogram accumulates data indefinitely instead of being periodically reset.

The histogram uses a tiered bucket layout to provide fine-grained resolution at low latencies while covering a wide range (0–100 ms). Use `aura_histogram_percentile()` for accurate percentile computation or `aura_histogram_bucket_upper_bound_us()` for bucket bounds.

| Field | Type | Description |
|-------|------|-------------|
| `buckets[320]` | `uint32_t[]` | Latency frequency buckets across 4 tiers |
| `overflow` | `uint32_t` | Count of operations exceeding `max_tracked_us` |
| `total_count` | `uint32_t` | Total samples in this snapshot |
| `tier_count` | `int` | Number of tiers (4) |
| `tier_start_us[4]` | `int` | Start latency of each tier in microseconds |
| `tier_width_us[4]` | `int` | Bucket width for each tier in microseconds |
| `tier_base_bucket[4]` | `int` | Starting bucket index for each tier |
| `max_tracked_us` | `int` | Maximum tracked latency in microseconds (100,000 µs) |

#### `aura_buffer_stats_t` -- Buffer Pool Stats

| Field | Type | Description |
|-------|------|-------------|
| `total_allocated_bytes` | `size_t` | Total bytes currently allocated from pool |
| `total_buffers` | `size_t` | Total buffer count currently allocated |
| `shard_count` | `int` | Number of pool shards |

### Constants

#### Version

| Macro | Value | Description |
|-------|-------|-------------|
| `AURA_VERSION_MAJOR` | 0 | Major version |
| `AURA_VERSION_MINOR` | 4 | Minor version |
| `AURA_VERSION_PATCH` | 0 | Patch version |
| `AURA_VERSION` | 400 | Combined: `major * 10000 + minor * 100 + patch` |
| `AURA_VERSION_STRING` | `"0.5.0"` | Version string |

#### AIMD Phase Constants

Used in `aura_ring_stats_t.aimd_phase`:

| Constant | Value | Meaning |
|----------|-------|---------|
| `AURA_PHASE_BASELINE` | 0 | Collecting baseline latency samples |
| `AURA_PHASE_PROBING` | 1 | Additive increase -- testing higher depth |
| `AURA_PHASE_STEADY` | 2 | Throughput plateau -- holding position |
| `AURA_PHASE_BACKOFF` | 3 | Multiplicative decrease -- P99 exceeded target |
| `AURA_PHASE_SETTLING` | 4 | Post-backoff stabilization |
| `AURA_PHASE_CONVERGED` | 5 | Optimal depth found |

#### Histogram Constants

| Macro | Value | Description |
|-------|-------|-------------|
| `AURA_HISTOGRAM_BUCKETS` | 320 | Total number of histogram buckets |
| `AURA_HISTOGRAM_TIER_COUNT` | 4 | Number of tiers in the histogram |
| `AURA_HISTOGRAM_MAX_US` | 100000 | Maximum tracked latency (100 ms) |

**Tiered Layout:** The histogram covers 0–100 ms in 4 tiers with increasing bucket widths:
- Tier 0: 0–1,000 µs, 10 µs width (100 buckets)
- Tier 1: 1,000–5,000 µs, 50 µs width (80 buckets)
- Tier 2: 5,000–20,000 µs, 250 µs width (60 buckets)
- Tier 3: 20,000–100,000 µs, 1,000 µs width (80 buckets)

Operations exceeding 100 ms are counted in `overflow`.

---

### Functions

#### Configuration

##### `aura_options_init`

```c
void aura_options_init(aura_options_t *options);
```

Initialize an options struct with default values. Always call before modifying individual fields.

| Parameter | Type | Description |
|-----------|------|-------------|
| `options` | `aura_options_t *` | Options struct to initialize |

---

#### Lifecycle

##### `aura_create`

```c
aura_engine_t *aura_create(void);
```

Create a new engine with default options. Detects CPU cores, creates one io_uring ring per core, and initializes AIMD controllers.

**Returns:** Engine handle, or `NULL` on failure (errno set).

---

##### `aura_create_with_options`

```c
aura_engine_t *aura_create_with_options(const aura_options_t *options);
```

Create a new engine with custom options.

| Parameter | Type | Description |
|-----------|------|-------------|
| `options` | `const aura_options_t *` | Configuration (must be initialized with `aura_options_init` first) |

**Returns:** Engine handle, or `NULL` on failure (errno set).

---

##### `aura_destroy`

```c
void aura_destroy(aura_engine_t *engine);
```

Destroy the engine. Signals shutdown (new submissions fail with `ESHUTDOWN`), waits for all pending operations to complete, then frees resources. Safe to call from any thread. `NULL` is a no-op.

The caller must ensure all worker threads have stopped submitting I/O and completed their buffer pool operations before calling this function. The recommended shutdown sequence is:

1. Signal worker threads to stop
2. Join/wait for all worker threads
3. Call `aura_destroy()`

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine to destroy (may be `NULL`) |

---

#### Core I/O Operations

All I/O submission functions are **non-blocking** and return a request handle on success, or `NULL` on failure with errno set to one of: `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOENT`, `EOVERFLOW`, `ENOMEM`.

When the ring is at capacity, the engine automatically attempts to flush pending SQEs to the kernel and poll for completions before returning `EAGAIN`. If the ring is still full after this internal recovery attempt, the caller should drain completions (via `aura_poll` or `aura_wait`) and retry.

Buffers must remain valid until the callback fires. The callback may be `NULL` for fire-and-forget operations.

---

##### `aura_read`

```c
aura_request_t *aura_read(aura_engine_t *engine, int fd, aura_buf_t buf,
                           size_t len, off_t offset,
                           aura_callback_t callback, void *user_data);
```

Submit an async read. Supports regular and registered buffers.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `buf` | `aura_buf_t` | Buffer descriptor (use `aura_buf()` or `aura_buf_fixed()`) |
| `len` | `size_t` | Bytes to read |
| `offset` | `off_t` | File offset |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOENT`, `EOVERFLOW`, `EBUSY`, `ENOMEM`

---

##### `aura_write`

```c
aura_request_t *aura_write(aura_engine_t *engine, int fd, aura_buf_t buf,
                            size_t len, off_t offset,
                            aura_callback_t callback, void *user_data);
```

Submit an async write. Same parameters as `aura_read`.

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOENT`, `EOVERFLOW`, `EBUSY`, `ENOMEM`

---

##### `aura_fsync`

```c
aura_request_t *aura_fsync(aura_engine_t *engine, int fd,
                            unsigned int flags,
                            aura_callback_t callback, void *user_data);
```

Submit an async fsync. Pass `AURA_FSYNC_DEFAULT` for a full fsync (metadata + data) or `AURA_FSYNC_DATASYNC` for fdatasync behavior (data only, skip metadata if possible).

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `flags` | `unsigned int` | `AURA_FSYNC_DEFAULT` (0) or `AURA_FSYNC_DATASYNC` (1) |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_readv`

```c
aura_request_t *aura_readv(aura_engine_t *engine, int fd,
                            const struct iovec *iov, int iovcnt,
                            off_t offset, aura_callback_t callback,
                            void *user_data);
```

Submit an async scatter read into multiple buffers. The iovec array and all buffers must remain valid until the callback fires.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | Open file descriptor |
| `iov` | `const struct iovec *` | Array of iovec structures |
| `iovcnt` | `int` | Number of elements in `iov` |
| `offset` | `off_t` | File offset |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_writev`

```c
aura_request_t *aura_writev(aura_engine_t *engine, int fd,
                             const struct iovec *iov, int iovcnt,
                             off_t offset, aura_callback_t callback,
                             void *user_data);
```

Submit an async gather write from multiple buffers. Same parameters as `aura_readv`.

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

#### Metadata Operations

Async wrappers for file lifecycle syscalls. These skip AIMD latency sampling (metadata is not throughput-sensitive) but benefit from io_uring batching and async execution.

Minimum kernel versions: openat/close/statx/fallocate 5.6+, sync_file_range 5.2+, ftruncate 6.9+.

---

##### `aura_openat`

```c
aura_request_t *aura_openat(aura_engine_t *engine, int dirfd,
                             const char *pathname, int flags, mode_t mode,
                             aura_callback_t callback, void *user_data);
```

Submit an async openat. The callback receives the new file descriptor as the result (>= 0 on success, negative errno on failure). The pathname must remain valid until the callback fires.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `dirfd` | `int` | Directory fd (`AT_FDCWD` for current directory) |
| `pathname` | `const char *` | Path to file (relative to `dirfd`) |
| `flags` | `int` | Open flags (`AURA_O_RDONLY`, `AURA_O_WRONLY`, `AURA_O_CREAT`, etc.) |
| `mode` | `mode_t` | File mode (used when `O_CREAT` is set) |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_close`

```c
aura_request_t *aura_close(aura_engine_t *engine, int fd,
                            aura_callback_t callback, void *user_data);
```

Submit an async close. Callback receives 0 on success, negative errno on failure.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | File descriptor to close |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_statx` (Linux only)

```c
aura_request_t *aura_statx(aura_engine_t *engine, int dirfd,
                            const char *pathname, int flags,
                            unsigned int mask, struct statx *statxbuf,
                            aura_callback_t callback, void *user_data);
```

Submit an async statx. Retrieves file metadata into the caller-provided statx buffer. Both `pathname` and `statxbuf` must remain valid until the callback fires.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `dirfd` | `int` | Directory fd (`AT_FDCWD` for current directory) |
| `pathname` | `const char *` | Path (relative to `dirfd`; `""` with `AURA_AT_EMPTY_PATH` for fd-based stat) |
| `flags` | `int` | Lookup flags (`AURA_AT_EMPTY_PATH`, `AURA_AT_SYMLINK_NOFOLLOW`) |
| `mask` | `unsigned int` | Requested fields (`AURA_STATX_SIZE`, `AURA_STATX_MTIME`, `AURA_STATX_ALL`, etc.) |
| `statxbuf` | `struct statx *` | Output buffer -- kernel writes directly here |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_fallocate`

```c
aura_request_t *aura_fallocate(aura_engine_t *engine, int fd, int mode,
                                off_t offset, off_t len,
                                aura_callback_t callback, void *user_data);
```

Submit an async fallocate. Preallocates or deallocates file space.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | File descriptor |
| `mode` | `int` | Allocation mode (`AURA_FALLOC_DEFAULT`, `AURA_FALLOC_KEEP_SIZE`, etc.) |
| `offset` | `off_t` | Starting byte offset |
| `len` | `off_t` | Length of region |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_ftruncate`

```c
aura_request_t *aura_ftruncate(aura_engine_t *engine, int fd,
                                off_t length, aura_callback_t callback,
                                void *user_data);
```

Submit an async ftruncate. Truncates a file to the specified length. Requires kernel 6.9+ and liburing >= 2.7.

**liburing version check:** If the installed liburing lacks `ftruncate` support (< 2.7), `aura_ftruncate` returns `NULL` with `errno = ENOSYS` immediately -- no SQE is submitted and no callback fires. This is distinct from kernel rejection: when the kernel lacks support (kernel < 6.9 with a sufficiently new liburing), the SQE is submitted and the callback receives `result = -ENOSYS`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | File descriptor |
| `length` | `off_t` | New file length |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

##### `aura_sync_file_range`

```c
aura_request_t *aura_sync_file_range(aura_engine_t *engine, int fd,
                                      off_t offset, off_t nbytes,
                                      unsigned int flags,
                                      aura_callback_t callback,
                                      void *user_data);
```

Submit an async sync_file_range. Syncs a byte range without flushing metadata.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fd` | `int` | File descriptor |
| `offset` | `off_t` | Starting byte offset |
| `nbytes` | `off_t` | Number of bytes to sync (0 = to end of file) |
| `flags` | `unsigned int` | `AURA_SYNC_RANGE_WAIT_BEFORE`, `AURA_SYNC_RANGE_WRITE`, `AURA_SYNC_RANGE_WAIT_AFTER` |
| `callback` | `aura_callback_t` | Completion callback (may be `NULL`) |
| `user_data` | `void *` | Passed to callback |

**Errors:** `EINVAL`, `EAGAIN`, `ESHUTDOWN`, `ENOMEM`

---

#### Request Management

##### `aura_cancel`

```c
int aura_cancel(aura_engine_t *engine, aura_request_t *req);
```

Cancel a pending I/O operation (best-effort). If cancelled, the callback receives `result = -ECANCELED`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `req` | `aura_request_t *` | Request to cancel (must belong to this engine) |

**Returns:** 0 if cancellation submitted, -1 on error. A return of 0 does not guarantee the operation will actually be cancelled.

**Errors:** `EINVAL`, `EALREADY`, `EBUSY`, `ESHUTDOWN`, `ENOMEM`

| errno | Meaning |
|-------|---------|
| `EINVAL` | `engine` or `req` is `NULL`, or `req` does not belong to this engine |
| `EALREADY` | Request has already completed |
| `EBUSY` | Submission queue is full; cancel SQE could not be submitted |
| `ESHUTDOWN` | Engine is shutting down |
| `ENOMEM` | Memory allocation failed |

---

##### `aura_request_pending`

```c
bool aura_request_pending(const aura_request_t *req);
```

Check if a request is still in-flight.

**Returns:** `true` if pending, `false` if completed or cancelled.

---

##### `aura_request_fd`

```c
int aura_request_fd(const aura_request_t *req);
```

Get the file descriptor associated with a request.

**Returns:** File descriptor, or -1 if request is invalid.

---

##### `aura_request_user_data`

```c
void *aura_request_user_data(const aura_request_t *req);
```

Get the user data pointer associated with a request.

**Returns:** The `user_data` pointer from the submission call.

---

##### `aura_request_op_type`

```c
int aura_request_op_type(const aura_request_t *req);
```

Get the operation type of a request. Useful in generic completion handlers to distinguish between operations (e.g., openat returns a new fd, read/write return bytes transferred).

**Returns:** Operation type (`AURA_OP_READ`, `AURA_OP_OPENAT`, etc.), or -1 if `req` is `NULL`.

---

#### Event Processing

##### `aura_get_poll_fd`

```c
int aura_get_poll_fd(const aura_engine_t *engine);
```

Get a pollable file descriptor for event loop integration. Becomes readable when completions are available. Uses level-triggered semantics: remains readable as long as unprocessed completions exist. Compatible with epoll (`EPOLLIN`), poll (`POLLIN`), and select.

**Returns:** Pollable fd, or -1 on error (errno set).

---

##### `aura_poll`

```c
int aura_poll(aura_engine_t *engine);
```

Process completed operations without blocking. Invokes callbacks for any finished operations.

**Returns:** Number of completions processed.

---

##### `aura_wait`

```c
int aura_wait(aura_engine_t *engine, int timeout_ms);
```

Block until at least one operation completes or timeout expires.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `timeout_ms` | `int` | Max wait: -1 = forever, 0 = non-blocking |

**Returns:** Number of completions processed, or -1 on error (errno set). Returns -1 with `errno = ETIMEDOUT` when the timeout expires with pending operations remaining.

---

##### `aura_run`

```c
void aura_run(aura_engine_t *engine);
```

Run the event loop until `aura_stop()` is called. Blocks the calling thread. Useful for dedicating a thread to I/O processing. After `aura_stop()` is called, `aura_run()` drains all in-flight I/O with a 10-second timeout before returning.

---

##### `aura_stop`

```c
void aura_stop(aura_engine_t *engine);
```

Signal the event loop to stop. Thread-safe -- can be called from any thread or from within a callback. `aura_run()` returns after processing current completions.

---

##### `aura_drain`

```c
int aura_drain(aura_engine_t *engine, int timeout_ms);
```

Wait until all in-flight operations across all rings have completed. Useful for graceful shutdown or synchronization points.

New submissions are **not** blocked during drain; if other threads submit concurrently, drain processes those as well.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `timeout_ms` | `int` | Max wait: -1 = forever, 0 = non-blocking poll |

**Returns:** Total completions processed, or -1 on error/timeout.

**Errors:** `ETIMEDOUT` if deadline exceeded, `EINVAL` if engine is `NULL`.

---

#### Buffer Management

##### `aura_buffer_alloc`

```c
void *aura_buffer_alloc(aura_engine_t *engine, size_t size);
```

Allocate a page-aligned buffer from the engine's pool. Suitable for `O_DIRECT` I/O. Thread-safe; uses per-thread caching for fast allocation. A buffer allocated by one thread may be freed by another.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `size` | `size_t` | Buffer size in bytes |

**Returns:** Aligned buffer pointer, or `NULL` on failure.

---

##### `aura_buffer_free`

```c
void aura_buffer_free(aura_engine_t *engine, void *buf);
```

Return a buffer to the engine's pool. Thread-safe. The size is stored internally and does not need to be provided.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `buf` | `void *` | Buffer to free (may be `NULL`) |

---

#### Registered Buffers

Registered buffers eliminate kernel mapping overhead for small, frequent I/O. After registration, use `aura_buf_fixed()` with the standard `aura_read`/`aura_write`. Best for buffers reused across many I/O operations (1000+) and high-frequency small I/O (< 16KB).

##### `aura_register_buffers`

```c
int aura_register_buffers(aura_engine_t *engine, const struct iovec *iovs, unsigned int count);
```

Pre-register buffers with the kernel for zero-copy I/O. Call once at startup. After registration, use `aura_buf_fixed()` to reference buffers by index in read/write calls.

Fixed-buffer submissions fail with `EBUSY` while a deferred unregister is draining.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `iovs` | `const struct iovec *` | Array of buffer descriptors |
| `count` | `unsigned int` | Number of buffers |

**Returns:** 0 on success, -1 on error (errno set).

---

#### File Registration

##### `aura_register_files`

```c
int aura_register_files(aura_engine_t *engine, const int *fds, unsigned int count);
```

Pre-register file descriptors with the kernel to eliminate lookup overhead.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `fds` | `const int *` | Array of file descriptors |
| `count` | `unsigned int` | Number of file descriptors |

**Returns:** 0 on success, -1 on error (errno set).

---

##### `aura_update_file`

```c
int aura_update_file(aura_engine_t *engine, int index, int fd);
```

Replace a registered fd at the given index. Pass -1 as `fd` to unregister a slot.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `index` | `int` | Index in registered file array |
| `fd` | `int` | New fd, or -1 to unregister slot |

**Returns:** 0 on success, -1 on error (errno set). On failure, ring state may be inconsistent -- call `aura_unregister(engine, AURA_REG_FILES)` and re-register.

---

#### Unified Registration Lifecycle

##### `aura_unregister`

```c
int aura_unregister(aura_engine_t *engine, aura_reg_type_t type);
```

Unregister previously registered buffers or files (synchronous). For non-callback callers, waits until in-flight operations using registered resources drain and unregister completes. If called from a completion callback, automatically degrades to the deferred (non-blocking) path -- equivalent to `aura_request_unregister()`.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `type` | `aura_reg_type_t` | Resource type (`AURA_REG_BUFFERS` or `AURA_REG_FILES`) |

**Returns:** 0 on success, -1 on error.

---

##### `aura_request_unregister`

```c
int aura_request_unregister(aura_engine_t *engine, aura_reg_type_t type);
```

Request deferred unregister (callback-safe). Marks registered resources as draining. For buffers: new fixed-buffer submissions fail with `EBUSY` while draining. Final unregister completes lazily once in-flight operations reach zero.

When no in-flight operations are present at call time, the kernel unregistration may happen synchronously and the function may block briefly. The non-blocking guarantee applies only when in-flight operations are present and draining is deferred to their completion.

**Safe to call from completion callbacks.**

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `aura_engine_t *` | Engine handle |
| `type` | `aura_reg_type_t` | Resource type (`AURA_REG_BUFFERS` or `AURA_REG_FILES`) |

**Returns:** 0 on success, -1 on error.

---

#### Statistics

All stats functions are thread-safe and safe to call during active I/O.

##### `aura_get_stats`

```c
int aura_get_stats(const aura_engine_t *engine, aura_stats_t *stats, size_t stats_size);
```

Get engine-wide aggregate statistics. The `stats_size` parameter enables forward compatibility: pass `sizeof(aura_stats_t)` so the library writes at most that many bytes.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const aura_engine_t *` | Engine handle |
| `stats` | `aura_stats_t *` | Output struct |
| `stats_size` | `size_t` | Size of the stats struct (use `sizeof(aura_stats_t)`) |

**Returns:** 0 on success, -1 on error (errno set to `EINVAL` if engine, stats, or stats_size is invalid).

---

##### `aura_get_ring_count`

```c
int aura_get_ring_count(const aura_engine_t *engine);
```

**Returns:** Number of io_uring rings, or 0 if engine is `NULL`.

---

##### `aura_get_ring_stats`

```c
int aura_get_ring_stats(const aura_engine_t *engine, int ring_idx,
                         aura_ring_stats_t *stats, size_t stats_size);
```

Get per-ring statistics.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const aura_engine_t *` | Engine handle |
| `ring_idx` | `int` | Ring index (0 to `aura_get_ring_count()-1`) |
| `stats` | `aura_ring_stats_t *` | Output struct |
| `stats_size` | `size_t` | Size of the stats struct (use `sizeof(aura_ring_stats_t)`) |

**Returns:** 0 on success. -1 if engine/stats is `NULL` or `ring_idx` is out of range (stats zeroed on invalid index).

---

##### `aura_get_histogram`

```c
int aura_get_histogram(const aura_engine_t *engine, int ring_idx,
                        aura_histogram_t *hist, size_t hist_size);
```

Get a latency histogram snapshot for a ring. The snapshot is approximate -- see `aura_histogram_t` documentation.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const aura_engine_t *` | Engine handle |
| `ring_idx` | `int` | Ring index (0 to `aura_get_ring_count()-1`) |
| `hist` | `aura_histogram_t *` | Output struct |
| `hist_size` | `size_t` | Size of the histogram struct (use `sizeof(aura_histogram_t)`) |

**Returns:** 0 on success. -1 if engine/hist is `NULL` or `ring_idx` is out of range (hist zeroed on invalid index).

---

##### `aura_get_buffer_stats`

```c
int aura_get_buffer_stats(const aura_engine_t *engine, aura_buffer_stats_t *stats, size_t stats_size);
```

Get buffer pool statistics. Lockless -- reads atomic counters.

| Parameter | Type | Description |
|-----------|------|-------------|
| `engine` | `const aura_engine_t *` | Engine handle |
| `stats` | `aura_buffer_stats_t *` | Output struct |
| `stats_size` | `size_t` | Size of stats struct (use `sizeof(aura_buffer_stats_t)`) |

**Returns:** 0 on success, -1 if engine or stats is `NULL`.

---

##### `aura_phase_name`

```c
const char *aura_phase_name(int phase);
```

Get a human-readable name for an AIMD phase value.

| Parameter | Type | Description |
|-----------|------|-------------|
| `phase` | `int` | Phase value (0-5) from `aura_ring_stats_t.aimd_phase` |

**Returns:** Static string: `"BASELINE"`, `"PROBING"`, `"STEADY"`, `"BACKOFF"`, `"SETTLING"`, `"CONVERGED"`, or `"UNKNOWN"` for out-of-range values.

---

#### Diagnostics

##### `aura_get_fatal_error`

```c
int aura_get_fatal_error(const aura_engine_t *engine);
```

Check if the engine has a fatal error. Once a fatal error is latched (e.g., io_uring ring fd becomes invalid), all subsequent submissions fail with `ESHUTDOWN`. Use this to distinguish a permanently broken engine from transient `EAGAIN`.

**Returns:** 0 if healthy, positive errno value if fatally broken, -1 if engine is `NULL` (errno set to `EINVAL`).

---

##### `aura_in_callback_context`

```c
bool aura_in_callback_context(void);
```

Check if the current thread is inside a completion callback. Useful for libraries building on AuraIO to choose between synchronous and deferred code paths (e.g., `aura_unregister` vs `aura_request_unregister`). Uses thread-local state.

**Returns:** `true` if inside a completion callback, `false` otherwise.

---

##### `aura_histogram_percentile`

```c
double aura_histogram_percentile(const aura_histogram_t *hist, double percentile);
```

Compute a latency percentile from a histogram snapshot. The histogram must have been obtained via `aura_get_histogram()`. **This is the preferred API for percentile computation.**

| Parameter | Type | Description |
|-----------|------|-------------|
| `hist` | `const aura_histogram_t *` | Histogram snapshot |
| `percentile` | `double` | Percentile to compute (0.0 to 100.0, e.g. 99.0 for p99) |

**Returns:** Latency in milliseconds, or -1.0 if histogram is empty or percentile is out of range.

---

##### `aura_histogram_bucket_upper_bound_us`

```c
int aura_histogram_bucket_upper_bound_us(const aura_histogram_t *hist, int bucket);
```

Get the upper bound latency in microseconds for a histogram bucket. Useful for manual bucket iteration or advanced integration scenarios.

| Parameter | Type | Description |
|-----------|------|-------------|
| `hist` | `const aura_histogram_t *` | Histogram snapshot |
| `bucket` | `int` | Bucket index (0 to `AURA_HISTOGRAM_BUCKETS - 1`) |

**Returns:** Upper bound in microseconds, or 0 if bucket index is out of range.

---

#### Logging

##### `aura_log_fn` (callback type)

```c
typedef void (*aura_log_fn)(int level, const char *msg, void *userdata);
```

Log callback type. The callback may be invoked concurrently from multiple threads. Implementations **must** be thread-safe.

| Parameter | Type | Description |
|-----------|------|-------------|
| `level` | `int` | Severity (`AURA_LOG_ERR` .. `AURA_LOG_DEBUG`; matches syslog) |
| `msg` | `const char *` | Formatted message string (NUL-terminated) |
| `userdata` | `void *` | Opaque pointer passed to `aura_set_log_handler()` |

---

##### `aura_set_log_handler`

```c
void aura_set_log_handler(aura_log_fn handler, void *userdata);
```

Set the library-wide log handler. By default no handler is installed and the library is silent. The handler is global (process-wide) and may be called from any thread. Pass `NULL` as `handler` to disable logging.

---

##### `aura_log_emit`

```c
void aura_log_emit(int level, const char *fmt, ...);
```

Emit a log message through the registered handler. No-op when no handler is registered. Thread-safe. Messages are formatted into a 256-byte internal buffer (truncated if longer).

---

#### Version

##### `aura_version`

```c
const char *aura_version(void);
```

**Returns:** Version string (e.g., `"0.5.0"`).

---

##### `aura_version_int`

```c
int aura_version_int(void);
```

**Returns:** Version as integer: `major * 10000 + minor * 100 + patch` (e.g., 400).

---

## C++ API

Header: `#include <aura.hpp>` (includes all sub-headers)

Requires: C++20 (`-std=c++20`)

Namespace: `aura`

### Error Handling

The C++ API throws exceptions on failure. I/O and lifecycle operations throw `aura::Error`. Per-ring stats/histogram accessors throw `std::out_of_range` on invalid ring index. Exceptions thrown from user callbacks result in `std::terminate()`.

### `aura::Error`

Defined in `<aura/error.hpp>`. Inherits from `std::system_error`.

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

### `aura::Options`

Defined in `<aura/options.hpp>`. Builder pattern for engine configuration.

```cpp
aura::Options opts;
opts.queue_depth(512)
    .ring_count(4)
    .enable_sqpoll(true);

aura::Engine engine(opts);
```

**Builder methods** (all return `Options&` for chaining, all `noexcept`):

| Method | Parameter | Description |
|--------|-----------|-------------|
| `queue_depth(int)` | depth | Queue depth per ring (default: 1024) |
| `ring_count(int)` | count | Number of rings (0 = auto) |
| `initial_in_flight(int)` | limit | Starting in-flight limit |
| `min_in_flight(int)` | limit | Minimum in-flight limit |
| `max_p99_latency_ms(double)` | ms | Target P99 latency (0 = auto) |
| `buffer_alignment(size_t)` | align | Buffer alignment (default: page size) |
| `disable_adaptive(bool)` | disable | Disable AIMD tuning (default param: `true`) |
| `enable_sqpoll(bool)` | enable | Enable SQPOLL mode (default param: `true`) |
| `sqpoll_idle_ms(int)` | ms | SQPOLL idle timeout |
| `ring_select(RingSelect)` | mode | Ring selection mode |
| `single_thread(bool)` | enable | Skip ring mutexes (default param: `true`) |

`RingSelect` enum: `aura::RingSelect::Adaptive`, `CpuLocal`, `RoundRobin`.

**Getters** (all `[[nodiscard]] noexcept`): same names as setters, no arguments.

`c_options()` returns `const aura_options_t&` for access to the underlying C struct.

---

### `aura::Engine`

Defined in `<aura/engine.hpp>`. Main engine class. Move-only (non-copyable). Movable when no event loop methods are executing on another thread.

#### Constructors and Destructor

| Signature | Description |
|-----------|-------------|
| `Engine()` | Create with default options. Throws `Error`. |
| `explicit Engine(const Options& opts)` | Create with custom options. Throws `Error`. |
| `Engine(Engine&&) noexcept` | Move constructor |
| `Engine& operator=(Engine&&) noexcept` | Move assignment (destroys current engine first) |
| `~Engine()` | Waits for pending I/O, then destroys |

#### Core I/O Operations (Callback)

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

#### Metadata Operations (Callback)

All template on `Callback` concept. All throw `Error` on submission failure. All return `Request` (marked `[[nodiscard]]`).

| Method | Description |
|--------|-------------|
| `openat(int dirfd, const char* pathname, int flags, mode_t mode, F&& cb)` | Async openat (result is new fd or negative errno) |
| `close(int fd, F&& cb)` | Async close |
| `statx(int dirfd, const char* pathname, int flags, unsigned int mask, struct statx* buf, F&& cb)` | Async statx (Linux only) |
| `fallocate(int fd, int mode, off_t offset, off_t len, F&& cb)` | Async fallocate |
| `ftruncate(int fd, off_t length, F&& cb)` | Async ftruncate (kernel 6.9+) |
| `sync_file_range(int fd, off_t offset, off_t nbytes, unsigned int flags, F&& cb)` | Async sync_file_range |

#### Core I/O Operations (Coroutine)

| Method | Returns | Description |
|--------|---------|-------------|
| `async_read(int fd, BufferRef buf, size_t len, off_t offset)` | `IoAwaitable` | `co_await` yields `ssize_t` |
| `async_write(int fd, BufferRef buf, size_t len, off_t offset)` | `IoAwaitable` | `co_await` yields `ssize_t` |
| `async_fsync(int fd)` | `FsyncAwaitable` | `co_await` completes on fsync |
| `async_fdatasync(int fd)` | `FsyncAwaitable` | `co_await` completes on fdatasync |

#### Metadata Operations (Coroutine)

| Method | Returns | Description |
|--------|---------|-------------|
| `async_openat(int dirfd, const char* pathname, int flags, mode_t mode)` | `OpenatAwaitable` | `co_await` yields `int` fd |
| `async_close(int fd)` | `MetadataAwaitable` | `co_await` returns void |
| `async_statx(int dirfd, const char* pathname, int flags, unsigned int mask, struct statx* buf)` | `StatxAwaitable` | `co_await` returns void (Linux only) |
| `async_fallocate(int fd, int mode, off_t offset, off_t len)` | `MetadataAwaitable` | `co_await` returns void |
| `async_ftruncate(int fd, off_t length)` | `MetadataAwaitable` | `co_await` returns void |
| `async_sync_file_range(int fd, off_t offset, off_t nbytes, unsigned int flags)` | `MetadataAwaitable` | `co_await` returns void |

Coroutine awaitables throw `Error` on negative results when resumed.

#### Cancellation

| Method | Returns | Description |
|--------|---------|-------------|
| `cancel(Request& req) noexcept` | `bool` | `true` if cancellation submitted, `false` on error (e.g., queue full, engine shutting down). Never throws; unlike other `Engine` methods, errors are signalled via the return value. |

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

#### File Registration

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `register_files(std::span<const int>)` | `void` | `Error` | Register fds with kernel |
| `update_file(int index, int fd)` | `void` | `Error` | Replace registered fd |

#### Unified Registration Lifecycle

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `unregister(aura_reg_type_t type)` | `void` | `Error` | Synchronous unregister; degrades to deferred if called from callback |
| `request_unregister(aura_reg_type_t type)` | `void` | `Error` | Deferred unregister (callback-safe) |

#### Statistics

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `get_stats() const` | `Stats` | `Error` | Aggregate engine stats |
| `ring_count() const noexcept` | `int` | -- | Number of rings |
| `get_ring_stats(int ring_idx) const` | `RingStats` | `std::out_of_range` | Per-ring stats |
| `get_histogram(int ring_idx) const` | `Histogram` | `std::out_of_range` | Latency histogram |
| `get_buffer_stats() const noexcept` | `BufferStats` | -- | Buffer pool stats |

#### Diagnostics

| Method | Returns | Throws | Description |
|--------|---------|--------|-------------|
| `get_fatal_error() const` | `int` | `Error` | 0 if healthy, positive errno if fatally broken |
| `static in_callback_context() noexcept` | `bool` | -- | `true` if calling thread is inside a completion callback |

#### Raw Access

| Method | Returns | Description |
|--------|---------|-------------|
| `handle() noexcept` | `aura_engine_t*` | Underlying C handle |
| `handle() const noexcept` | `const aura_engine_t*` | Underlying C handle (const) |
| `operator bool() const noexcept` | `bool` | `true` if valid |

---

### `aura::Request`

Defined in `<aura/request.hpp>`. Non-owning reference to an in-flight request. Valid from submission until callback begins.

| Method | Returns | Description |
|--------|---------|-------------|
| `pending() const noexcept` | `bool` | `true` if still in-flight |
| `fd() const noexcept` | `int` | Associated fd, or -1 |
| `op_type() const noexcept` | `int` | Operation type (`AURA_OP_READ`, etc.), or -1 |
| `handle() noexcept` | `aura_request_t*` | Underlying C handle |
| `handle() const noexcept` | `const aura_request_t*` | Underlying C handle (const) |
| `operator bool() const noexcept` | `bool` | `true` if handle is valid |

---

### `aura::BufferRef`

Defined in `<aura/buffer.hpp>`. Lightweight buffer descriptor (value type, no ownership).

| Constructor / Factory | Description |
|-----------------------|-------------|
| `BufferRef(void* ptr)` | Wrap unregistered buffer pointer |
| `explicit BufferRef(const void* ptr)` | Wrap const unregistered buffer pointer (for write operations only; explicit to prevent accidental use with reads) |
| `static BufferRef fixed(int index, size_t offset = 0)` | Reference a registered buffer |

| Method | Returns | Description |
|--------|---------|-------------|
| `is_registered() const noexcept` | `bool` | `true` if registered buffer |
| `c_buf() const noexcept` | `aura_buf_t` | Underlying C descriptor |

Free functions: `buf(void* ptr)`, `buf_fixed(int index, size_t offset = 0)`.

---

### `aura::Buffer`

Defined in `<aura/buffer.hpp>`. RAII buffer from engine pool. Move-only. Automatically freed on destruction. If a `Buffer` outlives its `Engine`, AuraIO falls back to `free()` instead of touching destroyed engine state.

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

All defined in `<aura/stats.hpp>`. All getters are `[[nodiscard]] noexcept`.

#### `aura::Stats`

| Method | Returns | C Field |
|--------|---------|---------|
| `ops_completed()` | `int64_t` | `ops_completed` |
| `bytes_transferred()` | `int64_t` | `bytes_transferred` |
| `throughput_bps()` | `double` | `current_throughput_bps` |
| `p99_latency_ms()` | `double` | `p99_latency_ms` |
| `current_in_flight()` | `int` | `current_in_flight` |
| `peak_in_flight()` | `int` | `peak_in_flight` |
| `optimal_in_flight()` | `int` | `optimal_in_flight` |
| `optimal_batch_size()` | `int` | `optimal_batch_size` |
| `adaptive_spills()` | `uint64_t` | `adaptive_spills` |
| `c_stats()` | `const aura_stats_t&` | Full C struct |

#### `aura::RingStats`

| Method | Returns | C Field |
|--------|---------|---------|
| `ops_completed()` | `int64_t` | `ops_completed` |
| `bytes_transferred()` | `int64_t` | `bytes_transferred` |
| `pending_count()` | `int` | `pending_count` |
| `peak_in_flight()` | `int` | `peak_in_flight` |
| `in_flight_limit()` | `int` | `in_flight_limit` |
| `batch_threshold()` | `int` | `batch_threshold` |
| `p99_latency_ms()` | `double` | `p99_latency_ms` |
| `throughput_bps()` | `double` | `throughput_bps` |
| `aimd_phase()` | `int` | `aimd_phase` |
| `aimd_phase_name()` | `const char*` | via `aura_phase_name()` |
| `queue_depth()` | `int` | `queue_depth` |
| `ring_index()` | `int` | (set by Engine) |
| `c_stats()` | `const aura_ring_stats_t&` | Full C struct |

#### `aura::Histogram`

**Constants:**

| Constant | Value |
|----------|-------|
| `Histogram::bucket_count` | 320 |

**Methods:**

| Method | Returns | Description |
|--------|---------|-------------|
| `bucket(int idx)` | `uint32_t` | Count at bucket index (0 for out-of-range) |
| `overflow()` | `uint32_t` | Count exceeding max tracked latency |
| `total_count()` | `uint32_t` | Total samples |
| `max_tracked_us()` | `int` | Maximum tracked latency (us, 100,000) |
| `bucket_lower_us(int idx)` | `int` | Lower bound of bucket (us). 0 for out-of-range |
| `bucket_upper_us(int idx)` | `int` | Upper bound of bucket (us). 0 for out-of-range |
| `percentile(double pct)` | `double` | Compute percentile (0.0-100.0). **Preferred API.** Returns latency in ms, or -1.0 if empty/out-of-range |
| `c_histogram()` | `const aura_histogram_t&` | Full C struct |

**Notes:** The histogram uses a tiered bucket layout for fine-grained resolution at low latencies. Prefer `percentile()` for accurate percentile computation. Manual bucket iteration is supported for advanced integration scenarios.

#### `aura::BufferStats`

| Method | Returns | C Field |
|--------|---------|---------|
| `total_allocated_bytes()` | `size_t` | `total_allocated_bytes` |
| `total_buffers()` | `size_t` | `total_buffers` |
| `shard_count()` | `int` | `shard_count` |
| `c_stats()` | `const aura_buffer_stats_t&` | Full C struct |

---

### Coroutine Support

Defined in `<aura/coro.hpp>`. Requires C++20 coroutines.

#### `aura::Task<T>`

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

#### `aura::IoAwaitable`

Returned by `Engine::async_read` and `Engine::async_write`. Not constructed directly.

When `co_await`ed, submits the I/O operation and suspends the coroutine. Resumes with `ssize_t` (bytes transferred). Throws `Error` if the result is negative.

#### `aura::FsyncAwaitable`

Returned by `Engine::async_fsync` and `Engine::async_fdatasync`. Not constructed directly.

When `co_await`ed, submits the fsync and suspends. Resumes with `void`. Throws `Error` if the result is negative.

---

### Free Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `aura::version()` | `const char*` | Library version string (e.g., `"0.5.0"`) |
| `aura::version_int()` | `int` | Version as integer (`major * 10000 + minor * 100 + patch`) |

---

## Rust API

Crate: `aura` (safe bindings) + `aura-sys` (raw FFI)

Requires: Rust edition 2021, `libaura` and `liburing` linked

### Rust Error Handling

The Rust API uses `Result<T, aura::Error>` (aliased as `aura::Result<T>`) instead of exceptions.

#### `aura::Error`

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

`aura::Result<T>` is a type alias for `std::result::Result<T, Error>`.

---

### Options

```rust
use aura::Options;

let opts = Options::new()
    .queue_depth(512)
    .ring_count(4)
    .enable_sqpoll(true);
```

`Options` implements `Clone` and `Default`.

**Builder methods** (all consume and return `Self`):

| Method | Parameter | Description |
|--------|-----------|-------------|
| `queue_depth(i32)` | depth | Queue depth per ring (default: 1024) |
| `ring_count(i32)` | count | Number of rings (0 = auto) |
| `initial_in_flight(i32)` | limit | Starting in-flight limit |
| `min_in_flight(i32)` | limit | Minimum in-flight limit |
| `max_p99_latency_ms(f64)` | latency | Target P99 latency (0 = auto) |
| `buffer_alignment(usize)` | alignment | Buffer alignment (default: 4096) |
| `disable_adaptive(bool)` | disable | Disable AIMD tuning |
| `enable_sqpoll(bool)` | enable | Enable SQPOLL mode |
| `sqpoll_idle_ms(i32)` | timeout | SQPOLL idle timeout |
| `ring_select(RingSelect)` | mode | Ring selection mode |
| `single_thread(bool)` | enable | Skip ring mutexes (caller guarantees single-thread access) |

#### `aura::RingSelect`

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
use aura::Engine;

let engine = Engine::new()?;
// or
let engine = Engine::with_options(&opts)?;
```

`Engine` is `Send + Sync`. Multiple threads can submit I/O concurrently. Drop calls `aura_destroy()` to wait for pending I/O and free resources.

#### Lifecycle

| Method | Returns | Description |
|--------|---------|-------------|
| `Engine::new()` | `Result<Engine>` | Create with default options |
| `Engine::with_options(&Options)` | `Result<Engine>` | Create with custom options |
| `as_ptr()` | `*mut aura_engine_t` | Raw C handle |

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

#### Core I/O Operations

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

#### Metadata Operations

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `openat(RawFd, &CStr, i32, u32, callback)` | `Result<RequestHandle>` | safe | Async openat (result is new fd) |
| `close(RawFd, callback)` | `Result<RequestHandle>` | safe | Async close |
| `statx(RawFd, &CStr, i32, u32, *mut statx, callback)` | `Result<RequestHandle>` | **unsafe** | Async statx (statxbuf must remain valid) |
| `fallocate(RawFd, i32, i64, i64, callback)` | `Result<RequestHandle>` | safe | Async fallocate |
| `ftruncate(RawFd, i64, callback)` | `Result<RequestHandle>` | safe | Async ftruncate (kernel 6.9+) |
| `sync_file_range(RawFd, i64, i64, u32, callback)` | `Result<RequestHandle>` | safe | Async sync_file_range |

#### Cancellation

| Method | Returns | Safety | Description |
|--------|---------|--------|-------------|
| `cancel(&RequestHandle)` | `Result<()>` | **unsafe** | Cancel pending operation (best-effort). Caller must ensure handle is still valid. |

#### Event Processing

| Method | Returns | Description |
|--------|---------|-------------|
| `poll_fd()` | `Result<RawFd>` | Get pollable fd for event loop integration |
| `poll()` | `Result<usize>` | Process completions (non-blocking) |
| `wait(i32)` | `Result<usize>` | Block until completion or timeout (-1 = forever) |
| `run()` | `()` | Run event loop until `stop()` |
| `stop()` | `()` | Signal event loop to stop (thread-safe) |
| `drain(i32)` | `Result<usize>` | Wait until all in-flight ops complete |

#### Statistics

| Method | Returns | Description |
|--------|---------|-------------|
| `stats()` | `Result<Stats>` | Get aggregate engine statistics |
| `ring_count()` | `i32` | Get number of rings |
| `ring_stats(i32)` | `Result<RingStats>` | Get per-ring statistics |
| `histogram(i32)` | `Result<Histogram>` | Get latency histogram for a ring |
| `buffer_stats()` | `Result<BufferStats>` | Get buffer pool statistics |
| `get_fatal_error()` | `Result<i32>` | 0 if healthy, positive errno if fatally broken |

#### Free Functions

| Function | Returns | Description |
|----------|---------|-------------|
| `aura::version()` | `&'static str` | Library version string |
| `aura::version_int()` | `i32` | Library version as integer |
| `aura::in_callback_context()` | `bool` | `true` if inside a completion callback |

---

### Buffer and BufferRef

#### `aura::Buffer`

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

#### `aura::BufferRef`

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
| `op_type()` | `i32` | **unsafe** | Operation type (`AURA_OP_READ`, etc.). Caller must ensure handle is valid. |
| `user_data()` | `*mut c_void` | **unsafe** | User data pointer. Caller must ensure handle is valid. |

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
| `peak_in_flight()` | `i32` | Peak in-flight count (high-water mark) |

#### `aura::RingStats`

Per-ring statistics snapshot. `Clone + Debug`.

| Method | Returns | Description |
|--------|---------|-------------|
| `ops_completed()` | `i64` | Operations completed on this ring |
| `bytes_transferred()` | `i64` | Bytes transferred through this ring |
| `pending_count()` | `i32` | Current in-flight operations |
| `peak_in_flight()` | `i32` | Peak in-flight for this ring |
| `in_flight_limit()` | `i32` | AIMD-tuned in-flight limit |
| `batch_threshold()` | `i32` | AIMD-tuned batch threshold |
| `p99_latency_ms()` | `f64` | P99 latency (ms) |
| `throughput_bps()` | `f64` | Throughput (bytes/sec) |
| `aimd_phase()` | `i32` | AIMD phase (raw value) |
| `aimd_phase_name()` | `&'static str` | AIMD phase name |
| `queue_depth()` | `i32` | Queue depth |

#### `aura::Histogram`

Latency histogram snapshot. `Clone + Debug`. Uses a tiered bucket layout for efficient fine-grained resolution.

| Method | Returns | Description |
|--------|---------|-------------|
| `bucket(usize)` | `u32` | Count at bucket index |
| `overflow()` | `u32` | Count exceeding max tracked latency |
| `total_count()` | `u32` | Total samples |
| `max_tracked_us()` | `i32` | Maximum tracked latency (100,000 µs / 100 ms) |
| `bucket_lower_us(usize)` | `i32` | Lower bound of bucket (us) |
| `bucket_upper_us(usize)` | `i32` | Upper bound of bucket (us) |
| `percentile(f64)` | `Option<f64>` | Compute percentile (0.0-100.0). **Preferred API.** Returns `Some(latency_ms)` or `None` if empty/out-of-range |

**Notes:** Manual bucket iteration is supported for advanced integration scenarios. Prefer `percentile()` for accurate percentile computation.

#### `aura::BufferStats`

Buffer pool statistics snapshot. `Clone + Debug`.

| Method | Returns | Description |
|--------|---------|-------------|
| `total_allocated_bytes()` | `usize` | Total bytes allocated |
| `total_buffers()` | `usize` | Total buffer count |
| `shard_count()` | `i32` | Number of pool shards |

---

### Logging

#### `aura::LogLevel`

```rust
pub enum LogLevel {
    Error   = 3,   // syslog LOG_ERR
    Warning = 4,   // syslog LOG_WARNING
    Notice  = 5,   // syslog LOG_NOTICE
    Info    = 6,   // syslog LOG_INFO
    Debug   = 7,   // syslog LOG_DEBUG
}
```

Implements `Clone`, `Copy`, `Debug`, `Display`, `PartialEq`, `Eq`, `PartialOrd`, `Ord`, `Hash`.

| Method | Returns | Description |
|--------|---------|-------------|
| `name(self)` | `&'static str` | Short name (`"ERR"`, `"WARN"`, etc.) |

#### Logging Functions

| Function | Description |
|----------|-------------|
| `aura::set_log_handler(F)` | Install process-wide log handler (`F: Fn(LogLevel, &str) + Send + 'static`) |
| `aura::clear_log_handler()` | Remove current log handler |
| `aura::log_emit(LogLevel, &str)` | Emit log message through registered handler |

---

### Async Support

Feature-gated behind `async` feature flag. Provides `Future`-based async I/O that integrates with any async runtime (tokio, async-std, smol, etc.).

#### `AsyncEngine` trait

Automatically implemented for `Engine` when the `async` feature is enabled.

```rust
use aura::{Engine, async_io::AsyncEngine};

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
| `ESHUTDOWN` | errno set, returns `NULL`/-1 | throws `Error` (`.is_shutdown()`) | `Error::Submission` | Engine is shutting down or has a fatal error |
| `ENOENT` | errno set, returns `NULL` | throws `Error` (`.is_not_found()`) | `Error::Submission` | No ring available for the operation |
| `EOVERFLOW` | errno set, returns `NULL` | throws `Error` | `Error::Submission` | Queue overflow |
| `ENOMEM` | errno set, returns `NULL`/-1 | throws `Error` | `Error::Submission` / `Error::BufferAlloc` | Memory allocation failed |
| `EBUSY` | errno set, returns `NULL` | throws `Error` (`.is_busy()`) | `Error::Io` | Fixed-buffer submission while deferred unregister is draining |
| `ECANCELED` | callback `result = -ECANCELED` | `Error` (`.is_cancelled()`) | `Error::Cancelled` | Operation was cancelled via `cancel()` |
| `ETIMEDOUT` | errno set, returns -1 | throws `Error` | `Error::Io` | `drain()` or `wait()` timeout exceeded |
| `EALREADY` | errno set, returns -1 | (cancel returns `false`) | `Error::Io` | Cancel called on already-completed request |

---

## Threading Model

AuraIO uses a **multi-submitter, single-poller** threading model:

- **Submissions are thread-safe.** Any number of threads may call `aura_read()`, `aura_write()`, `aura_fsync()`, `aura_openat()`, `aura_close()`, `aura_statx()`, `aura_fallocate()`, `aura_ftruncate()`, `aura_sync_file_range()`, etc. concurrently. Each ring has its own submission lock (`lock`) to protect the submission queue and request pool.

- **Event processing is single-threaded.** `aura_poll()`, `aura_wait()`, `aura_run()`, and `aura_drain()` must not be called concurrently on the same engine. In the C++ and Rust bindings, this is enforced by an internal `Mutex` that serializes concurrent poll/wait/run/drain calls (they block rather than race). In the C API, concurrent event processing is undefined behavior.

- **Statistics are thread-safe.** All stats functions (`aura_get_stats()`, `aura_get_ring_stats()`, etc.) are safe to call from any thread at any time.

- **Callbacks execute in the polling thread.** Callbacks fire during `aura_poll()` or `aura_wait()`. A callback may fire on any thread that polls the engine, not necessarily the thread that submitted the I/O.

- **Buffer pool is thread-safe.** `aura_buffer_alloc()` and `aura_buffer_free()` can be called from any thread. The pool uses per-thread caching for performance.

- **`aura_stop()` is thread-safe.** Can be called from any thread or from within a callback.

- **`aura_destroy()` should be called after joining worker threads.** While it handles in-flight I/O gracefully, the caller must ensure no other thread is actively submitting I/O or using the buffer pool.

- **Registration operations must be serialized.** `aura_register_buffers()`, `aura_register_files()`, `aura_update_file()`, `aura_unregister()`, and `aura_request_unregister()` must not be called concurrently with each other. I/O submissions from other threads are safe during registration.

---

## Callback Rules

Rules that apply to completion callbacks across all API surfaces:

1. **Callbacks may submit new I/O.** It is safe to call `aura_read()`, `aura_write()`, `aura_fsync()`, `aura_openat()`, `aura_close()`, and other submission functions from within a callback.

2. **Callbacks must not call `aura_destroy()`.** This causes undefined behavior.

3. **Callbacks must not call `aura_poll()`/`aura_wait()`/`aura_run()`/`aura_drain()`.** These are already running on the call stack and must not be re-entered. In C++ and Rust, attempting this would deadlock on the internal mutex.

4. **Callbacks should complete quickly.** Long-running callbacks block other completions from being processed.

5. **The request handle is valid only during the callback.** After the callback returns, the handle is recycled and must not be used.

6. **`user_data` and buffer memory must remain valid until callback fires.** The library does not copy buffers or user data.

7. **Deferred unregister is safe from callbacks.** Use `aura_request_unregister(engine, type)` instead of `aura_unregister(engine, type)` when calling from within a callback. The synchronous `aura_unregister()` automatically degrades to the deferred behavior when called from a callback context.

8. **`aura_in_callback_context()` is available.** Libraries building on AuraIO can use this function to choose between synchronous and deferred code paths at runtime.

9. **C++ callbacks must not throw.** Exceptions from callbacks trigger `std::terminate()` because they cannot propagate through the C callback trampoline.

10. **Rust callbacks are `FnOnce`.** Each callback is invoked exactly once. Captured values are dropped after invocation.

---

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for details.
