# AuraIO

**Aura** = **A**daptive **U**ring **R**untime **A**rchitecture

![License](https://img.shields.io/badge/license-Apache--2.0-blue)
![Linux](https://img.shields.io/badge/platform-Linux%205.10%2B-blue)
![C11](https://img.shields.io/badge/language-C11-orange)
![C++20](https://img.shields.io/badge/language-C%2B%2B20-orange)
![Rust](https://img.shields.io/badge/language-Rust-orange)
![io_uring](https://img.shields.io/badge/backend-io__uring-green)

> io_uring that tunes itself. No knobs. No benchmarking sweeps. Just optimal I/O.

AuraIO is a drop-in async I/O library for Linux that **continuously finds the right queue depth** using AIMD congestion control. You don't pick a number and hope — AuraIO tracks the optimum across changing conditions, per ring, per storage target.

## The Problem: Static Depth Is a Treadmill

Every io_uring application has the same question: *what queue depth should I use?*

The standard answer is: benchmark it. But that answer has a shelf life. The optimal depth depends on the storage device, the workload mix, the number of tenants sharing the device, thermal conditions, background compaction — all of which change at runtime. A depth you benchmarked on Tuesday is wrong by Thursday.

**Where static depth fails:**

| Scenario | What happens with a fixed depth |
|----------|--------------------------------|
| **Noisy neighbor** | Another VM starts heavy writes. Your P99 spikes because your depth is too aggressive. By the time you notice, you've been violating SLOs for minutes. |
| **Workload phase change** | Your app switches from sequential bulk loads to random lookups. Optimal depth drops from 128 to 16. Static depth either wastes IOPS or blows latency. |
| **P99 constraint** | You need max throughput under 2ms P99. The right depth is somewhere between 32 and 64 — but you don't know where without sweeping, and it drifts. |
| **Multi-tenant storage** | Shared NVMe behind a hypervisor. Available bandwidth shifts constantly. No single depth is right for more than a few seconds. |

AuraIO replaces this treadmill with AIMD that tracks the optimum continuously:
- Probes up (+1 depth per tick) while throughput improves
- Backs off (×0.80) when latency spikes
- Reconverges within seconds when conditions change
- Works per-ring, per-storage-target — no global tuning

**The result:** You get io_uring performance without io_uring tuning. One function call, zero configuration, continuous optimality.

## Drop-In Adoption

AuraIO requires minimal code changes. You don't need to restructure your application — just replace I/O calls.

**Before (blocking):**
```c
ssize_t n = pread(fd, buf, len, offset);
if (n < 0) handle_error(errno);
process_data(buf, n);
```

**After (async):**
```c
aura_read(engine, fd, aura_buf(buf), len, offset, 0, on_complete, ctx);
while (aura_pending_count(engine) > 0)
    aura_wait(engine, 100);  // Or integrate with your event loop

void on_complete(aura_request_t *req, ssize_t n, void *ctx) {
    if (n < 0) handle_error(-n);
    process_data(/* ... */);
}
```

**Migration effort:** ~20 lines of boilerplate, then mechanical transformation of each I/O call.

## Why AuraIO?

| Challenge | Traditional Approach | AuraIO Approach |
|-----------|---------------------|-----------------|
| **Queue depth tuning** | Benchmark, pick a number, redeploy when it drifts | AIMD finds and tracks optimal depth continuously |
| **io_uring complexity** | Learn SQE/CQE, manage rings, handle edge cases | One function call: `aura_read()`/`aura_write()` |
| **Multi-core scaling** | Manual ring-per-core setup | Automatic per-CPU rings with CPU-aware routing |
| **Changing conditions** | Re-benchmark when storage or load changes | Adapts in seconds — noisy neighbors, phase changes, thermal throttling |
| **Buffer management** | Roll your own allocator | Built-in scalable pool with thread-local caching |
| **Overhead** | Always-on bookkeeping | Passthrough-first: near-zero overhead until pressure is detected |
| **Language support** | C-only or manual bindings | Triple API: C11, C++20 (with coroutines), and Rust (with async/await) |
| **Observability** | DIY metrics | Built-in Prometheus, OpenTelemetry, and syslog exporters |
| **Thread safety** | External locking | Thread-safe submission; event-loop integration via pollable fd |

## Three APIs: C, C++, and Rust

### C API — Maximum Control

For databases, storage engines, proxies, and infrastructure:

```c
#include <aura.h>

aura_engine_t *engine = aura_create();
void *buf = aura_buffer_alloc(engine, 4096);

aura_read(engine, fd, aura_buf(buf), 4096, 0, 0, callback, user_data);
aura_wait(engine, -1);

aura_buffer_free(engine, buf);
aura_destroy(engine);
```

### C++ API — Developer Productivity

For services, applications, and rapid development:

```cpp
#include <aura.hpp>

aura::Engine engine;
auto buf = engine.allocate_buffer(4096);

engine.read(fd, buf, 4096, 0, [&](auto& req, ssize_t n) {
    std::cout << "Read " << n << " bytes\n";
});
engine.wait();
// Buffer and engine automatically cleaned up (RAII)
```

**With C++20 coroutines:**

```cpp
aura::Task<void> copy_file(aura::Engine& engine, int src, int dst) {
    auto buf = engine.allocate_buffer(65536);
    for (off_t off = 0; ; off += 65536) {
        ssize_t n = co_await engine.async_read(src, buf, 65536, off);
        if (n <= 0) break;
        co_await engine.async_write(dst, buf, n, off);
    }
}
```

### Rust API — Safety and Performance

For Rust applications requiring safe, high-performance async I/O:

```rust
use aura::{Engine, Result};
use std::os::unix::io::AsRawFd;

fn main() -> Result<()> {
    let engine = Engine::new()?;
    let buf = engine.allocate_buffer(4096)?;
    let file = std::fs::File::open("/etc/hostname").unwrap();
    let fd = file.as_raw_fd();

    unsafe {
        engine.read(fd, (&buf).into(), 4096, 0, |result| {
            match result {
                Ok(n) => println!("Read {} bytes", n),
                Err(e) => eprintln!("Error: {}", e),
            }
        })?;
    }

    engine.wait(-1)?;
    Ok(())
}
```

**With async/await (feature flag):**

```rust
use aura::{Engine, async_io::AsyncEngine};

async fn copy_file(engine: &Engine, src: i32, dst: i32) -> aura::Result<()> {
    let buf = engine.allocate_buffer(65536)?;
    let mut offset = 0i64;
    loop {
        let n = unsafe { engine.async_read(src, &buf, 65536, offset) }?.await?;
        if n == 0 { break; }
        unsafe { engine.async_write(dst, &buf, n, offset) }?.await?;
        offset += n as i64;
    }
    Ok(())
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
| Rust application | **Rust** | Memory safety, async/await, cargo integration |
| Uncertain | **C++** or **Rust** | Both offer RAII and modern patterns |

## Quick Start

### C Example

```c
#include <aura.h>
#include <stdio.h>
#include <fcntl.h>

static int done = 0;

void on_read(aura_request_t *req, ssize_t result, void *ctx) {
    (void)req; (void)ctx;
    printf("Read %zd bytes\n", result);
    done = 1;
}

int main(void) {
    aura_engine_t *engine = aura_create();
    void *buf = aura_buffer_alloc(engine, 4096);

    int fd = open("/etc/hostname", O_RDONLY);
    aura_read(engine, fd, aura_buf(buf), 4096, 0, 0, on_read, NULL);

    while (aura_pending_count(engine) > 0) aura_wait(engine, 100);

    printf("Content: %s", (char *)buf);

    aura_buffer_free(engine, buf);
    close(fd);
    aura_destroy(engine);
}
```

### C++ Example

```cpp
#include <aura.hpp>
#include <iostream>
#include <fcntl.h>

int main() {
    aura::Engine engine;
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

### Rust Example

```rust
use aura::{Engine, Result};
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

fn main() -> Result<()> {
    let engine = Engine::new()?;
    let buf = engine.allocate_buffer(4096)?;

    let file = File::open("/etc/hostname").unwrap();
    let fd = file.as_raw_fd();
    let done = Arc::new(AtomicBool::new(false));
    let done_cb = done.clone();

    unsafe {
        engine.read(fd, (&buf).into(), 4096, 0, move |result| {
            match result {
                Ok(n) => println!("Read {} bytes", n),
                Err(e) => eprintln!("Error: {}", e),
            }
            done_cb.store(true, Ordering::SeqCst);
        })?;
    }

    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }

    println!("Content: {}", String::from_utf8_lossy(buf.as_slice()));
    Ok(())
}
```

See [`examples/`](examples/) for more: bulk readers, write modes, coroutines, async copy.

## How It Works

```
┌─────────────────────────────────────────────────────────────┐
│                       Your Application                      │
├─────────────────────────────────────────────────────────────┤
│                         AuraIO API                          │
├───────────────────┬───────────────────┬───────┬─────────────┤
│  Ring 0 (Core 0)  │  Ring 1 (Core 1)  │  ...  │   Ring N    │
│  ┌─────────────┐  │  ┌─────────────┐  │       │             │
│  │  Adaptive   │  │  │  Adaptive   │  │       │             │
│  │  Controller │  │  │  Controller │  │       │             │
│  └─────────────┘  │  └─────────────┘  │       │             │
├───────────────────┴───────────────────┴───────┴─────────────┤
│                      io_uring / Kernel                      │
└─────────────────────────────────────────────────────────────┘
```

**Passthrough-First AIMD Self-Tuning:**
1. **Passthrough** (default) — No AIMD gating, near-zero overhead. Sparse latency sampling (1-in-64). Stays here while I/O completes without pressure.
2. **Engage AIMD** — When the tick thread detects growing queue depth or P99 exceeding a user-set target, transitions to the full AIMD state machine:
   - **Baseline** — Measures P99 latency at conservative concurrency
   - **Probe** — Increases in-flight limit by +1 per tick (10ms) while throughput improves
   - **Back off** — Cuts limit by ×0.80 if P99 latency spikes (see `ADAPTIVE_LATENCY_GUARD_MULT` and `ADAPTIVE_AIMD_DECREASE` in `engine/src/adaptive_engine.h`)
   - **Converge** — Settles at the depth that maximizes throughput without blowing latency
3. **Return to Passthrough** — After AIMD converges and pending counts stabilize, overhead drops back to near-zero.

Uses a gentler 20% decrease (vs TCP's 50%) because storage latency
is more predictable than network RTT — fewer oscillations, faster convergence.

**Result:** Page-cache and low-latency workloads run at near-raw io_uring speed. Workloads under pressure get automatic AIMD tuning. The transition is seamless and bidirectional.

## Installation

### C/C++ Library

```bash
git clone https://github.com/Par-B/AuraIO.git
cd AuraIO
make
sudo make install
```

**Linking (manual):**
```bash
# C
gcc myapp.c -laura -luring -lpthread -o myapp

# C++
g++ -std=c++20 myapp.cpp -laura -luring -lpthread -o myapp
```

### CMake

AuraIO provides a `CMakeLists.txt` that builds **only the engine library** (no tools, bindings, or examples). This is the recommended integration path for C/C++ projects.

**As a git submodule** (recommended for projects that vendor dependencies):

```bash
git submodule add https://github.com/Par-B/AuraIO.git extern/auraio
```

```cmake
# Your project's CMakeLists.txt
add_subdirectory(extern/auraio)
target_link_libraries(myapp PRIVATE aura)
# Header: <aura.h> (C) or <aura.hpp> / <aura/*.hpp> (C++)
# Dependencies (liburing, pthreads) are linked transitively.
```

**Via FetchContent** (downloads at configure time):

```cmake
include(FetchContent)
FetchContent_Declare(auraio
    GIT_REPOSITORY https://github.com/Par-B/AuraIO.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(auraio)
target_link_libraries(myapp PRIVATE aura)
```

**Via pkg-config** (after `sudo make install`):
```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(AURA REQUIRED IMPORTED_TARGET libaura)
target_link_libraries(myapp PRIVATE PkgConfig::AURA)
```

> **Note:** The submodule and FetchContent approaches require `liburing` to be installed on the system (`apt install liburing-dev` or equivalent). AuraIO's CMake build finds it via pkg-config.

### Rust Crate

Add to your `Cargo.toml`:

```toml
[dependencies]
# Callback-based API (default):
aura = { git = "https://github.com/Par-B/AuraIO.git", subdirectory = "bindings/rust/aura" }

# Or with async/await support (enables Future-based async_read/async_write):
aura = { git = "https://github.com/Par-B/AuraIO.git", subdirectory = "bindings/rust/aura", features = ["async"] }

# For local development, use a path dependency instead:
# aura = { path = "../AuraIO/bindings/rust/aura" }
```

Or build the Rust bindings from the repository:

```bash
make rust           # Build Rust bindings
make rust-test      # Run Rust tests
make rust-examples  # Build Rust examples
```

## Requirements

- **Linux 5.10+** (io_uring support; 6.0+ recommended for latest features)
- **liburing** — `apt install liburing-dev`
- **C11 compiler** (C API)
- **C++20 compiler** (C++ API, optional)
- **Rust 1.70+** (Rust bindings, optional)

Run `make deps` to install all build and test dependencies, or `make deps-check` to verify what's installed.

## Proving It: Adaptive Value Benchmark

AuraIO includes a benchmark (`tests/adaptive_value`) that demonstrates where adaptive AIMD outperforms static depth tuning on real block devices:

```bash
make bench-adaptive                              # Full run on default temp file
make bench-adaptive ADAPTIVE_BENCH_ARGS="--file /dev/nvme0n1 --duration 30"  # Real device
```

Three scenarios:
1. **Noisy Neighbor** — Background writes create variable I/O pressure. Static depth either over-commits (P99 spikes) or under-commits (wastes IOPS). Adaptive backs off during noise, probes back up during quiet.
2. **P99-Constrained Throughput** — Find max IOPS under a latency ceiling. Static requires sweeping across depths. Adaptive converges to the right depth automatically.
3. **Workload Phase Change** — Sequential reads switch to random reads mid-run. Optimal depth changes dramatically. Adaptive reconverges in seconds.

## API Overview

### Core Operations (C)

| Function | Description |
|----------|-------------|
| `aura_create()` | Create engine with auto-detected settings — ring count, queue depth, and AIMD tuning are all configured automatically. |
| `aura_read()` / `aura_write()` | Submit async read or write |
| `aura_readv()` / `aura_writev()` | Submit vectored (scatter/gather) async I/O |
| `aura_fsync()` | Submit async fsync/fdatasync |
| `aura_cancel()` | Cancel a pending request |
| `aura_pending_count()` | Return number of in-flight operations (useful for wait loops) |
| `aura_poll()` | Process completions without blocking |
| `aura_wait()` | Wait for completions (with timeout) |
| `aura_drain()` | Wait until all pending operations complete |
| `aura_buffer_alloc()` / `aura_buffer_free()` | Allocate/free aligned I/O buffers from the built-in pool |
| `aura_destroy()` | Clean up (waits for pending ops) |

### C++ Equivalents

| C++ | Equivalent to |
|-----|---------------|
| `Engine engine;` | `aura_create()` |
| `engine.read(...)` | `aura_read(...)` with lambda callback |
| `co_await engine.async_read(...)` | Coroutine-style async |
| `~Engine()` | `aura_destroy()` |

### Rust Equivalents

| Rust | Equivalent to |
|------|---------------|
| `Engine::new()?` | `aura_create()` |
| `engine.read(fd, &buf, ...)` | `aura_read(...)` with closure callback |
| `engine.async_read(...).await` | Future-based async (requires `async` feature) |
| `drop(engine)` | `aura_destroy()` |

Full API documentation: [`docs/api-reference.md`](docs/api-reference.md)

## Incremental Adoption

You don't need to rewrite your application:

1. **Identify hot paths** — Profile to find blocking I/O
2. **Add AuraIO** — Create one engine, convert hot paths
3. **Measure** — Compare throughput/latency
4. **Expand** — Migrate more call sites over time

Synchronous and async code coexist — call `aura_wait()` immediately after submission for sync-style semantics while still getting io_uring efficiency.

## Performance Tips

1. **Use `aura_buffer_alloc()`** for O_DIRECT — Returns aligned buffers
2. **Reuse buffers** — Freed buffers go to thread-local cache
3. **Passthrough is instant** — No warmup needed for page-cache workloads; AIMD engages automatically only when pressure is detected
4. **Batch submissions** — Submit multiple ops before waiting
5. **Use vectored I/O** — Combine buffers with `aura_readv`/`aura_writev`

## Tools

Build all tools with `make tools`, or individually:

| Tool | Build | Description | Docs |
|------|-------|-------------|------|
| **BFFIO** | `make BFFIO` | FIO-compatible benchmark with AIMD auto-tuning. Drop-in replacement that accepts the same CLI flags and `.fio` job files. | [docs/bffio.md](docs/bffio.md), [CLI reference](tools/BFFIO/USAGE.md) |
| **sspa** | `make sspa` | Simple Storage Performance Analyzer. Zero-config storage health check — runs 8 real-world I/O patterns and reports bandwidth, IOPS, and latency. | [docs/sspa.md](docs/sspa.md) |
| **auracp** | `make auracp` | High-performance async file copy (C version). | — |
| **auracp-cpp** | `make auracp-cpp` | High-performance async file copy (C++ version). | — |
| **aura-hash** | `make aura-hash` | Parallel file checksum tool (SHA-256, SHA-1, MD5). | [docs/aura-hash.md](docs/aura-hash.md) |
| **atree** | `make atree` | Directory tree with per-file stats (`tree` replacement using batched statx). | [tools/atree/](tools/atree/) |

## Integrations

Built-in metric exporters for observability stacks:

| Integration | Build target | Description |
|-------------|-------------|-------------|
| **Prometheus** | `make integration-prometheus` | Per-ring stats, latency histograms, AIMD phase |
| **OpenTelemetry** | `make integration-otel` | Push-based metrics export |
| **Syslog** | `make integration-syslog` | Structured logging for I/O events |

Build all with `make integrations`. See the [Observability Guide](docs/observability.md) for usage.

## Documentation

- [Architecture Guide](docs/architecture.md) — Design decisions, adoption guide
- [API Quickstart](docs/API-Quickstart.md) — Get started with C, C++, or Rust in minutes
- [API Reference](docs/api-reference.md) — Full function documentation
- [Async Lifecycle](docs/async-lifecycle.md) — Submission vs completion semantics
- [Observability Guide](docs/observability.md) — Stats API, Prometheus integration, sampling costs
- [Performance Guide](docs/performance.md) — Tuning constants, benchmark methodology
- [BFFIO Overview](docs/bffio.md) — FIO-compatible benchmark design and baseline comparisons
- [BFFIO Usage Guide](tools/BFFIO/USAGE.md) — Complete CLI reference, options, examples
- [SSPA Guide](docs/sspa.md) — Storage analyzer workloads and output reference
- [aura-hash Guide](docs/aura-hash.md) — Parallel checksum tool usage
- [Examples](examples/) — Working code samples

## Stability and Versioning

AuraIO is currently pre-1.0 (`0.x`).

1. API and ABI may still evolve between minor releases.
2. Breaking changes are documented in `CHANGELOG.md` with migration guidance.

## Development

```bash
make test             # Build everything + run all tests (C, C++, Rust)
make test-sanitizers  # Run Valgrind, ThreadSanitizer, and AddressSanitizer
make lint             # Static analysis (cppcheck or clang-tidy)
make coverage         # Generate code coverage report
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Submit a Pull Request

For bugs and features: [open an issue](https://github.com/Par-B/AuraIO/issues)

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details.
