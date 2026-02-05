# AuraIO

**Aura** = **A**daptive **U**ring **R**untime **A**rchitecture

![Linux](https://img.shields.io/badge/platform-Linux%206.0%2B-blue)
![C11](https://img.shields.io/badge/language-C11-orange)
![C++20](https://img.shields.io/badge/language-C%2B%2B20-orange)
![io_uring](https://img.shields.io/badge/backend-io__uring-green)

> Self-tuning async I/O for Linux. Zero config. Maximum throughput.

AuraIO is a drop-in async I/O library that automatically optimizes itself. Replace your blocking `read()`/`write()` calls with AuraIO and get io_uring performance with AIMD self-tuning—no manual configuration required.

## Why AuraIO?

| Challenge | Traditional Approach | AuraIO Approach |
|-----------|---------------------|-----------------|
| **io_uring complexity** | Learn SQE/CQE, manage rings, handle edge cases | One function call: `auraio_read()` |
| **Queue depth tuning** | Guess a number, benchmark, repeat | AIMD finds optimal depth automatically |
| **Multi-core scaling** | Manual ring-per-core setup | Automatic per-CPU rings with CPU-aware routing |
| **Buffer management** | Roll your own allocator | Built-in scalable pool with thread-local caching |

**The pitch:** Get 90% of hand-tuned io_uring performance with 10% of the code.

## Drop-In Adoption

AuraIO requires minimal code changes. You don't need to restructure your application—just replace I/O calls.

**Before (blocking):**
```c
ssize_t n = pread(fd, buf, len, offset);
if (n < 0) handle_error(errno);
process_data(buf, n);
```

**After (async):**
```c
auraio_read(engine, fd, auraio_buf(buf), len, offset, on_complete, ctx);
auraio_wait(engine, -1);  // Or integrate with your event loop

void on_complete(auraio_request_t *req, ssize_t n, void *ctx) {
    if (n < 0) handle_error(-n);
    process_data(/* ... */);
}
```

**Migration effort:** ~20 lines of boilerplate, then mechanical transformation of each I/O call.

## Two APIs: C for Systems, C++ for Applications

### C API — Maximum Control

For databases, storage engines, proxies, and infrastructure:

```c
#include <auraio.h>

auraio_engine_t *engine = auraio_create();
void *buf = auraio_buffer_alloc(engine, 4096);

auraio_read(engine, fd, auraio_buf(buf), 4096, 0, callback, user_data);
auraio_wait(engine, -1);

auraio_buffer_free(engine, buf, 4096);
auraio_destroy(engine);
```

### C++ API — Developer Productivity

For services, applications, and rapid development:

```cpp
#include <auraio.hpp>

auraio::Engine engine;
auto buf = engine.allocate_buffer(4096);

engine.read(fd, buf, 4096, 0, [&](auto& req, ssize_t n) {
    std::cout << "Read " << n << " bytes\n";
});
engine.wait();
// Buffer and engine automatically cleaned up (RAII)
```

**With C++20 coroutines:**

```cpp
auraio::Task<void> copy_file(auraio::Engine& engine, int src, int dst) {
    auto buf = engine.allocate_buffer(65536);
    for (off_t off = 0; ; off += 65536) {
        ssize_t n = co_await engine.async_read(src, buf, 65536, off);
        if (n <= 0) break;
        co_await engine.async_write(dst, buf, n, off);
    }
}
```

### Which API Should I Use?

| Building... | Recommended | Why |
|-------------|-------------|-----|
| Storage engine, database | **C** | Zero overhead, explicit control |
| Proxy, load balancer | **C** | Predictable memory, no exceptions |
| Web service, API server | **C++** | RAII, lambdas, faster iteration |
| Data pipeline, ETL | **C++** | Coroutines simplify async flows |
| Embedded system | **C** | Minimal dependencies |
| Uncertain | **C++** | Can always drop to C via `engine.handle()` |

## Features

- **Zero Configuration** — Automatically detects cores, tunes queue depth via AIMD
- **Per-Core Rings** — One io_uring per CPU eliminates cross-core contention
- **CPU-Aware Routing** — Submissions go to the calling thread's core
- **AIMD Self-Tuning** — Finds optimal concurrency without benchmarking
- **Scalable Buffer Pool** — Thread-local caches, auto-scaling shards
- **Dual API** — C for systems, C++ for applications
- **Coroutine Support** — C++20 `co_await` for clean async code
- **Thread-Safe** — Multiple threads can submit concurrently
- **Event Loop Integration** — Pollable fd for epoll/kqueue

## Quick Start

### C Example

```c
#include <auraio.h>
#include <stdio.h>
#include <fcntl.h>

static int done = 0;

void on_read(auraio_request_t *req, ssize_t result, void *ctx) {
    (void)req; (void)ctx;
    printf("Read %zd bytes\n", result);
    done = 1;
}

int main(void) {
    auraio_engine_t *engine = auraio_create();
    void *buf = auraio_buffer_alloc(engine, 4096);

    int fd = open("/etc/hostname", O_RDONLY);
    auraio_read(engine, fd, auraio_buf(buf), 4096, 0, on_read, NULL);

    while (!done) auraio_wait(engine, 100);

    printf("Content: %s", (char *)buf);

    auraio_buffer_free(engine, buf, 4096);
    close(fd);
    auraio_destroy(engine);
}
```

### C++ Example

```cpp
#include <auraio.hpp>
#include <iostream>
#include <fcntl.h>

int main() {
    auraio::Engine engine;
    auto buf = engine.allocate_buffer(4096);

    int fd = open("/etc/hostname", O_RDONLY);
    bool done = false;

    engine.read(fd, buf, 4096, 0, [&](auto&, ssize_t n) {
        std::cout << "Read " << n << " bytes\n";
        std::cout << "Content: " << static_cast<char*>(buf.data()) << "\n";
        done = true;
    });

    while (!done) engine.wait(100);
    close(fd);
}
```

See [`examples/`](examples/) for more: bulk readers, write modes, coroutines.

## Installation

```bash
git clone https://github.com/yourusername/auraio.git
cd auraio
make
sudo make install
```

**Linking:**
```bash
# C
gcc myapp.c -lauraio -luring -lpthread -o myapp

# C++
g++ -std=c++20 myapp.cpp -lauraio -luring -lpthread -o myapp
```

## Requirements

- **Linux 6.0+** (io_uring features)
- **liburing** — `apt install liburing-dev`
- **C11 compiler** (C API)
- **C++20 compiler** (C++ API, optional)

## API Overview

### Core Operations

| Function | Description |
|----------|-------------|
| `auraio_create()` | Create engine with auto-detected settings |
| `auraio_read()` / `auraio_write()` | Submit async I/O |
| `auraio_wait()` | Wait for completions |
| `auraio_destroy()` | Clean up (waits for pending ops) |

### C++ Equivalents

| C++ | Equivalent to |
|-----|---------------|
| `Engine engine;` | `auraio_create()` |
| `engine.read(...)` | `auraio_read(...)` with lambda callback |
| `co_await engine.async_read(...)` | Coroutine-style async |
| `~Engine()` | `auraio_destroy()` |

Full API documentation: [`docs/`](docs/)

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│                      Your Application                       │
├─────────────────────────────────────────────────────────────┤
│                        AuraIO API                           │
├─────────────────────────────────────────────────────────────┤
│  Ring 0 (Core 0)  │  Ring 1 (Core 1)  │  ...  │  Ring N    │
│  ┌─────────────┐  │  ┌─────────────┐  │       │            │
│  │  Adaptive   │  │  │  Adaptive   │  │       │            │
│  │  Controller │  │  │  Controller │  │       │            │
│  └─────────────┘  │  └─────────────┘  │       │            │
├───────────────────┴───────────────────┴───────┴────────────┤
│                       io_uring / Kernel                     │
└─────────────────────────────────────────────────────────────┘
```

**AIMD Self-Tuning:**
1. Measures baseline latency at low concurrency
2. Increases in-flight ops while throughput improves
3. Backs off if latency spikes (multiplicative decrease)
4. Converges to optimal without manual tuning

**Result:** Adapts automatically to NVMe, HDD, network storage, or noisy neighbors.

## Incremental Adoption

You don't need to rewrite your application:

1. **Identify hot paths** — Profile to find blocking I/O
2. **Add AuraIO** — Create one engine, convert hot paths
3. **Measure** — Compare throughput/latency
4. **Expand** — Migrate more call sites over time

Synchronous and async code coexist—call `auraio_wait()` immediately after submission for sync-style semantics while still getting io_uring efficiency.

## Performance Tips

1. **Use `auraio_buffer_alloc()`** for O_DIRECT — Returns aligned buffers
2. **Reuse buffers** — Freed buffers go to thread-local cache
3. **Let it tune** — Give 1-2 seconds for AIMD to converge
4. **Batch submissions** — Submit multiple ops before waiting
5. **Use vectored I/O** — Combine buffers with `readv`/`writev`

## Documentation

- [Architecture Guide](docs/architecture.md) — Design decisions, adoption guide
- [API Reference](docs/) — Full function documentation
- [Examples](examples/) — Working code samples

## Contributing

1. Fork the repository
2. Create a feature branch
3. Submit a Pull Request

For bugs and features: [open an issue](https://github.com/yourusername/auraio/issues)

## License

[Your license here]
