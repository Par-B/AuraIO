# AuraIO Performance Architecture

## Design Philosophy

AuraIO is built on the principle of **Hardware-Aware I/O**. It does not treat the kernel as a black box; instead, it aligns its internal structures with the physical realities of the hardware (CPU cores, cache lines, and NUMA nodes) to extract maximum IOPS with minimal latency jitter.

## 1. Zero-Copy & Kernel Bypass

### Registered Buffers
Standard `read/write` syscalls involve copying data between kernel space and user space. AuraIO mitigates this overhead through **Registered Buffers** (`aura_register_buffers`).
- **Mechanism**: Buffers are pinned in memory and mapped into the kernel's address space once at startup.
- **Benefit**: I/O operations using these buffers (`AURA_BUF_REGISTERED`) bypass the page mapping overhead entirely, essential for high-frequency small I/O (< 16KB).

### Registered Files
Every unregistered I/O operation forces the kernel to look up the internal `struct file` via an atomic reference count (`fget`/`fput`). AuraIO allows pre-registering file descriptors (`aura_register_files`) to pin these references at startup.
- **Mechanism**: File references are resolved once and stored in the ring's fixed file table. Subsequent I/O operations skip the per-op `fget`/`fput` atomic.
- **Benefit**: Eliminates cross-core cache-line contention on the file's reference count, which becomes a bottleneck at millions of IOPS on many-core systems.

## 2. Lock-Free Memory Management

The memory allocator (`adaptive_buffer.c`) is designed to be effectively lock-free for the "Fast Path".

### Multi-Tiered Allocation
1.  **Thread Cache (L1)**: Each thread maintains a stack of free buffers. Allocation and deallocation here are `O(1)` and require **zero atomic operations** and **zero locks**.
2.  **Sharded Global Pool (L2)**: If the local cache is empty, the thread acquires a lock on a specific *shard*. The pool is sharded by CPU count (up to 256 shards) to ensure that contention remains negligible even on massive servers.

### Alignment
All buffers are automatically aligned to 4096 bytes (or custom alignment) to ensure compatibility with `O_DIRECT`, preventing costly data copying inside the kernel's block layer.

## 3. CPU Locality & Cache Efficiency

### Ring Selection Modes
AuraIO provides three ring selection modes to balance cache locality against throughput scaling:

- **ADAPTIVE** (default, `AURA_SELECT_ADAPTIVE`): Uses the CPU-local ring for maximum cache locality. When the local ring is congested (>75% of in-flight limit), a two-gate spill check runs: if local load is at most 2x the global average, load is broadly distributed and spilling won't help, so it stays local. If local load exceeds 2x the global average, the local ring is an outlier and a **power-of-two random choice** picks the lighter of two random non-local rings. The tick thread computes average ring pending every 10ms. Zero overhead when load is balanced; spills only occur under broad system pressure. Monitor spills via `aura_stats_t.adaptive_spills`.

- **CPU_LOCAL** (`AURA_SELECT_CPU_LOCAL`): Strict CPU affinity via TLS-cached `sched_getcpu()` (refreshed every 32 submissions). Best for NUMA-sensitive workloads where cross-node traffic is expensive. Single-thread throughput is limited to one ring's capacity.

- **ROUND_ROBIN** (`AURA_SELECT_ROUND_ROBIN`): Always distributes via atomic round-robin across all rings. Best for single-thread event loops that need to saturate multiple rings, or for benchmarks measuring peak aggregate throughput.

**When to use each mode:**
- Many threads, each doing moderate I/O → **ADAPTIVE** (default)
- NUMA system, locality-critical → **CPU_LOCAL**
- Few threads, high per-thread IOPS → **ROUND_ROBIN**

**Startup behavior**: During the first ~10ms (before the tick thread computes `avg_ring_pending`), ADAPTIVE mode skips the outlier check and spills purely on the congestion threshold. This is conservative — if a ring fills quickly at startup, it spills to genuinely idle rings.

**NUMA note**: ADAPTIVE mode's power-of-two random choice is not NUMA-aware — it may spill across NUMA nodes. For NUMA-sensitive workloads, use CPU_LOCAL to guarantee node-local ring access.

### Cache Efficiency
Cache misses are expensive. AuraIO ensures that data stays hot in the L1/L2 cache of the CPU core processing it.
- **Submission**: In CPU_LOCAL and ADAPTIVE modes, each thread maintains a TLS-cached CPU ID (via `sched_getcpu()`, refreshed every 32 submissions). I/O operations route to the `io_uring` instance dedicated to that core, avoiding cross-core contention without a syscall on every request.
- **Completion**: With cooperative task delivery (`IORING_SETUP_COOP_TASKRUN`, enabled automatically on kernel 5.19+), completions are processed on the submitting core rather than via interrupts. This maximizes the chance that the user thread reads CQE data from L2 cache.

## 4. Adaptive Congestion Control (The "Brain")

Performance is not just about raw speed but about **consistent latency**. AuraIO embeds an intelligent controller to manage queue depth.

### Passthrough Mode

The default start state is **passthrough** — no AIMD gating on the submission hot path. In passthrough mode:
- `ring_can_submit()` returns `true` immediately (single atomic load of the passthrough flag)
- Latency sampling is reduced from 1-in-8 to 1-in-64, cutting `clock_gettime()` overhead by 8x
- The tick thread (every 10ms) monitors `pending_count` and sparse P99 samples for signs of I/O pressure

This means page-cache workloads and low-latency storage see near-zero overhead from AuraIO's adaptive machinery.

**Engagement criteria** (any one triggers AIMD):
- `pending_count` growing by >4 per tick for 3 consecutive ticks (30ms)
- `pending_count` exceeds half the max queue depth (immediate)
- When `max_p99_latency_ms` is set: sparse P99 exceeds the target

**Return to passthrough**: After AIMD reaches CONVERGED and pending counts remain flat (delta ≤ 2) for 100ms (10 ticks), the controller returns to passthrough with the in-flight limit reset to max.

### Double-Loop Control System (AIMD Active)

When AIMD is engaged, the full two-loop control system operates:

1.  **Inner Loop (Batch Optimizer)**:
    - Monitors the ratio of `SQE Submitted` vs `Syscalls Made`.
    - Dynamically tunes batch sizes to amortize the cost of the `io_uring_submit` syscall without adding artificial latency to individual requests.

2.  **Outer Loop (Throughput/Latency Manager)**:
    - Uses **AIMD** (Additive Increase Multiplicative Decrease) logic.
    - Tuned for I/O patterns consistent with both local NVMe and network latencies. Self-adapts to the hardware it runs on — works equally well with high-performance NVMe and 7200 RPM magnetic platter storage.
    - Self-adjusting variable-width sample windows based on IOPS rate to avoid low-IOPS workloads skewing the sample results.
    - **P99 Guard Logic**: It continuously samples the 99th percentile latency. By default, the guard threshold is auto-derived at 10x the measured baseline P99 (with a 10ms initial threshold before baseline is established). If latency exceeds this threshold, it immediately slashes concurrency. This prevents "Bufferbloat" at the device level, keeping the I/O piping full but not overflowing.

### State Machine and Convergence

The controller starts in PASSTHROUGH. When pressure is detected, it transitions to BASELINE and progresses through: BASELINE → PROBING → SETTLING → STEADY → CONVERGED → PASSTHROUGH. CONVERGED is **not terminal** in either direction — spike detection can trigger BACKOFF → SETTLING → STEADY at any time, and stable conditions trigger a return to PASSTHROUGH. From STEADY, if the ring entered via BACKOFF (tracked via `entered_via_backoff`), the controller automatically re-enters PROBING after `ADAPTIVE_REPROBE_INTERVAL` (100 ticks ≈ 1 second), allowing re-adaptation when workload characteristics change.

**Time-to-convergence**: STEADY state requires `ADAPTIVE_STEADY_THRESHOLD` = 500 ticks (× 10ms tick interval = ~5 seconds) of stable operation before transitioning to CONVERGED. Total ramp time from AIMD engagement is approximately **7–15 seconds** depending on workload variability — faster on stable, consistent workloads; slower on highly variable or latency-sensitive ones. Workloads that never trigger pressure remain in passthrough indefinitely with near-zero overhead.

## 5. Performance Tips for Users

To fully leverage this architecture:
1.  **Pin Your Threads**: Use `pthread_setaffinity_np` in your worker threads. AuraIO routes I/O to per-core rings via `sched_getcpu()`. Pinning ensures stable core affinity, keeping ring data and CQE memory hot in L1/L2 cache.
2.  **Use `O_DIRECT`**: This bypasses the kernel page cache, allowing AuraIO's adaptive controller to see the *true* device latency and tune accurately.
3.  **Pre-register Memory**: For long-running applications, register your buffer pool at startup to shave off microseconds from every operation.
4.  **Pre-allocate Buffers**: Call `aura_buffer_alloc()` once per slot at startup and reuse the same buffers across I/O operations. Calling alloc/free on every I/O adds significant overhead — the buffer pool's thread-local cache is fast, but skipping it entirely is faster. For write workloads, overwrite the buffer content in place before each submission.

    ```c
    // At startup: pre-allocate one buffer per in-flight slot
    void *bufs[DEPTH];
    for (int i = 0; i < DEPTH; i++)
        bufs[i] = aura_buffer_alloc(engine, BUF_SIZE);

    // Hot loop: reuse buffers, no alloc/free
    void *buf = bufs[slot];
    aura_read(engine, fd, aura_buf(buf), BUF_SIZE, offset, 0, cb, data);

    // At shutdown: free once
    for (int i = 0; i < DEPTH; i++)
        aura_buffer_free(engine, bufs[i]);
    ```

5.  **Use `aura_poll()` in Hot Loops**: Prefer the non-blocking `aura_poll()` over `aura_wait(engine, timeout_ms)` when your thread is actively submitting I/O. `aura_wait()` with even a 1ms timeout can stall the pipeline on fast media (tmpfs, Optane, NVMe) where completions arrive in microseconds. Use `aura_wait()` only when you need to block (drain loops, idle event loops).

    ```c
    // Hot loop: submit then poll without blocking
    while (running) {
        submit_batch(engine, ...);
        aura_poll(engine);  // non-blocking, processes whatever is ready
    }

    // Drain loop: blocking is fine here
    while (inflight > 0)
        aura_wait(engine, 100);
    ```

6.  **`initial_in_flight` is ignored in passthrough mode**: The engine starts in passthrough mode where the in-flight limit equals `max_queue_depth` (no gating). The `initial_in_flight` option only takes effect if AIMD engages due to I/O pressure — at that point, the controller starts at `min_in_flight` and probes upward. For most workloads, the defaults work well. If you disable adaptive tuning (`disable_adaptive = true`), the engine uses `initial_in_flight` as a fixed limit.

    ```c
    aura_options_t opts;
    aura_options_init(&opts);
    opts.queue_depth = 512;

    // Default: passthrough mode, full queue depth available immediately
    // AIMD engages only when pressure is detected

    // Latency-targeting mode: AIMD engages when P99 exceeds target
    opts.max_p99_latency_ms = 2.0;

    // Fixed depth (no adaptive tuning):
    opts.disable_adaptive = true;
    opts.initial_in_flight = 64;
    ```

## 6. High-Performance Deployment (64+ Cores, NVMe Arrays, High-Speed Networks)

AuraIO is designed to scale linearly on large-core servers with high-bandwidth storage. Its per-ring isolation model (independent locks, AIMD controllers, and request pools per ring) eliminates cross-core contention on the I/O hot path. This section covers the kernel-side optimizations that become critical when targeting millions of IOPS.

### Registered Files — Eliminate Per-Op Kernel Overhead

**Why it matters**: Every unregistered I/O operation forces the kernel to call `fget()` / `fput()` — an atomic reference count increment/decrement on the file's internal `struct file`. At millions of IOPS across hundreds of cores, this single atomic becomes a severe cross-core cache-line contention point *inside the kernel*, invisible to userspace profiling. File registration eliminates this entirely by pinning the file reference at setup time.

**Impact**: On a 128-core system doing 2M IOPS, unregistered file access can waste 10-20% of kernel CPU time on `fget`/`fput` contention. Registration reduces this to zero.

**How to use (C)**:

```c
// At startup: register all file descriptors
int fds[] = { fd1, fd2, fd3, /* ... */ };
aura_register_files(engine, fds, count);

// Auto-detection: pass raw fds as usual with flags=0 — the engine
// resolves them to registered indices automatically (O(n) lookup per
// submission). No change needed to aura_read/aura_write calls.

// Direct-index: pass the registered index and AURA_FIXED_FILE to
// skip the auto-detection lookup (O(1), best for hot paths).
aura_request_t *req = aura_read(engine, 0 /* index */, aura_buf(buf),
                                 len, offset, AURA_FIXED_FILE, cb, ud);

// To replace a file descriptor (e.g., log rotation):
aura_update_file(engine, index, new_fd);

// At shutdown:
aura_unregister(engine, AURA_REG_FILES);
```

**How to use (C++)**:

```cpp
std::vector<int> fds = { fd1, fd2, fd3 };
engine.register_files(fds);

// I/O calls unchanged
engine.read(fd1, buf, len, offset, callback);

// Replace a registered fd:
engine.update_file(0, new_fd);

// Cleanup:
engine.unregister(AURA_REG_FILES);
```

**Guidelines**:
- Register all file descriptors that will be used for high-frequency I/O at engine startup.
- Registration is applied to all rings and requires a brief lock per ring — do it once, not per-request.
- The `aura_update_file()` function allows replacing individual file descriptors without unregistering the entire set (useful for log rotation or connection recycling).
- There is no AuraIO-imposed limit on registered file count; the limit comes from the kernel (typically 64K+ on modern kernels).
- **Auto-detection vs direct-index**: By default, the engine scans the registered file table on each submission to auto-detect registered fds (O(n) in the number of registered files). For hot paths with many registered files, pass `AURA_FIXED_FILE` in the `flags` parameter with the registered index directly to skip this lookup.

### Registered Buffers — Zero-Copy I/O

**Why it matters**: Unregistered buffers require the kernel to map user pages into kernel address space on every I/O operation (`get_user_pages`). For small, frequent I/Os (4KB metadata reads, index lookups), this page-pinning overhead can exceed the actual I/O time. Registered buffers are pinned once at startup and mapped permanently into the kernel's address space.

**Impact**: Most significant for small I/O sizes (< 16KB) at high frequency. For large sequential I/O (1MB+), the per-op mapping cost is amortized and registration provides less benefit.

**How to use (C)**:

```c
// Allocate and register a pool of buffers at startup
#define NUM_BUFFERS 256
#define BUF_SIZE 4096

void *bufs[NUM_BUFFERS];
struct iovec iovs[NUM_BUFFERS];
for (int i = 0; i < NUM_BUFFERS; i++) {
    bufs[i] = aligned_alloc(4096, BUF_SIZE);
    iovs[i] = (struct iovec){ .iov_base = bufs[i], .iov_len = BUF_SIZE };
}
aura_register_buffers(engine, iovs, NUM_BUFFERS);

// Use registered buffers by index:
aura_read(engine, fd, aura_buf_fixed(0, 0), BUF_SIZE, offset, 0, cb, data);

// For sub-buffer offsets (reading into the middle of a registered buffer):
aura_read(engine, fd, aura_buf_fixed(0, 2048), 2048, offset, 0, cb, data);

// At shutdown:
aura_unregister(engine, AURA_REG_BUFFERS);
for (int i = 0; i < NUM_BUFFERS; i++) free(bufs[i]);
```

**How to use (C++)**:

```cpp
std::vector<void*> bufs(256);
std::vector<iovec> iovs(256);
for (int i = 0; i < 256; i++) {
    bufs[i] = aligned_alloc(4096, 4096);
    iovs[i] = { bufs[i], 4096 };
}
engine.register_buffers(iovs);

// Use registered buffers:
engine.read(fd, aura::BufferRef::fixed(0), 4096, offset, callback);

// Cleanup:
engine.unregister(AURA_REG_BUFFERS);
```

**Guidelines**:
- Register buffers for your hottest I/O paths (metadata reads, index lookups, small random I/O).
- Registered buffers must remain valid and at the same address for the lifetime of the registration.
- `aura_unregister()` blocks until all in-flight fixed-buffer operations complete, then frees the buffers from the kernel. Buffers are safe to free after it returns.
- You can mix registered and unregistered buffers freely — use `aura_buf()` for regular buffers and `aura_buf_fixed()` for registered ones.

### SQPOLL — Kernel-Side Submission Polling

**Why it matters**: In the default mode, every `io_uring_submit()` call requires a `io_uring_enter` syscall — a user-to-kernel context switch. AuraIO batches submissions to amortize this (typically ~16 ops per syscall), but the syscall overhead still consumes CPU cycles. SQPOLL mode creates a dedicated kernel thread that continuously polls the submission queue, eliminating the syscall entirely on the submission path.

**Impact**: Reduces submission latency by ~1-2 microseconds per batch. Most impactful for latency-sensitive workloads (database WAL writes, key-value store gets) where every microsecond matters. Less impactful for bulk throughput where batching already amortizes the syscall cost.

**How to use (C)**:

```c
aura_options_t opts;
aura_options_init(&opts);
opts.enable_sqpoll = true;
opts.sqpoll_idle_ms = 2000;  // Kernel thread sleeps after 2s idle
aura_engine_t *engine = aura_create_with_options(&opts);
```

**How to use (C++)**:

```cpp
aura::Options opts;
opts.enable_sqpoll(true)
    .sqpoll_idle_ms(2000);
aura::Engine engine(opts);
```

**Requirements and trade-offs**:
- Requires `root` or `CAP_SYS_NICE` capability. AuraIO gracefully falls back to normal mode if SQPOLL initialization fails — no code changes needed.
- Each ring spawns a kernel thread that consumes CPU while polling. On a 64-ring system, that's 64 kernel threads. Set `sqpoll_idle_ms` to control how quickly idle threads sleep.
- Best suited for systems with dedicated I/O cores where the CPU cost of polling threads is acceptable.
- SQPOLL is orthogonal to registered files/buffers — combine all three for maximum throughput.

### Deployment Checklist

For production deployments targeting high IOPS on large-core systems:

| Step | API | Impact |
|------|-----|--------|
| 1. Register file descriptors | `aura_register_files()` | Eliminates kernel `fget`/`fput` atomic contention per-op |
| 2. Register I/O buffers | `aura_register_buffers()` | Eliminates kernel page-pinning per-op |
| 3. Enable SQPOLL (if latency-critical) | `opts.enable_sqpoll = true` | Eliminates `io_uring_enter` syscall on submission |
| 4. Use `O_DIRECT` | `open(..., O_DIRECT)` | Bypasses page cache; AIMD sees true device latency |
| 5. Pin worker threads | `pthread_setaffinity_np()` | Keeps I/O data hot in L1/L2 cache |
| 6. Set ring count to match topology | `opts.ring_count = 0` (auto) | One ring per core; no cross-core lock contention |

### Scalability Architecture

AuraIO's internal architecture avoids global contention points:

- **Per-ring locks**: Each io_uring instance has independent submission and completion mutexes. No global lock.
- **Per-ring AIMD**: Each ring has its own adaptive controller. No shared congestion state.
- **Per-thread buffer cache**: Buffer allocation is lock-free on the fast path (TLS cache hit). The sharded global pool (up to 256 shards) handles cache misses with minimal contention.
- **CPU-local ring selection**: Submissions are routed to the ring for the current CPU via a TLS-cached `sched_getcpu()` call (refreshed every 32 submissions). No atomic operations on the ring selection path.
- **Cooperative completions**: `IORING_SETUP_COOP_TASKRUN` (kernel 5.19+) is enabled automatically, delivering completions cooperatively rather than via interrupts. This reduces context switches and latency jitter.

## 7. Internal Optimization Techniques

AuraIO's implementation applies several micro-architectural optimizations to minimize overhead on the I/O hot path. These are transparent to users but explain where the performance comes from.

### Cache-Line-Aware Data Layout

Internal structures are organized so that fields accessed together on the hot path share a 64-byte cache line:

- **Request structs**: File descriptor, opcode, buffer pointer, and callback are packed into the first cache line. Completion metadata occupies a second line, avoiding false sharing between submission and completion.
- **Thread-local buffer caches**: The pool pointer, generation ID, and per-class counts live in cache line 0 for fast-path validation. The buffer pointer arrays (cold) are in subsequent lines.
- **Adaptive controller**: AIMD state (inflight limit, batch threshold, phase) is separated from histogram data. The controller's hot loop touches only the first cache line.

### Lock-Free Fast Paths

The most frequent operations avoid locks and atomics entirely:

- **Thread-local buffer cache**: Allocation and deallocation from the per-thread cache are `O(1)` with zero atomics — just a pointer swap on a stack.
- **Registered file/buffer elision**: An `_Atomic bool` flag tracks whether any files or buffers are registered. The common unregistered path reads this flag with `memory_order_acquire` and skips the `reg_lock` rwlock entirely — eliminating ~93 instructions per operation (rdlock + unlock + resolution function) when no registrations are active.
- **TLS-cached CPU routing**: The result of `sched_getcpu()` is cached in thread-local storage and refreshed every 32 submissions, avoiding a syscall on every I/O operation.
- **Size-class lookup**: Common buffer sizes (up to 256KB) use a comparison cascade rather than a `CLZ` instruction, which is faster on most x86 microarchitectures for the typical 4KB-64KB range.

### Lock Batching

Where locks are required, AuraIO amortizes the cost by doing more work per acquisition:

- **Completion queue drain**: The `cq_lock` is held for up to 32 CQE extractions per acquisition, rather than lock-per-CQE.
- **Wait path**: A single lock acquisition covers the flush, pending-request check, and batch extraction — three operations that previously required separate lock cycles.

### Per-Operation Overhead Reduction

Micro-optimizations that compound at high IOPS:

- **Request initialization**: Only the 8 fields needed for a new request are written (targeted stores), rather than zeroing the full struct with `memset`.
- **Buffer allocation fast path**: The TLS cache validity check is a single pointer comparison (`cache->pool == pool`) followed by an atomic `pool->destroyed` check. The generation ID (`pool_id`) is only validated on the slow path as defense-in-depth, keeping the hot path to two comparisons.
- **Inline hot getters**: The adaptive controller's `inflight_limit` and `batch_threshold` are computed via `static inline` functions in the header, eliminating function call overhead on every submission.

### Kernel Cooperation

AuraIO configures `io_uring` to minimize kernel-userspace friction:

- **`IORING_SETUP_COOP_TASKRUN`** (kernel 5.19+): Completions are delivered cooperatively when the application polls, rather than via asynchronous interrupts. This reduces context switches and keeps completion data on the submitting core's cache.
- **Merged flush+poll**: The wait path performs submission and completion reaping in a single pass, with an early exit as soon as the first completions arrive — avoiding unnecessary re-entry into the kernel.

## 8. Performance Regression Test

### Purpose

`tests/perf_regression.c` measures AuraIO's per-operation overhead against a minimal raw io_uring baseline. It exists to answer: **"How much does AuraIO's adaptive machinery cost in real IOPS?"** and to catch regressions when the engine internals change.

### Design Decisions

**Why raw io_uring as the baseline (not fio)?**
Fio has its own overhead (job parsing, stats, threading). A hand-written io_uring loop with no locks, no atomics, no callbacks, and no histograms represents the theoretical floor — the fastest possible single-threaded io_uring performance. This isolates AuraIO's overhead from any benchmark framework noise.

**Why single-threaded, single-ring?**
Multi-threading introduces scheduling noise, lock contention, and ring selection overhead — all of which are separate concerns with their own benchmarks. The regression test is specifically about per-operation overhead: the cost of AIMD checks, request pool management, callback dispatch, histogram sampling, and mutex acquisition that AuraIO adds on every I/O.

**Why fixed batch submission (default: 8 SQEs per submit)?**
The raw io_uring loop submits a fixed batch of SQEs per `io_uring_submit()` call. The default of 8 matches AuraIO's `ADAPTIVE_TARGET_SQE_RATIO` — the target number of SQEs per syscall that the internal batch optimizer converges to. This makes the comparison apples-to-apples: both paths amortize the `io_uring_enter` syscall over the same number of operations. Submitting one-at-a-time would unfairly penalize the raw baseline; filling the entire SQ would mask per-op overhead in syscall amortization.

**Why sweep queue depths?**
Different depths exercise different bottlenecks. At low depth (32), the overhead is dominated by syscall frequency. At high depth (256), it shifts to lock contention and memory pressure. Sweeping finds each implementation's optimal operating point and compares them at their best — not at an arbitrary fixed depth that might favor one over the other.

**Three AuraIO configurations tested:**
1. **Static (adaptive disabled)** at the raw-optimal depth — pure apples-to-apples comparison of the engine's per-op machinery with no background tuning
2. **Adaptive** at depth=256 with AIMD enabled — measures the full stack including the tick thread, histogram recording, and limit adjustments. Uses extended warmup (2s vs 1s default) to let AIMD converge before measurement begins
3. The AIMD converged depth is reported to show whether the controller found a similar optimum to the sweep

**Latency sampling (1-in-8 AIMD, 1-in-64 passthrough):**
Both paths use identical `clock_gettime(CLOCK_MONOTONIC)` sampling. When AIMD is active, AuraIO samples at 1-in-8 rate (`RING_LATENCY_SAMPLE_RATE` = 8). In passthrough mode, sampling is reduced to 1-in-64 (`PASSTHROUGH_SAMPLE_MASK` = 0x3F), cutting `clock_gettime()` overhead by 8x. The regression test forces AIMD mode to compare like-for-like with the raw baseline.

**Temp file with unlink:**
The default test file is created via `mkstemp` + `fallocate(1GB)` + `unlink`. The unlink ensures automatic cleanup even on crash. Data is written with random bytes to prevent the filesystem from returning zeros for unwritten regions. `O_DIRECT` is enabled via `fcntl(F_SETFL)` to bypass the page cache — the AIMD controller needs to see true device latency, and cached reads would make both paths appear identically fast.

**Time-bound execution:**
Each test phase runs for a configurable duration (default 5s measurement + 1s warmup) rather than a fixed op count. This ensures stable measurements regardless of device speed — fast NVMe and slow HDD both get enough samples for reliable statistics. The offset table (1M entries) wraps around, so tests can run for arbitrary durations.

### Running

```bash
# Default: creates temp 1GB file, sweeps depths 32/64/128/256, 5s per test
make perf-regression

# Against a real device (best for stable numbers)
AURA_PERF_FILE=/dev/nvme0n1 make perf-regression

# Quick smoke test (2s per test, 1s warmup)
cd tests && ./perf_regression --duration 2 --warmup 1 --depths 32,64

# Custom batch size (e.g., compare batch=1 vs batch=16 vs batch=64)
./perf_regression --batch-size 16 --verbose
```

### Interpreting Results

The test reports IOPS overhead as a percentage: `(raw_IOPS - aura_IOPS) / raw_IOPS * 100`. The exit code is 0 (PASS) if overhead is below the threshold (default 10%), 1 (FAIL) if above.

**Negative overhead** (AuraIO faster than raw) is common and expected — AuraIO's batch flush logic often achieves better syscall amortization than the fixed-batch raw loop. This is a feature: the adaptive controller's batch optimizer is doing its job.

**When overhead is positive**, it represents the real cost of: mutex lock/unlock per submission, request slot allocation, atomic counter updates, latency sampling, callback dispatch, and the AIMD tick thread. On fast media (tmpfs, NVMe), expect 2-8% overhead. On slower media (SATA SSD, HDD), the per-op overhead is dwarfed by device latency and effectively rounds to 0%.

### Threshold Guidance

- **10% (default)**: Conservative. Appropriate for general-purpose regression detection.
- **5%**: Strict. Use for NVMe-focused deployments where per-op overhead is visible.
- **15-20%**: Relaxed. Appropriate when adaptive tuning provides significant value (e.g., mixed workloads, variable-latency storage) and the overhead is an acceptable trade for auto-tuning.

See [API Reference](api_reference.md) for full function signatures and [Architecture](architecture.md) for internal design details.

---

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for details.
