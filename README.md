# AuraIO

**Aura** = **A**daptive **U**ring **R**untime **A**rchitecture
 
![Linux](https://img.shields.io/badge/platform-Linux%206.0%2B-blue)
![C11](https://img.shields.io/badge/language-C11-orange)
![io_uring](https://img.shields.io/badge/backend-io__uring-green)

> Self-tuning async I/O for Linux. Zero config. Maximum throughput.

AuraIO is a high-performance asynchronous I/O library that automatically optimizes itself using AIMD (Additive Increase Multiplicative Decrease) congestion control. Built on `io_uring`, it delivers maximum throughput without manual tuning.

## Features

- **Zero Configuration** - Automatically detects CPU cores and tunes in-flight limits based on real-time latency and throughput
- **Per-Core Rings** - One io_uring instance per CPU core eliminates cross-core contention
- **CPU-Aware Scheduling** - Submissions routed to the ring matching the calling thread's CPU for cache efficiency
- **AIMD Self-Tuning** - Additive Increase Multiplicative Decrease algorithm finds optimal concurrency
- **Scalable Buffer Pool** - Per-thread caches with auto-scaling shards (4 cores to 500+ cores)
- **Vectored I/O** - Support for scatter/gather reads and writes (readv/writev)
- **Request Cancellation** - Cancel in-flight operations
- **Event Loop Integration** - Pollable file descriptor for epoll/kqueue integration
- **Thread-Safe** - Per-ring locking enables concurrent submissions from multiple threads

## Table of Contents

- [Quick Start](#quick-start)
- [Installation](#installation)
- [Requirements](#requirements)
- [API Reference](#api-reference)
  - [Thread Safety](#thread-safety)
  - [Lifecycle Constraints](#lifecycle-constraints)
- [Configuration Options](#configuration-options)
- [Advanced Features](#advanced-features)
- [How It Works](#how-it-works)
- [Examples](#examples)
- [Performance Tips](#performance-tips)
- [Contributing](#contributing)

## Quick Start

```c
#include <auraio.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>

static int done = 0;

void on_done(auraio_request_t *req, ssize_t result, void *user_data) {
    (void)req; (void)user_data;
    printf("Read %zd bytes\n", result);
    done = 1;
}

int main(void) {
    auraio_engine_t *engine = auraio_create();
    void *buf = auraio_buffer_alloc(engine, 4096);

    int fd = open("/etc/hostname", O_RDONLY);
    auraio_request_t *req = auraio_read(engine, fd, auraio_buf(buf), 4096, 0, on_done, NULL);
    if (!req) {
        perror("auraio_read");
        return 1;
    }

    while (!done) auraio_wait(engine, 100);

    printf("Content: %s", (char *)buf);

    auraio_buffer_free(engine, buf, 4096);
    close(fd);
    auraio_destroy(engine);
    return 0;
}
```

See [`examples/quickstart.c`](examples/quickstart.c) for a complete working example.

## Installation

### Build from Source

```bash
git clone https://github.com/yourusername/auraio.git
cd auraio
make
sudo make install
```

### Build Options

```bash
make              # Build shared and static libraries
make debug        # Build with debug symbols
make test         # Build and run tests
make test-tsan    # Run with ThreadSanitizer
make test-asan    # Run with AddressSanitizer
make examples     # Build example programs
```

### Linking

```bash
# Dynamic linking
gcc myapp.c -lauraio -luring -lpthread -o myapp

# Static linking
gcc myapp.c /path/to/libauraio.a -luring -lpthread -o myapp
```

## Requirements

- **Linux 6.0+** (required for io_uring vectored I/O and async buffered writes)
- **liburing** - `apt install liburing-dev` (Debian/Ubuntu) or equivalent
- **C11 compiler** - GCC or Clang
- **pthreads**

## API Reference

### Core Types

```c
typedef struct auraio_engine auraio_engine_t;    // Opaque engine handle
typedef struct auraio_request auraio_request_t;  // Opaque request handle

// Completion callback
typedef void (*auraio_callback_t)(auraio_request_t *req, ssize_t result, void *user_data);

// Buffer descriptor (for read/write operations)
typedef struct {
    auraio_buf_type_t type;      // AURAIO_BUF_UNREGISTERED or AURAIO_BUF_REGISTERED
    union {
        void *ptr;               // Regular buffer pointer
        struct {
            int index;           // Registered buffer index
            size_t offset;       // Offset within buffer
        } fixed;
    } u;
} auraio_buf_t;

// Helper constructors:
//   auraio_buf(ptr)              - regular buffer
//   auraio_buf_fixed(idx, off)   - registered buffer with offset
//   auraio_buf_fixed_idx(idx)    - registered buffer at offset 0

// Statistics
typedef struct {
    long long ops_completed;
    long long bytes_transferred;
    double current_throughput_bps;
    double p99_latency_ms;
    int current_in_flight;
    int optimal_in_flight;
    int optimal_batch_size;
} auraio_stats_t;

// Configuration options
typedef struct {
    int queue_depth;           // Queue depth per ring (default: 256)
    int ring_count;            // Number of rings, 0 = auto (one per CPU)
    int initial_in_flight;     // Initial in-flight limit (default: queue_depth/4)
    int min_in_flight;         // Minimum in-flight limit (default: 4)
    double max_p99_latency_ms; // Target max P99 latency, 0 = auto
    size_t buffer_alignment;   // Buffer alignment (default: 4096)
    bool disable_adaptive;     // Disable adaptive tuning
} auraio_options_t;

// Fsync flags
typedef enum {
    AURAIO_FSYNC_DEFAULT = 0,   // Full fsync (metadata + data)
    AURAIO_FSYNC_DATASYNC = 1,  // fdatasync (data only)
} auraio_fsync_flags_t;
```

### Functions

| Function | Description |
|----------|-------------|
| **Lifecycle** | |
| `auraio_create()` | Create engine with defaults |
| `auraio_create_with_options(opts)` | Create engine with custom options |
| `auraio_destroy(engine)` | Destroy engine (waits for pending ops) |
| `auraio_options_init(opts)` | Initialize options struct with defaults |
| **I/O Operations** | |
| `auraio_read(engine, fd, auraio_buf(...), len, off, cb, ud)` | Submit async read |
| `auraio_write(engine, fd, auraio_buf(...), len, off, cb, ud)` | Submit async write |
| `auraio_readv(...)` | Submit async vectored read |
| `auraio_writev(...)` | Submit async vectored write |
| `auraio_fsync(...)` | Submit async fsync |
| `auraio_fsync_ex(..., flags)` | Submit async fsync with flags |
| **Cancellation** | |
| `auraio_cancel(engine, req)` | Cancel a pending request |
| `auraio_request_pending(req)` | Check if request is still in-flight |
| `auraio_request_fd(req)` | Get file descriptor from request |
| `auraio_request_user_data(req)` | Get user data from request |
| **Event Processing** | |
| `auraio_poll(engine)` | Process completions (non-blocking) |
| `auraio_wait(engine, timeout_ms)` | Wait for completions |
| `auraio_run(engine)` | Run event loop until `auraio_stop()` |
| `auraio_stop(engine)` | Signal event loop to stop |
| `auraio_get_poll_fd(engine)` | Get fd for epoll/select integration |
| **Buffers** | |
| `auraio_buffer_alloc(engine, size)` | Allocate aligned buffer |
| `auraio_buffer_free(engine, buf, size)` | Free aligned buffer |
| **Statistics** | |
| `auraio_get_stats(engine, stats)` | Get performance statistics |

### Return Values

Submission functions (`auraio_read`, `auraio_write`, etc.) return:
- `auraio_request_t*` on success - can be used for cancellation
- `NULL` on error with `errno` set:
  - `EINVAL` - Invalid parameters (NULL engine, bad fd, etc.)
  - `EAGAIN` - At in-flight limit, try again later
  - `ESHUTDOWN` - Engine is shutting down (destroy in progress)
  - `ENOMEM` - No free request slots

### Thread Safety

- **Submissions**: Multiple threads can safely submit operations concurrently
- **Polling**: `auraio_poll()` and `auraio_wait()` should be called from a single thread
- **Destroy**: Once `auraio_destroy()` is called, all subsequent submissions return `ESHUTDOWN`. In-flight operations complete normally before destroy returns.

### Lifecycle Constraints

**Proper shutdown pattern:**

```c
// 1. Signal worker threads to stop
atomic_store(&should_stop, true);

// 2. Wait for all workers to complete
for (int i = 0; i < num_workers; i++) {
    pthread_join(workers[i], NULL);
}

// 3. NOW it's safe to destroy
auraio_destroy(engine);
```

**Registered buffers:** If using `auraio_register_buffers()`, ensure no I/O operations using those buffers are in-flight before calling `auraio_unregister_buffers()` or `auraio_destroy()`.

**One engine per application:** For optimal buffer pool performance, use a single `auraio_engine_t` per application. Multiple engines will work but only the first engine accessed by each thread gets per-thread buffer caching benefits.

## Configuration Options

Use `auraio_create_with_options()` for custom configuration:

```c
auraio_options_t opts;
auraio_options_init(&opts);

opts.queue_depth = 512;          // Larger queues for high-throughput workloads
opts.ring_count = 4;             // Limit to 4 rings instead of auto-detect
opts.initial_in_flight = 64;     // Start with higher concurrency
opts.max_p99_latency_ms = 5.0;   // Target 5ms P99 latency
opts.disable_adaptive = false;   // Keep adaptive tuning enabled

auraio_engine_t *engine = auraio_create_with_options(&opts);
```

## Advanced Features

### Vectored I/O

Read or write multiple buffers in a single operation:

```c
struct iovec iov[3] = {
    { .iov_base = buf1, .iov_len = 4096 },
    { .iov_base = buf2, .iov_len = 8192 },
    { .iov_base = buf3, .iov_len = 4096 },
};

auraio_request_t *req = auraio_readv(engine, fd, iov, 3, offset, callback, user_data);
```

### Request Cancellation

Cancel in-flight operations:

```c
auraio_request_t *req = auraio_read(engine, fd, auraio_buf(buf), len, offset, callback, data);

// Later, if needed:
if (auraio_request_pending(req)) {
    auraio_cancel(engine, req);
    // Callback will be invoked with result = -ECANCELED
}
```

**Important:** The request handle is only valid until the callback begins execution.
Once the callback starts (whether for success, error, or cancellation), the handle
becomes invalid. Do not use the request handle after the callback has fired.

### Event Loop Integration

Integrate with existing event loops using a pollable file descriptor:

```c
int poll_fd = auraio_get_poll_fd(engine);

// Add to epoll
struct epoll_event ev = { .events = EPOLLIN, .data.fd = poll_fd };
epoll_ctl(epoll_fd, EPOLL_CTL_ADD, poll_fd, &ev);

// In your event loop:
while (1) {
    int n = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
    for (int i = 0; i < n; i++) {
        if (events[i].data.fd == poll_fd) {
            auraio_poll(engine);  // Process completions from ALL rings
        }
    }
}
```

**Note:** The poll fd is a unified `eventfd` registered with all io_uring rings. When ANY ring completes an operation, this fd becomes readable. This ensures proper event loop integration in multi-core setups where operations may complete on any CPU's ring.

### Fdatasync

Use `AURAIO_FSYNC_DATASYNC` for fdatasync behavior (skips metadata sync when possible):

```c
auraio_fsync_ex(engine, fd, AURAIO_FSYNC_DATASYNC, callback, user_data);
```

### Registered Buffers

Registered buffers are pre-mapped into the kernel's address space, eliminating per-I/O mapping overhead. This enables true zero-copy I/O.

```c
// Register buffers once at startup
struct iovec bufs[2] = {
    { .iov_base = buf1, .iov_len = 64 * 1024 },
    { .iov_base = buf2, .iov_len = 64 * 1024 },
};
auraio_register_buffers(engine, bufs, 2);

// Use registered buffers by index (not pointer)
auraio_read(engine, fd, auraio_buf_fixed(0, 0), 64 * 1024, offset, callback, data);
auraio_write(engine, fd, auraio_buf_fixed(1, 0), 64 * 1024, offset, callback, data);

// Cleanup (ensure no in-flight I/O using these buffers)
auraio_unregister_buffers(engine);
```

#### When to Use Registered Buffers

| Use Case | Buffer Type | Why |
|----------|-------------|-----|
| Same buffers reused across many I/O ops | **Registered** | Amortize one-time registration cost over thousands of operations |
| High-frequency small I/O (< 16KB) | **Registered** | Per-I/O mapping overhead is proportionally higher for small transfers |
| Zero-copy is critical | **Registered** | Eliminates kernel page table updates entirely |
| One-off or infrequent operations | **Regular** | Registration overhead exceeds benefit |
| Short-lived buffers | **Regular** | Buffer must remain valid until unregistered |
| Dynamic buffer count | **Regular** | Registered buffer set is fixed at registration time |
| Simple code, maximum portability | **Regular** | No registration/unregistration lifecycle to manage |

**Rule of thumb:** If you're doing 1000+ I/O operations with the same buffer set, registered buffers are worth it. For typical application code with varied buffer usage, regular buffers are simpler and perform well.

## How It Works

### AIMD Self-Tuning

AuraIO uses AIMD congestion control:

1. **Baseline** - Measures P99 latency at low concurrency
2. **Probing** - Gradually increases in-flight ops while throughput improves
3. **Steady** - Maintains optimal config when throughput plateaus
4. **Backoff** - Reduces concurrency if latency spikes (>10x baseline)

### Low-IOPS Adaptive Sampling

AuraIO handles slow storage devices gracefully by extending its sample window when IOPS is low:

| IOPS Rate | Sample Window | Behavior |
|-----------|---------------|----------|
| > 2000/sec | ~10ms | Decision every tick once 20 samples collected |
| 200-2000/sec | 10-100ms | Window extends until 20 samples collected |
| 20-200/sec | 100ms | Uses 100ms minimum window with available samples |
| < 20/sec | Up to 1 second | Extended window, decisions with sparse data |

This prevents noisy statistics and erratic tuning on slow devices (HDDs, network storage, etc.) while maintaining fast 10ms response on high-performance NVMe drives.

### Architecture

```
┌───────────────────────────────────────────────────────────────────┐
│                         auraio_engine_t                           │
│  ┌───────────────┐  ┌───────────────┐       ┌───────────────┐    │
│  │    Ring 0     │  │    Ring 1     │  ...  │    Ring N     │    │
│  │   (Core 0)    │  │   (Core 1)    │       │   (Core N)    │    │
│  │               │  │               │       │               │    │
│  │ ┌───────────┐ │  │ ┌───────────┐ │       │ ┌───────────┐ │    │
│  │ │ Adaptive  │ │  │ │ Adaptive  │ │       │ │ Adaptive  │ │    │
│  │ │Controller │ │  │ │Controller │ │       │ │Controller │ │    │
│  │ └───────────┘ │  │ └───────────┘ │       │ └───────────┘ │    │
│  │ ┌───────────┐ │  │ ┌───────────┐ │       │ ┌───────────┐ │    │
│  │ │ io_uring  │ │  │ │ io_uring  │ │       │ │ io_uring  │ │    │
│  │ └───────────┘ │  │ └───────────┘ │       │ └───────────┘ │    │
│  └───────────────┘  └───────────────┘       └───────────────┘    │
│  ┌────────────────────────────────────────────────────────────┐  │
│  │                    Shared Buffer Pool                      │  │
│  └────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────┘
```

- **One ring per CPU core** eliminates cross-core contention
- **CPU-aware ring selection** routes submissions to the matching core's ring
- **Per-ring adaptive controller** tunes each core independently
- **Per-ring locking** enables safe concurrent access from multiple threads

### Scalable Buffer Pool

The buffer pool auto-scales from laptops to high-core-count servers:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Buffer Pool                                    │
│                                                                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                   │
│  │ Thread Cache │  │ Thread Cache │  │ Thread Cache │  ... (per thread) │
│  │  (no lock)   │  │  (no lock)   │  │  (no lock)   │                   │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘                   │
│         │                 │                 │                           │
│         ▼                 ▼                 ▼                           │
│  ┌────────────┐    ┌────────────┐    ┌────────────┐                     │
│  │  Shard 0   │    │  Shard 1   │    │  Shard N   │  (auto-scaled)      │
│  │  (locked)  │    │  (locked)  │    │  (locked)  │                     │
│  └────────────┘    └────────────┘    └────────────┘                     │
└─────────────────────────────────────────────────────────────────────────┘
```

| System Size | CPU Cores | Shards | Memory Overhead |
|-------------|-----------|--------|-----------------|
| Laptop | 4 | 2 | ~3 KB |
| Desktop | 16 | 4 | ~6 KB |
| Server | 64 | 16 | ~25 KB |
| HPC Node | 500 | 64 | ~100 KB |

- **Fast path**: Thread-local cache hit → no locking, ~10ns
- **Slow path**: Cache miss → single shard lock, ~50ns
- **Contention**: Always ~4 threads per shard regardless of core count

## Examples

See the [`examples/`](examples/) directory:

| Example | Description |
|---------|-------------|
| `quickstart.c` | Minimal working example (start here) |
| `simple_read.c` | Single-file async read with statistics |
| `bulk_reader.c` | High-throughput concurrent directory scanner |
| `write_modes.c` | Demonstrates O_DIRECT vs buffered writes |

Build and run:

```bash
make examples
./examples/simple_read /path/to/file
./examples/bulk_reader /path/to/directory
./examples/write_modes /tmp/testfile
```

## O_DIRECT vs Buffered I/O

AuraIO is **agnostic** to the I/O mode - it works with both O_DIRECT and buffered I/O:

```c
// O_DIRECT: Bypasses page cache, requires aligned buffers
int fd = open("file", O_RDWR | O_DIRECT);
void *buf = auraio_buffer_alloc(engine, size);  // Must use aligned buffer

// Buffered: Uses page cache, any buffer works
int fd = open("file", O_RDWR);
void *buf = malloc(size);  // Regular malloc is fine
```

| Mode | Pros | Cons |
|------|------|------|
| **O_DIRECT** | Predictable latency, no page cache pollution, true async | Requires aligned buffers, no read-ahead |
| **Buffered** | Works with any buffer, benefits from page cache | Latency varies, may block on writeback |

The adaptive tuning works identically for both modes - it measures actual completion latency regardless of how the kernel handles the I/O internally.

## Performance Tips

1. **Use O_DIRECT for consistent latency** - Bypasses page cache variability
2. **Use `auraio_buffer_alloc()` for O_DIRECT** - Returns properly aligned buffers from the scalable pool
3. **Reuse buffers** - Alloc once, use many times; freed buffers go to thread-local cache
4. **Use buffered I/O for small random reads** - Page cache helps with locality
5. **Larger I/Os** - 64KB+ transfers are more efficient than small ones
6. **Let it tune** - Give the engine 1-2 seconds to find optimal settings
7. **Batch submissions** - Submit multiple ops before calling `auraio_poll()`
8. **Use vectored I/O** - Combine multiple buffers into single operations
9. **Integrate with event loops** - Use `auraio_get_poll_fd()` for epoll integration

## Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

For bugs and feature requests, please [open an issue](https://github.com/yourusername/auraio/issues).
