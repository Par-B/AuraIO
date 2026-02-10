# BFFIO vs FIO Performance Gap Investigation

**Date**: 2026-02-08
**Test environment**: OrbStack Linux VM (tmpfs), single-threaded (numjobs=1)
**Workload**: randread, bs=4K, direct=0, runtime=10s, time_based

## Summary

BFFIO is ~19-28% slower than FIO depending on configuration. The gap is **not** in BFFIO's callback or stats code -- it's in AuraIO's per-I/O library overhead (mutexes, atomics, request pool management).

## Benchmark Data

### Baseline (before optimizations)

| Config | FIO IOPS | BFFIO IOPS | Delta |
|--------|----------|------------|-------|
| 1 file, iodepth=128 | 1,504K | ~1,188K | **-21%** |
| 5 files, iodepth=128 | 1,657K | 1,188K | **-28%** |

### BFFIO Optimizations Tried (cumulative)

| Optimization | BFFIO IOPS (median) | Delta vs Baseline |
|-------------|---------------------|-------------------|
| Baseline (12 rings, spinlock pool) | 1,188K | -- |
| + Batch clock_gettime (submit+completion) | 1,201K | +1.1% (noise) |
| + Lock-free pool (Treiber stack) | 1,184K | -0.3% (noise) |
| + ring_count=1 for numjobs=1 | 1,221K | +2.8% |
| + Minimal callback (zero stats) | 1,185K | -0.3% (noise) |

### Single File vs Multi-File

| Config | FIO | BFFIO | Delta |
|--------|-----|-------|-------|
| nrfiles=1 (single file) | 1,504K | 1,219K | **-19%** |
| nrfiles=5 (multi-file) | 1,686K | 1,221K | **-28%** |

Key finding: FIO gains +12% from multiple files (tmpfs VFS parallelism). BFFIO stays flat because with ring_count=1 all I/O goes to one ring. The single-file gap (-19%) is the true structural cost.

## Root Cause Analysis

### Why the minimal callback test is definitive

Even with a callback that does ZERO latency tracking (just counts ops+bytes, 2 atomic_fetch_add), BFFIO achieves only 1,185K IOPS -- identical to the full-stats version. This proves BFFIO's callback overhead is negligible. The bottleneck is AuraIO's internal per-I/O path.

### AuraIO per-I/O overhead breakdown

**Submission path** (per I/O):
1. Ring selection: ~2-3 atomics (adaptive mode reads load from candidate rings)
2. `pthread_mutex_lock(&ring->lock)`: acquire submission lock
3. `ring_get_request()`: pop from free request stack
4. SQE prep: io_uring_get_sqe + io_uring_prep_read + io_uring_sqe_set_data
5. `atomic_store(&req->pending, true)`: mark in-flight
6. `atomic_fetch_add(&ring->pending_count, 1)`: increment pending
7. `ring->queued_sqes++`: batch counter
8. Auto-flush check: may call `io_uring_submit()` + `adaptive_record_submit()`
9. `pthread_mutex_unlock(&ring->lock)`: release

**Completion path** (per I/O, from `auraio_poll` -> `ring_poll`):
1. `pthread_mutex_lock(&ring->cq_lock)`: acquire CQ lock
2. `io_uring_peek_cqe()`: check for completion
3. `io_uring_cqe_seen()`: mark CQE consumed
4. `pthread_mutex_unlock(&ring->cq_lock)`: release
5. `process_completion()`:
   - Maybe `get_time_ns()` + `adaptive_record_completion()` (1/8 sample rate)
   - `atomic_store(&req->pending, false)`: clear in-flight
   - **User callback fires here** (negligible cost as proven above)
6. `pthread_mutex_lock(&ring->lock)`: acquire for counter update
7. `ctx->ops_completed++`, `ctx->bytes_completed += result`
8. `atomic_fetch_sub(&ring->pending_count, 1)`: decrement pending
9. `ring_put_request()`: return slot to free stack
10. `pthread_mutex_unlock(&ring->lock)`: release

**Total per I/O**: ~4 mutex lock/unlock pairs + ~8-12 atomic operations + request pool alloc/free + AIMD sampling. Each mutex lock/unlock is ~20-30ns uncontended. Each atomic is ~5-10ns. Total library overhead: ~200-250ns per I/O.

At 1.2M IOPS: 200ns * 1.2M = 240ms/sec = **24% of wall time** -- matches the observed gap.

### FIO comparison (why it's faster)

FIO's io_uring path per I/O:
1. Get SQE
2. Prep read/write
3. Set data
4. io_uring_submit (batched)
5. Peek CQE
6. Process result
7. Mark CQE seen

No mutexes (single-threaded), no atomics, no adaptive engine, no ring selection, no request pool management. Just direct io_uring calls. Total overhead: ~50ns per I/O.

## Potential Engine Optimizations (not yet implemented)

### High impact

1. **Batch CQE processing**: `ring_poll` currently does lock-peek-unlock per CQE. Use `io_uring_for_each_cqe()` to process N completions under one cq_lock acquisition. Saves (N-1) mutex lock/unlock pairs per poll cycle.

2. **Combine completion lock regions**: `process_completion` acquires `ring->lock` for counter updates + request return. This could be batched: process all completions, then do one lock acquisition to update counters and return all slots.

3. **Single-thread fast path**: When engine has only 1 ring and caller is single-threaded, skip mutexes entirely. Use a flag or compile-time option to enable lock-free operation.

### Medium impact

4. **Reduce pending_count atomics**: `pending_count` is incremented at submit (under lock) and decremented at completion (under lock). Since both happen under `ring->lock`, they could be plain integers instead of atomics. The atomic is only needed for `ring_can_submit()` which reads it without the lock.

5. **Pool-free request management**: Instead of a separate free stack, use a generation counter on request slots. Eliminates `ring_get_request` / `ring_put_request` overhead.

6. **Multi-file ring distribution**: With nrfiles>1 and multiple rings, distribute files across rings so each ring handles a subset. Enables per-ring file affinity.

### Low impact (already tested)

7. BFFIO clock_gettime batching: +1% (adaptive sampling already limits to ~100K/sec/thread)
8. BFFIO lock-free pool: 0% (uncontended spinlock is already fast)
9. BFFIO ring_count=1: +3% (eliminates empty ring polling overhead)

## Changes Kept from This Session

The following BFFIO changes were implemented and are in the working tree:

1. **Batch submit timestamps** (`workload.c`): One `now_ns()` per submission batch instead of per I/O. Clean code improvement even though impact is negligible.

2. **Batch completion timestamps** (`workload.c`): TLS `tls_completion_ns` cached per poll cycle. First callback calls `now_ns()`, subsequent reuse it.

3. **Lock-free io_ctx_pool** (`workload.h`, `workload.c`): Replaced `pthread_spinlock_t` + `free_stack[]` with Treiber stack using `_Atomic int free_head` + per-slot `_Atomic int next_free`. MPSC safe (single consumer get, multi-producer put). Better design even though no measurable speed difference.

4. **ring_count=1 for numjobs<=1** (`main.c`): Avoids creating 12 rings when only 1 thread is driving I/O. Small but consistent +3% improvement.

## How to Reproduce

```bash
# Build
orb -m linux bash -c "make clean && make -j4 && make -C tools/BFFIO clean all"

# BFFIO benchmark (single file)
orb -m linux bash -c "tools/bin/BFFIO --rw=randread --bs=4K --size=250M \
  --directory=/tmp/bffio_perf --nrfiles=1 --direct=0 --runtime=10 --time_based \
  --iodepth=128 --numjobs=1 --output-format=json"

# FIO reference (single file)
orb -m linux bash -c "fio --name=ref --rw=randread --bs=4k --size=250M \
  --directory=/tmp/bffio_perf --nrfiles=1 --direct=0 --runtime=10 --time_based \
  --ioengine=io_uring --iodepth=128 --numjobs=1 --output-format=json"
```
