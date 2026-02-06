# AuraIO Performance Architecture

## Design Philosophy

AuraIO is built on the principle of **Hardware-Aware I/O**. It does not treat the kernel as a black box; instead, it aligns its internal structures with the physical realities of the hardware (CPU cores, cache lines, and NUMA nodes) to extract maximum IOPS with minimal latency jitter.

## 1. Zero-Copy & Kernel Bypass

### Registered Buffers
Standard `read/write` syscalls involve copying data between kernel space and user space. AuraIO mitigates this overhead through **Registered Buffers** (`auraio_register_buffers`).
- **Mechanism**: Buffers are pinned in memory and mapped into the kernel's address space once at startup.
- **Benefit**: I/O operations using these buffers (`AURAIO_BUF_REGISTERED`) bypass the page mapping overhead entirely, essential for high-frequency small I/O (< 16KB).

### Registered Files
Every unregistered I/O operation forces the kernel to look up the internal `struct file` via an atomic reference count (`fget`/`fput`). AuraIO allows pre-registering file descriptors (`auraio_register_files`) to pin these references at startup.
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

### The "Sticky Ring" Strategy
Cache misses are expensive. AuraIO ensures that data stays hot in the L1/L2 cache of the CPU core processing it.
- **Submission**: Each thread maintains a TLS-cached CPU ID (via `sched_getcpu()`, refreshed every 32 submissions). I/O operations route to the `io_uring` instance dedicated to that core, avoiding cross-core contention without a syscall on every request.
- **Completion**: With cooperative task delivery (`IORING_SETUP_COOP_TASKRUN`, enabled automatically on kernel 5.19+), completions are processed on the submitting core rather than via interrupts. This maximizes the chance that the user thread reads CQE data from L2 cache.

## 4. Adaptive Congestion Control (The "Brain")

Performance is not just about raw speed but about **consistent latency**. AuraIO embeds an intelligent controller to manage queue depth.

### Double-Loop Control System
1.  **Inner Loop (Batch Optimizer)**:
    - Monitors the ratio of `SQE Submitted` vs `Syscalls Made`.
    - Dynamically tunes batch sizes to amortize the cost of the `io_uring_submit` syscall without adding artificial latency to individual requests.

2.  **Outer Loop (Throughput/Latency Manager)**:
    - Uses **AIMD** (Additive Increase Multiplicative Decrease) logic.
    - Tuned for I/O patterns consistent with both local NVMe and network latencies. Self-adapts to the hardware it runs on — works equally well with high-performance NVMe and 7200 RPM magnetic platter storage.
    - Self-adjusting variable-width sample windows based on IOPS rate to avoid low-IOPS workloads skewing the sample results.
    - **P99 Guard Logic**: It continuously samples the 99th percentile latency. If latency exceeds the guard threshold (default 10ms jitter), it immediately slashes concurrency. This prevents "Bufferbloat" at the device level, keeping the I/O piping full but not overflowing.

## 5. Performance Tips for Users

To fully leverage this architecture:
1.  **Pin Your Threads**: Use `pthread_setaffinity_np` in your worker threads. AuraIO routes I/O to per-core rings via `sched_getcpu()`. Pinning ensures stable core affinity, keeping ring data and CQE memory hot in L1/L2 cache.
2.  **Use `O_DIRECT`**: This bypasses the kernel page cache, allowing AuraIO's adaptive controller to see the *true* device latency and tune accurately.
3.  **Pre-register Memory**: For long-running applications, register your buffer pool at startup to shave off microseconds from every operation.

## 6. High-Performance Deployment (64+ Cores, NVMe Arrays, High-Speed Networks)

AuraIO is designed to scale linearly on large-core servers with high-bandwidth storage. Its per-ring isolation model (independent locks, AIMD controllers, and request pools per ring) eliminates cross-core contention on the I/O hot path. This section covers the kernel-side optimizations that become critical when targeting millions of IOPS.

### Registered Files — Eliminate Per-Op Kernel Overhead

**Why it matters**: Every unregistered I/O operation forces the kernel to call `fget()` / `fput()` — an atomic reference count increment/decrement on the file's internal `struct file`. At millions of IOPS across hundreds of cores, this single atomic becomes a severe cross-core cache-line contention point *inside the kernel*, invisible to userspace profiling. File registration eliminates this entirely by pinning the file reference at setup time.

**Impact**: On a 128-core system doing 2M IOPS, unregistered file access can waste 10-20% of kernel CPU time on `fget`/`fput` contention. Registration reduces this to zero.

**How to use (C)**:

```c
// At startup: register all file descriptors
int fds[] = { fd1, fd2, fd3, /* ... */ };
auraio_register_files(engine, fds, count);

// I/O operations automatically use registered files when available.
// No change needed to auraio_read/auraio_write calls.

// To replace a file descriptor (e.g., log rotation):
auraio_update_file(engine, index, new_fd);

// At shutdown:
auraio_unregister_files(engine);
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
engine.unregister_files();
```

**Guidelines**:
- Register all file descriptors that will be used for high-frequency I/O at engine startup.
- Registration is applied to all rings and requires a brief lock per ring — do it once, not per-request.
- The `auraio_update_file()` function allows replacing individual file descriptors without unregistering the entire set (useful for log rotation or connection recycling).
- There is no AuraIO-imposed limit on registered file count; the limit comes from the kernel (typically 64K+ on modern kernels).

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
auraio_register_buffers(engine, iovs, NUM_BUFFERS);

// Use registered buffers by index:
auraio_read(engine, fd, auraio_buf_fixed(0, 0), BUF_SIZE, offset, cb, data);

// For sub-buffer offsets (reading into the middle of a registered buffer):
auraio_read(engine, fd, auraio_buf_fixed(0, 2048), 2048, offset, cb, data);

// At shutdown:
auraio_unregister_buffers(engine);
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
engine.read(fd, auraio::BufferRef::fixed(0), 4096, offset, callback);

// Cleanup:
engine.unregister_buffers();
```

**Guidelines**:
- Register buffers for your hottest I/O paths (metadata reads, index lookups, small random I/O).
- Registered buffers must remain valid and at the same address for the lifetime of the registration.
- No in-flight operations may reference registered buffers when calling `unregister_buffers`.
- You can mix registered and unregistered buffers freely — use `auraio_buf()` for regular buffers and `auraio_buf_fixed()` for registered ones.

### SQPOLL — Kernel-Side Submission Polling

**Why it matters**: In the default mode, every `io_uring_submit()` call requires a `io_uring_enter` syscall — a user-to-kernel context switch. AuraIO batches submissions to amortize this (typically ~16 ops per syscall), but the syscall overhead still consumes CPU cycles. SQPOLL mode creates a dedicated kernel thread that continuously polls the submission queue, eliminating the syscall entirely on the submission path.

**Impact**: Reduces submission latency by ~1-2 microseconds per batch. Most impactful for latency-sensitive workloads (database WAL writes, key-value store gets) where every microsecond matters. Less impactful for bulk throughput where batching already amortizes the syscall cost.

**How to use (C)**:

```c
auraio_options_t opts;
auraio_options_init(&opts);
opts.enable_sqpoll = true;
opts.sqpoll_idle_ms = 2000;  // Kernel thread sleeps after 2s idle
auraio_engine_t *engine = auraio_create_with_options(&opts);
```

**How to use (C++)**:

```cpp
auraio::Options opts;
opts.enable_sqpoll(true)
    .sqpoll_idle_ms(2000);
auraio::Engine engine(opts);
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
| 1. Register file descriptors | `auraio_register_files()` | Eliminates kernel `fget`/`fput` atomic contention per-op |
| 2. Register I/O buffers | `auraio_register_buffers()` | Eliminates kernel page-pinning per-op |
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
- **TLS-cached CPU routing**: The result of `sched_getcpu()` is cached in thread-local storage and refreshed every 32 submissions, avoiding a syscall on every I/O operation.
- **Size-class lookup**: Common buffer sizes (up to 256KB) use a comparison cascade rather than a `CLZ` instruction, which is faster on most x86 microarchitectures for the typical 4KB-64KB range.

### Lock Batching

Where locks are required, AuraIO amortizes the cost by doing more work per acquisition:

- **Completion queue drain**: The `cq_lock` is held for up to 32 CQE extractions per acquisition, rather than lock-per-CQE.
- **Wait path**: A single lock acquisition covers the flush, pending-request check, and batch extraction — three operations that previously required separate lock cycles.

### Per-Operation Overhead Reduction

Micro-optimizations that compound at high IOPS:

- **Request initialization**: Only the 8 fields needed for a new request are written (targeted stores), rather than zeroing the full struct with `memset`.
- **Buffer allocation fast path**: The TLS cache validity check (pointer + generation ID comparison) runs before the atomic `pool->destroyed` check, keeping the common case branch-free.
- **Inline hot getters**: The adaptive controller's `inflight_limit` and `batch_threshold` are computed via `static inline` functions in the header, eliminating function call overhead on every submission.

### Kernel Cooperation

AuraIO configures `io_uring` to minimize kernel-userspace friction:

- **`IORING_SETUP_COOP_TASKRUN`** (kernel 5.19+): Completions are delivered cooperatively when the application polls, rather than via asynchronous interrupts. This reduces context switches and keeps completion data on the submitting core's cache.
- **Merged flush+poll**: The wait path performs submission and completion reaping in a single pass, with an early exit as soon as the first completions arrive — avoiding unnecessary re-entry into the kernel.

See [API Reference](api_reference.md) for full function signatures and [Architecture](architecture.md) for internal design details.
