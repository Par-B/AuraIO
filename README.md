# AuraIO

**Aura** = **A**daptive **U**ring **R**untime **A**rchitecture

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
aura_read(engine, fd, aura_buf(buf), len, offset, on_complete, ctx);
aura_wait(engine, -1);  // Or integrate with your event loop

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

## Three APIs: C, C++, and Rust

### C API — Maximum Control

For databases, storage engines, proxies, and infrastructure:

```c
#include <aura.h>

aura_engine_t *engine = aura_create();
void *buf = aura_buffer_alloc(engine, 4096);

aura_read(engine, fd, aura_buf(buf), 4096, 0, callback, user_data);
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

## Features

- **AIMD Self-Tuning** — Finds and tracks optimal concurrency continuously, per ring, per target
- **Zero Configuration** — Automatically detects cores, allocates rings, sizes queues
- **Per-Core Rings** — One io_uring per CPU eliminates cross-core contention
- **Smart Ring Selection** — Three modes: ADAPTIVE (CPU-local with overflow spilling), CPU_LOCAL (strict affinity), ROUND_ROBIN (max scaling)
- **Scalable Buffer Pool** — Thread-local caches, auto-scaling shards
- **Triple API** — C for systems, C++ for applications, Rust for safety
- **Coroutine Support** — C++20 `co_await` and Rust async/await
- **Thread-Safe** — Multiple threads can submit concurrently
- **Event Loop Integration** — Pollable fd for epoll
- **Prometheus Metrics** — Built-in exporter with per-ring stats, latency histograms, AIMD phase

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
    aura_read(engine, fd, aura_buf(buf), 4096, 0, on_read, NULL);

    while (!done) aura_wait(engine, 100);

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

**AIMD Self-Tuning:**
1. **Baseline** — Measures P99 latency at low concurrency (depth 4)
2. **Probe** — Increases in-flight limit by +1 per tick (10ms) while throughput improves
3. **Back off** — Cuts limit by ×0.80 if P99 latency spikes (see `ADAPTIVE_LATENCY_GUARD_MULT` and `ADAPTIVE_AIMD_DECREASE` in `engine/src/adaptive_engine.h`)
4. **Converge** — Settles at the depth that maximizes throughput without blowing latency
5. **Re-probe** — Periodically re-tests to track changing conditions

Uses a gentler 20% decrease (vs TCP's 50%) because storage latency
is more predictable than network RTT — fewer oscillations, faster convergence.

**Result:** Adapts automatically to NVMe, HDD, network storage, noisy neighbors, and workload phase changes.

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

## Installation

### C/C++ Library

```bash
git clone https://github.com/your-org/auraio.git
cd auraio
make
sudo make install
```

**Linking:**
```bash
# C
gcc myapp.c -laura -luring -lpthread -o myapp

# C++
g++ -std=c++20 myapp.cpp -laura -luring -lpthread -o myapp
```

### Rust Crate

Add to your `Cargo.toml`:

```toml
[dependencies]
# Callback-based API (default):
aura = { path = "path/to/auraio/bindings/rust/aura" }

# Or with async/await support (enables Future-based async_read/async_write):
aura = { path = "path/to/auraio/bindings/rust/aura", features = ["async"] }
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

## API Overview

### Core Operations (C)

| Function | Description |
|----------|-------------|
| `aura_create()` | Create engine with auto-detected settings |
| `aura_read()` / `aura_write()` | Submit async I/O |
| `aura_wait()` | Wait for completions |
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

Full API documentation: [`docs/`](docs/)

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
3. **Let it tune** — Give 1-2 seconds for AIMD to converge
4. **Batch submissions** — Submit multiple ops before waiting
5. **Use vectored I/O** — Combine buffers with `readv`/`writev`

## BFFIO — FIO-Compatible Benchmark

AuraIO ships with **BFFIO** (Better Faster FIO), a drop-in FIO-compatible benchmark that uses AuraIO as its I/O engine. It accepts the same CLI flags and `.fio` job files as FIO, but replaces the static io_uring engine with AuraIO's AIMD adaptive tuning — automatically finding the optimal queue depth for your hardware.

**Basic usage:**
```bash
make BFFIO
./tools/BFFIO/BFFIO --name=test --rw=randread --bs=4k --size=1G \
    --directory=/tmp/bffio --direct=1 --runtime=10 --time_based
```

**Multi-file workloads:**
```bash
# 4 files, round-robin distribution
./tools/BFFIO/BFFIO --rw=randread --bs=4k --size=1G --nrfiles=4 \
    --directory=/tmp/bffio --direct=1 --runtime=10 --time_based

# Sequential file exhaustion pattern
./tools/BFFIO/BFFIO --rw=read --bs=128k --nrfiles=4 --size=512M \
    --file_service_type=sequential --directory=/tmp/bffio --direct=1 \
    --runtime=10 --time_based
```

See [tools/BFFIO/USAGE.md](tools/BFFIO/USAGE.md) for complete CLI reference and [docs/BFFIO.md](docs/BFFIO.md) for design overview and baseline comparisons.

## Documentation

- [Architecture Guide](docs/architecture.md) — Design decisions, adoption guide
- [API Quickstart](docs/API-Quickstart.md) — Get started with C, C++, or Rust in minutes
- [API Reference](docs/api_reference.md) — Full function documentation
- [Async Lifecycle](docs/ASYNC_LIFECYCLE.md) — Submission vs completion semantics
- [Observability Guide](docs/observability.md) — Stats API, Prometheus integration, sampling costs
- [Performance Guide](docs/performance.md) — Tuning constants, benchmark methodology
- [BFFIO Overview](docs/BFFIO.md) — FIO-compatible benchmark design and baseline comparisons
- [BFFIO Usage Guide](tools/BFFIO/USAGE.md) — Complete CLI reference, options, examples
- [Codebase Map](docs/CODEBASE_MAP.md) — File-level guide for contributors
- [Examples](examples/) — Working code samples

## Stability and Versioning

AuraIO is currently pre-1.0 (`0.x`).

1. API and ABI may still evolve between minor releases.
2. API/ABI surface snapshots are tracked in `api/snapshots/` for visibility.
3. Breaking changes require an RFC in `docs/rfcs/`.

## Contributing

1. Fork the repository
2. Create a feature branch
3. Submit a Pull Request

For bugs and features: [open an issue](https://github.com/your-org/auraio/issues)

## License

[Your license here]
