# AuraIO Performance Architecture

## Design Philosophy

AuraIO is built on the principle of **Hardware-Aware I/O**. It does not treat the kernel as a black box; instead, it aligns its internal structures with the physical realities of the hardware (CPU cores, cache lines, and NUMA nodes) to extract maximum IOPS with minimal latency jitter.

## 1. Zero-Copy & Kernel Bypass

### Registered Buffers
Standard `read/write` syscalls involve copying data between kernel space and user space. AuraIO mitigates this overhead through **Registered Buffers** (`auraio_register_buffers`).
- **Mechanism**: Buffers are pinned in memory and mapped into the kernel's address space once at startup.
- **Benefit**: I/O operations using these buffers (`AURAIO_BUF_REGISTERED`) bypass the page mapping overhead entirely, essential for high-frequency small I/O (< 4KB).

### Registered Files
Similar to buffers, file descriptors can be pre-registered to avoid the overhead of looking up the internal file structure in the kernel for every operation.

## 2. Lock-Free Memory Management

The memory allocator (`adaptive_buffer.c`) is designed to be effectively lock-free for the "Fast Path".

### Multi-Tiered Allocation
1.  **Thread Cache (L1)**: Each thread maintains a stack of free buffers. Allocation and deallocation here are `O(1)` and require **zero atomic operations** and **zero locks**.
2.  **Sharded Global Pool (L2)**: If the local cache is empty, the thread acquires a lock on a specific *shard*. The pool is sharded by CPU count (up to 64 shards) to ensure that contention remains negligible even on massive servers.

### Alignment
All buffers are automatically aligned to 4096 bytes (or custom alignment) to ensure compatibility with `O_DIRECT`, preventing costly data copying inside the kernel's block layer.

## 3. CPU Locality & Cache Efficiency

### The "Sticky Ring" Strategy
Cache misses are expensive. AuraIO ensures that data stays hot in the L1/L2 cache of the CPU core processing it.
- **Submission**: `auraio_read` checks the current CPU ID. It routes the request to the `io_uring` instance dedicated to that specific core.
- **Completion**: Since the ring is bound to the core, the kernel writes the completion event (CQE) to memory associated with that core, maximizing the chance that the user thread (which is likely still on that core) reads it from L2 cache.

## 4. Adaptive Congestion Control (The "Brain")

Performance is not just about raw speed but about **consistent latency**. AuraIO embeds an intelligent controller to manage queue depth.

### Double-Loop Control System
1.  **Inner Loop (Batch Optimizer)**:
    - Monitors the ratio of `SQE Submitted` vs `Syscalls Made`.
    - Dynamically tunes batch sizes to amortize the cost of the `io_uring_submit` syscall without adding artificial latency to individual requests.

2.  **Outer Loop (Throughput/Latency Manager)**:
    - Uses **AIMD** (Additive Increase Multiplicative Decrease) logic.
    - **P99 Guard Logic**: It continuously samples the 99th percentile latency. If latency exceeds the guard threshold (default 10ms jitter), it immediately slashes concurrency. This prevents "Bufferbloat" at the device level, keeping the I/O piping full but not overflowing.

## 5. Performance Tips for Users

To fully leverage this architecture:
1.  **Pin Your Threads**: Use `pthread_setaffinity_np` in your worker threads. AuraIO automatically detects this and aligns its rings to match your topology.
2.  **Use `O_DIRECT`**: This bypasses the kernel page cache, allowing AuraIO's adaptive controller to see the *true* device latency and tune accurately.
3.  **Pre-register Memory**: For long-running applications, register your buffer pool at startup to shave off microseconds from every operation.
