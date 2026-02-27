# AuraIO API Quickstart

Get up and running with AuraIO in minutes. This guide covers installation, the core usage pattern, and practical examples in C, C++, and Rust.

---

## Installation

### Dependencies

- Linux kernel 5.6+ (for io_uring)
- liburing (`apt install liburing-dev` or build from source)
- GCC 11+ (C11) or G++ 11+ (C++20 with `-fcoroutines`)
- Rust 1.70+ (for Rust bindings)

### Build from Source

```bash
git clone <repo-url> && cd AuraIO
make           # Build libaura.a + libaura.so
make install   # Install to /usr/local (override with PREFIX=)
```

The library installs:
- `include/aura.h` and `include/aura.hpp` (headers)
- `lib/libaura.a` and `lib/libaura.so` (libraries)

### Linking

```bash
# C
gcc -o myapp myapp.c -laura -luring -lpthread

# C++ (requires C++20)
g++ -std=c++20 -fcoroutines -o myapp myapp.cpp -laura -luring -lpthread

# Static linking (no runtime dependency on libaura.so)
gcc -o myapp myapp.c /path/to/libaura.a -luring -lpthread -lm
```

### Rust

Add to `Cargo.toml`:

```toml
[dependencies]
aura = { path = "bindings/rust/aura" }
```

---

## Core Concepts

AuraIO has four key components:

| Component | Purpose |
|-----------|---------|
| **Engine** | Manages io_uring rings and AIMD controllers. One per application. |
| **Buffers** | Page-aligned memory from the engine's pool. Required for `O_DIRECT`. |
| **Callbacks** | Functions invoked when I/O completes. Fire during `poll()`/`wait()`. |
| **AIMD Controller** | Passthrough-first adaptive tuning. Starts with zero overhead; engages AIMD congestion control only when I/O pressure is detected. |

### Self-Tuning Defaults

AuraIO is designed to work well out of the box with **zero configuration**. The adaptive controller automatically:

- Detects your CPU core count and creates one io_uring ring per core
- Starts in **passthrough mode** — no gating, near-zero overhead
- Engages AIMD when I/O pressure is detected (growing queue depth or P99 target exceeded)
- Probes for optimal concurrency depth, backs off on latency spikes
- Returns to passthrough once the workload stabilizes

**For most applications, `aura_create()` with no options is the right choice.** Override options only when you have a specific constraint (see [When to Override Defaults](#when-to-override-defaults) below).

### The Basic Pattern

Every AuraIO program follows the same flow:

```
1. Create engine
2. Allocate buffers
3. Submit I/O with callbacks
4. Poll/wait for completions
5. Destroy engine
```

---

## C Quickstart

### Minimal Read Example

```c
#include <aura.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static int done = 0;

void on_read(aura_request_t *req, ssize_t result, void *user_data) {
    (void)req;
    if (result > 0)
        printf("Read %zd bytes: %.*s\n", result, (int)result, (char *)user_data);
    else
        fprintf(stderr, "Read failed: %zd\n", result);
    done = 1;
}

int main(void) {
    // 1. Create engine
    aura_engine_t *engine = aura_create();
    if (!engine) { perror("aura_create"); return 1; }

    // 2. Allocate buffer
    void *buf = aura_buffer_alloc(engine, 4096);
    memset(buf, 0, 4096);

    // 3. Open file and submit async read
    int fd = open("/etc/hostname", O_RDONLY);
    aura_read(engine, fd, aura_buf(buf), 4096, 0, on_read, buf);

    // 4. Wait for completion
    while (!done)
        aura_wait(engine, 100);

    // 5. Cleanup
    close(fd);
    aura_buffer_free(engine, buf);
    aura_destroy(engine);
    return 0;
}
```

### When to Override Defaults

`aura_create()` auto-detects everything. You only need `aura_create_with_options()` for specific constraints:

| Option | Default | Override when... |
|--------|---------|------------------|
| `queue_depth` | 1024 | You need even deeper queues for extreme-throughput devices or workloads |
| `ring_count` | 0 (auto: 1 per core) | You want fewer rings (e.g., `1` for a single-threaded tool) |
| `max_p99_latency_ms` | 0 (auto) | You have a hard latency SLA (e.g., `2.0` for a 2ms P99 ceiling). See below. |
| `single_thread` | false | Your application is single-threaded and you want to skip mutex overhead |
| `initial_in_flight` | queue_depth/4 | Only used when adaptive is disabled. In passthrough mode (default), the limit starts at max. |
| `min_in_flight` | 4 | Floor for AIMD in-flight limit when AIMD is engaged. Rarely needs changing |
| `disable_adaptive` | false | Benchmarking at a fixed concurrency depth (disables passthrough and AIMD entirely) |
| `enable_sqpoll` | false | Kernel-side submission polling (requires root/`CAP_SYS_NICE`) |

#### How auto P99 latency detection works

When `max_p99_latency_ms` is 0 (the default), the engine starts in **passthrough mode** and the AIMD controller only engages when I/O pressure is detected (growing queue depth). Once AIMD is active, it derives the backoff threshold automatically:

1. **Baseline measurement** -- During warmup, the controller collects P99 latency samples and tracks the sliding minimum over a 500ms window. This becomes the **baseline P99** -- the best-case latency your device can achieve at the current concurrency.

2. **10x guard multiplier** -- The backoff threshold is set to **10x the baseline P99**. For example, if your NVMe SSD's baseline P99 is 100us, the controller backs off when P99 exceeds 1ms. This allows normal workload variability while catching genuine device saturation.

3. **10ms hard ceiling** -- Before a baseline is established (or if the baseline is still zero), a 10ms hard ceiling is used. No modern SSD should exceed 10ms for a single I/O under normal conditions.

When you set `max_p99_latency_ms` to a specific value (e.g., `2.0`), the controller additionally monitors sparse P99 samples during passthrough mode. If sparse P99 exceeds the target, AIMD engages immediately — even without queue depth pressure. Once active, AIMD increases concurrency as long as P99 stays below your target, and backs off when it exceeds it. This is useful for latency-sensitive services with an SLA.

```c
// Example: latency-sensitive service with a 2ms P99 target
aura_options_t opts;
aura_options_init(&opts);              // Always init first!
opts.max_p99_latency_ms = 2.0;        // AIMD targets this ceiling
opts.initial_in_flight = 4;           // Start conservative, let AIMD probe up

aura_engine_t *engine = aura_create_with_options(&opts);

// Example: single-threaded CLI tool
aura_options_t opts2;
aura_options_init(&opts2);
opts2.ring_count = 1;                  // Only need one ring
opts2.single_thread = true;            // Skip mutex overhead

aura_engine_t *engine2 = aura_create_with_options(&opts2);
```

Most users should start with `aura_create()` and only add options when profiling reveals a need.

### Writing Data

```c
const char *data = "Hello, AuraIO!\n";
void *buf = aura_buffer_alloc(engine, 4096);
memcpy(buf, data, strlen(data));

int fd = open("/tmp/output.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);

aura_write(engine, fd, aura_buf(buf), strlen(data), 0, on_write_done, NULL);

// Don't forget: buf must stay alive until callback fires!
```

### Pipeline Pattern (Multiple In-Flight)

Submit several operations and process completions in a loop:

```c
#define CHUNK 65536
#define SLOTS 8

void *bufs[SLOTS];
for (int i = 0; i < SLOTS; i++) {
    bufs[i] = aura_buffer_alloc(engine, CHUNK);
    aura_read(engine, fd, aura_buf(bufs[i]), CHUNK,
              (off_t)i * CHUNK, on_chunk_done, bufs[i]);
}

// Process completions; callbacks resubmit or mark done
while (active_ops > 0)
    aura_wait(engine, 100);
```

### Monitoring AIMD State

```c
aura_stats_t stats;
aura_get_stats(engine, &stats, sizeof(stats));
printf("Throughput: %.1f MB/s, P99: %.2f ms, In-flight: %d/%d\n",
       stats.current_throughput_bps / (1024*1024),
       stats.p99_latency_ms,
       stats.current_in_flight,
       stats.optimal_in_flight);
```

---

## C++ Quickstart

The C++ API provides RAII, lambda callbacks, and coroutine support. Everything is in namespace `aura`.

### Minimal Read Example

```cpp
#include <aura.hpp>
#include <iostream>
#include <fcntl.h>
#include <unistd.h>

int main() {
    try {
        // 1. Create engine (RAII -- auto-destroyed)
        aura::Engine engine;

        // 2. Allocate buffer (RAII -- auto-freed)
        auto buf = engine.allocate_buffer(4096);

        // 3. Open file and submit async read with lambda
        int fd = open("/etc/hostname", O_RDONLY);
        bool done = false;

        engine.read(fd, buf, 4096, 0, [&](aura::Request&, ssize_t result) {
            if (result > 0)
                std::cout << "Read " << result << " bytes\n";
            done = true;
        });

        // 4. Wait for completion
        while (!done)
            engine.wait(-1);

        close(fd);
        // buf and engine cleaned up automatically

    } catch (const aura::Error& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
```

### Overriding Defaults

The default `aura::Engine engine;` auto-tunes everything. Only use `Options` when you have a specific constraint (see the [options table](#when-to-override-defaults) in the C section above for guidance).

```cpp
// Example: latency-sensitive service
aura::Options opts;
opts.max_p99_latency_ms(2.0)      // AIMD targets 2ms P99 ceiling
    .initial_in_flight(4);        // Start conservative

aura::Engine engine(opts);

// Example: single-threaded CLI tool
aura::Options opts2;
opts2.ring_count(1)
     .single_thread(true);        // Skip mutex overhead

aura::Engine engine2(opts2);
```

### Coroutine-Based Copy

```cpp
#include <aura.hpp>

aura::Task<void> copy_chunk(aura::Engine& engine, int src, int dst,
                             size_t size, off_t offset) {
    auto buf = engine.allocate_buffer(size);

    // co_await suspends until I/O completes -- no callbacks needed
    ssize_t n = co_await engine.async_read(src, buf, size, offset);
    co_await engine.async_write(dst, buf, n, offset);
    co_await engine.async_fdatasync(dst);
}

// Drive the coroutine
auto task = copy_chunk(engine, src_fd, dst_fd, 65536, 0);
task.resume();  // Start the coroutine (submits first I/O)
while (!task.done())
    engine.wait(100);
```

### Metadata Operations

```cpp
// Async file open
engine.openat(AT_FDCWD, "/tmp/test.txt",
              AURA_O_WRONLY | AURA_O_CREAT | AURA_O_TRUNC, 0644,
              [&](aura::Request&, ssize_t result) {
    if (result >= 0) {
        int new_fd = static_cast<int>(result);
        // Use new_fd for writes...
    }
});

// Async fallocate (preallocate space)
engine.fallocate(fd, 0, 0, 1024 * 1024, [](aura::Request&, ssize_t result) {
    if (result == 0) { /* success */ }
});
```

---

## Rust Quickstart

### Minimal Read Example

```rust
use aura::{Engine, Result};
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

fn main() -> Result<()> {
    // 1. Create engine
    let engine = Engine::new()?;

    // 2. Allocate buffer (RAII -- auto-freed on drop)
    let mut buf = engine.allocate_buffer(4096)?;
    buf.as_mut_slice().fill(0);

    // 3. Open file and submit async read
    let file = File::open("/etc/hostname").expect("open");
    let fd = file.as_raw_fd();

    let done = Arc::new(AtomicBool::new(false));
    let done2 = done.clone();

    unsafe {
        engine.read(fd, (&buf).into(), 4096, 0, move |result| {
            match result {
                Ok(n) => println!("Read {} bytes", n),
                Err(e) => eprintln!("Read failed: {}", e),
            }
            done2.store(true, Ordering::SeqCst);
        })?;
    }

    // 4. Wait for completion
    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }

    Ok(())
    // engine and buf dropped automatically
}
```

### Why `unsafe`?

Buffer I/O methods (`read`, `write`, `readv`, `writev`) are `unsafe` because `BufferRef` is `Copy` with no lifetime. The compiler can't prove the buffer outlives the I/O. You must ensure:

1. The buffer stays alive until the callback fires
2. No mutable aliasing of the buffer during the operation

`fsync`, `fdatasync`, and metadata operations are safe because they don't reference user buffers.

### Async/Await (with Tokio)

Enable the `async` feature:

```toml
[dependencies]
aura = { path = "bindings/rust/aura", features = ["async"] }
```

```rust
use aura::{Engine, async_io::AsyncEngine};

// In an async context:
let future = unsafe { engine.async_read(fd, &buf, 4096, 0) }?;
let bytes_read = future.await?;
```

Note: You must poll the engine for completions. Either run `engine.wait()` in a background thread or integrate `engine.poll_fd()` with your async runtime.

---

## Common Patterns

### Event Loop Integration

Use `poll_fd()` to integrate with epoll/poll/select:

```c
int pfd = aura_get_poll_fd(engine);

struct pollfd fds = { .fd = pfd, .events = POLLIN };
while (running) {
    poll(&fds, 1, 100);
    if (fds.revents & POLLIN)
        aura_poll(engine);
}
```

### Dedicated Event Loop Thread (`aura_run` / `engine.run`)

For server-style applications, run the engine on a dedicated thread using `aura_run()` (which blocks until `aura_stop()` is called):

```c
// C: dedicated event loop thread
#include <pthread.h>

static aura_engine_t *g_engine;

void *io_thread(void *arg) {
    (void)arg;
    aura_run(g_engine);   // blocks; fires callbacks on this thread
    return NULL;
}

int main(void) {
    g_engine = aura_create();

    pthread_t tid;
    pthread_create(&tid, NULL, io_thread, NULL);

    // Submit I/O from any thread; callbacks fire on io_thread
    aura_read(g_engine, fd, aura_buf(buf), len, 0, on_done, NULL);

    // Shutdown
    aura_stop(g_engine);   // signals aura_run() to return
    pthread_join(tid, NULL);
    aura_destroy(g_engine);
}
```

```cpp
// C++: equivalent using std::thread
aura::Engine engine;
std::thread io_thread([&]{ engine.run(); });  // blocks until stop()

engine.read(fd, buf, len, 0, callback);       // submit from any thread

engine.stop();         // wake the event loop thread
io_thread.join();
```

### Graceful Shutdown

```c
// 1. Signal workers to stop
atomic_store(&should_stop, true);

// 2. Wait for workers to finish
for (int i = 0; i < num_workers; i++)
    pthread_join(workers[i], NULL);

// 3. Drain remaining I/O
aura_drain(engine, 5000);  // 5 second timeout

// 4. Destroy engine
aura_destroy(engine);
```

### Linked Operations (Op Chaining)

Chain dependent operations so the kernel executes them in order without round-tripping back to user space. For example, write-then-fsync:

```c
// Write data — mark as linked so fsync waits for it
aura_request_t *wreq = aura_write(engine, fd, aura_buf(buf), 4096, 0, write_cb, ud);
aura_request_set_linked(wreq);

// Fsync — chained to the write, NOT marked as linked (end of chain)
aura_request_t *freq = aura_fsync(engine, fd, 0, fsync_cb, ud);

// Both are submitted together; fsync starts only after write succeeds.
// If write fails, fsync callback receives -ECANCELED.
aura_wait(engine, -1);
```

Multi-op chains work too — just mark every op except the last as linked:

```c
aura_request_t *w1 = aura_write(engine, fd, aura_buf(b1), 4096, 0, cb, ud);
aura_request_set_linked(w1);

aura_request_t *w2 = aura_write(engine, fd, aura_buf(b2), 4096, 4096, cb, ud);
aura_request_set_linked(w2);

// Final op: not linked
aura_fsync(engine, fd, 0, cb, ud);

aura_wait(engine, -1);
```

Use `aura_flush(engine)` if you need to force-submit a chain without waiting for completions.

### Registered Buffers (Zero-Copy)

For high-frequency small I/O, register buffers to eliminate kernel mapping overhead:

```c
// Register once at startup
struct iovec iovs[2] = {
    { .iov_base = buf1, .iov_len = 4096 },
    { .iov_base = buf2, .iov_len = 4096 },
};
aura_register_buffers(engine, iovs, 2);

// Use aura_buf_fixed() instead of aura_buf()
aura_read(engine, fd, aura_buf_fixed(0, 0), 4096, offset, cb, ud);

// Unregister when done
aura_unregister(engine, AURA_REG_BUFFERS);
```

### Logging

Install a log handler to see internal library messages:

```c
void my_logger(int level, const char *msg, void *userdata) {
    (void)userdata;
    fprintf(stderr, "[AURA %d] %s\n", level, msg);
}

aura_set_log_handler(my_logger, NULL);
```

### Error Handling Patterns

Submission calls (`aura_read`, `aura_write`, etc.) are **non-blocking**. When the ring is at capacity, Aura tries to flush pending SQEs and poll for completions before giving up. If the ring is still full after that, the call returns `NULL` with `errno = EAGAIN` rather than blocking. This means your application is always in control of when to wait.

The idiomatic pattern is: submit, check for `EAGAIN`, poll completions to free slots, then retry.

```c
// C: Check every return value
aura_request_t *req = aura_read(engine, fd, aura_buf(buf), len, off, cb, ud);
if (!req) {
    if (errno == EAGAIN) {
        // Ring full -- poll completions and retry
        aura_poll(engine);
        req = aura_read(engine, fd, aura_buf(buf), len, off, cb, ud);
    } else {
        // Fatal error
        fprintf(stderr, "aura_read: %s\n", strerror(errno));
    }
}
```

```cpp
// C++: Exceptions with predicates
try {
    engine.read(fd, buf, len, offset, callback);
} catch (const aura::Error& e) {
    if (e.is_again()) {
        engine.poll();  // Make room, retry
    } else if (e.is_shutdown()) {
        // Engine is shutting down
    }
}
```

---

## What's Next

- **[API Reference](api_reference.md)** -- Complete function signatures and type documentation
- **`examples/C/`** -- C examples (simple_read, file_copy, vectored_io, registered_buffers, etc.)
- **`examples/cpp/`** -- C++ examples (coroutine_copy, bulk_reader, custom_config, etc.)
- **`examples/rust/`** -- Rust examples (async_copy, file_copy, registered_buffers, etc.)
- **`tools/BFFIO/`** -- FIO-compatible benchmark tool built on AuraIO
- **`tools/auracp/`** -- Production `cp` replacement with async pipelining

---

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for details.
