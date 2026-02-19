<!-- SPDX-License-Identifier: Apache-2.0 -->

# sspa — Simple Storage Performance Analyzer (SSPA)

## NAME

sspa — quick storage health check using real-world I/O patterns

## SYNOPSIS

```
sspa [options] [path] [size]
```

## DESCRIPTION

**sspa** is a zero-config storage performance analyzer. Point it at a directory and it runs 8 workloads that simulate real application I/O patterns, then reports bandwidth, IOPS, and latency for each.

It creates a temporary test file, runs each workload for a fixed duration, and cleans up when done. The test file is also removed on SIGINT.

## OPTIONS

| Option | Description |
|--------|-------------|
| `-l MS` | Max P99 latency target in milliseconds. When set, AuraIO's AIMD adaptive tuning backs off concurrency when P99 exceeds the target and ramps up when it's under — keeping latency in check while maximizing throughput. Default: unset (no latency constraint). |
| `-h`, `-?` | Show usage help and exit. |

## ARGUMENTS

| Argument | Description |
|----------|-------------|
| `path` | Directory to test in. Default: current directory |
| `size` | Test file size (e.g. `512M`, `1G`, `2G`). Default: auto |

### Auto-sizing

When no size is given, sspa uses 10% of the filesystem's free space, clamped to 256 MiB – 1 GiB. If 10% of free space is less than 256 MiB (i.e. less than 2.56 GiB free), sspa exits with an error.

An explicit size overrides the 1 GiB cap but must still be at least 256 MiB.

### Test duration

Each workload runs for 10 seconds. The first 3 seconds are always a warmup period (I/O runs but results are not recorded), so reported statistics reflect ~7 seconds of steady-state measurement. This ensures consistent measurement windows regardless of whether `-l` is set. Total runtime is roughly 80 seconds (8 × 10s) plus the time to create the test file.

## WORKLOADS

sspa runs 8 tests, each modeling a different storage access pattern. "Thr" shows how many threads are used — `N` means one per CPU core.

| Test | Pattern | IO Size | R/W | Threads | What it models |
|------|---------|---------|-----|---------|----------------|
| **Backup** | Sequential write | 512K | 0/100 | 1 | Single-stream backup or file copy write side. Measures sustained sequential write bandwidth. |
| **Recovery** | Sequential read | 512K | 100/0 | 1 | Single-stream restore or file scan. Measures sustained sequential read bandwidth. |
| **Database** | Random read/write mix | 4K | 70/30 | N | OLTP database with concurrent clients. 70% reads (point lookups) and 30% writes (updates/inserts). Small random I/O at high concurrency. |
| **Cache** | Random read | 4K | 100/0 | N | Read-heavy cache or CDN workload. All threads issuing random 4K reads — tests IOPS ceiling for reads. |
| **Logging** | Multi-stream sequential write | 4K | 0/100 | 4 | Application log writers or WAL flushes. Four independent sequential write streams with small I/O — tests how the device handles multiple concurrent sequential writers. |
| **KV-Store** | Append write + random read | 64K | 80/20 | N | Log-structured key-value store (LSM-tree). One thread appends 64K blocks sequentially (compaction/flush), remaining threads do random 64K reads (get operations). |
| **Training** | Sequential read + random seeks | 256K | 100/0 | 2 | ML data loader. Thread 0 reads sequentially (streaming training data), thread 1 reads random offsets (prefetcher/shuffler). Tests mixed sequential+random read performance. |
| **Lakehouse** | Scan + compact + lookup | 1M | 60/40 | N | Analytics lakehouse (Parquet/Delta Lake). ~40% of threads do sequential scans, ~30% do sequential compaction writes, ~30% do random 4K metadata lookups. Tests mixed large-sequential and small-random I/O. |

### Thread roles

For multi-threaded workloads, each thread gets a specific role:

- **Database / Cache**: All threads do the same thing (random I/O with the workload's read/write ratio).
- **Logging**: All 4 threads write sequentially, each to its own region of the file.
- **KV-Store**: Thread 0 is the sequential append writer; all other threads are random readers.
- **Training**: Thread 0 reads sequentially; thread 1 reads randomly.
- **Lakehouse**: Threads are divided roughly 40/30/30 between sequential scan, sequential compaction write, and random metadata lookup. The metadata lookup threads use 4K I/O regardless of the workload's nominal 1M I/O size.

## OUTPUT

```
sspa: /mnt/data  (1,024 MiB test file, 10s per test)

  Test         Pattern              IO Size   R/W %  Thr   Bandwidth       IOPS    Avg Lat   P99 Lat
  ──────────────────────────────────────────────────────────────────────────────────────────────────
  Backup       sequential write      512K    0/100    1    1,847 MB/s      3,694    0.54 ms   1.20 ms
  Recovery     sequential read       512K    100/0    1    2,103 MB/s      4,206    0.47 ms   0.98 ms
  Database     random rw mix           4K    70/30    8      349 MB/s     89,412    0.09 ms   0.41 ms
  Cache        random read             4K    100/0    8      487 MB/s    124,830    0.06 ms   0.31 ms
  Logging      multi-stream write      4K    0/100    4      774 MB/s    198,201    0.02 ms   0.08 ms
  KV-Store     append+rand read       64K    80/20    8      312 MB/s      4,992    0.40 ms   1.10 ms
  Training     seq read+shuffle      256K    100/0    2    1,956 MB/s      7,824    0.51 ms   1.15 ms
  Lakehouse    scan+compact+lookup     1M    60/40    8    1,534 MB/s      1,534    0.65 ms   1.80 ms

  Total I/O: 12,847,210 ops  (42% read, 58% write)

done.
```

### Column definitions

| Column | Description |
|--------|-------------|
| **Test** | Workload name |
| **Pattern** | Short description of the I/O pattern |
| **IO Size** | Size of each individual read or write operation |
| **R/W %** | Read/write percentage split (from workload definition) |
| **Thr** | Number of threads used |
| **Bandwidth** | Total throughput in MB/s (decimal megabytes, 10^6) across all threads |
| **IOPS** | Total I/O operations per second across all threads |
| **Avg Lat** | Mean per-operation latency (submit to completion) |
| **P99 Lat** | 99th percentile latency |

All numeric columns use comma thousand separators. Latency is measured per-operation from `clock_gettime(MONOTONIC)` at submission to completion callback.

## EXAMPLES

Test current directory with auto-sized file:

```bash
sspa
```

Test a specific mount point:

```bash
sspa /mnt/nvme0
```

Test with an explicit 2 GiB file (overrides the 1 GiB auto cap):

```bash
sspa /mnt/nvme0 2G
```

Set a 5 ms P99 latency target (AuraIO AIMD tuning will throttle concurrency to stay under):

```bash
sspa -l 5 /mnt/nvme0
```

Run inside the OrbStack Linux container (macOS development):

```bash
orb -m linux bash -c "sspa /var/tmp 512M"
```

## TIPS

- **Use a real filesystem**, not tmpfs. Testing on `/tmp` if it's tmpfs will benchmark RAM, not storage.
- **Larger files** give more representative results for random workloads. At 256 MiB, the entire file may fit in the page cache after the first test.
- **O_DIRECT** is used when supported. If the filesystem doesn't support it (e.g. tmpfs, some network filesystems), sspa falls back to buffered I/O silently.
- **Page cache** is dropped between tests via `posix_fadvise(DONTNEED)`. This is advisory — for true cold-cache reads, use `echo 3 > /proc/sys/vm/drop_caches` (requires root) before running.
- **Cross-validate** against fio for specific workloads if numbers seem off:
  ```bash
  # Example: validate sequential write
  fio --name=seqwrite --filename=/tmp/fio.tmp --size=256M --bs=1M \
      --rw=write --ioengine=io_uring --iodepth=32 --direct=1 \
      --runtime=10 --time_based
  ```

## EXIT STATUS

| Code | Meaning |
|------|---------|
| `0` | All tests completed |
| `1` | Error (insufficient space, I/O failure, bad arguments) |
| `130` | Interrupted by SIGINT |

## BUILDING

```bash
# From AuraIO root
make sspa

# Or from the tool directory
cd tools/sspa
make
```

**Dependencies**: liburing, pthreads

The binary is placed at `tools/bin/sspa`.

---

## Implementation

### Architecture

```
main()
  ├── parse args, auto-size test file
  ├── create test file (sequential write, 1 MiB chunks)
  ├── open with O_DIRECT (buffered fallback)
  ├── for each workload:
  │     ├── posix_fadvise(DONTNEED) to drop cache
  │     ├── run_test(fd, workload, threads, duration)
  │     │     ├── single-thread: worker_run() directly
  │     │     └── multi-thread: spawn N pthreads → worker_run() each
  │     ├── aggregate results across threads
  │     └── print formatted row
  ├── print summary
  └── cleanup (unlink test file, atexit + SIGINT handler)

worker_run(wctx, duration)
  ├── create AuraIO engine (single_thread=true, 1 ring, depth 128)
  ├── allocate 32 pipeline buffer slots
  ├── fill pipeline: submit_one() × 32
  └── event loop:
        ├── aura_wait(engine, 100ms)
        ├── retry FREE slots (EAGAIN recovery)
        └── check elapsed time → break when done
```

### Callback-driven pipeline

Each I/O slot cycles through two states: `SLOT_FREE` and `SLOT_INFLIGHT`. When a completion fires:

1. Mark slot `SLOT_FREE`, decrement `active_ops`
2. Record latency in per-thread histogram
3. Accumulate bytes/ops counters
4. If not stopping: call `submit_one()` to immediately resubmit

This keeps the pipeline full without polling for free slots. If `submit_one()` gets `EAGAIN` from the engine (ring full), the slot stays `FREE` and the event loop's retry scan picks it up on the next iteration.

### Per-thread PRNG

Random offsets use a `_Thread_local` xorshift64 PRNG seeded from `thread_id ^ clock_gettime(MONOTONIC)`. This eliminates cache-line contention that a shared RNG would cause, which matters at 100K+ IOPS where a shared atomic RNG becomes a bottleneck.

### Latency measurement

Each slot records `clock_gettime(CLOCK_MONOTONIC)` at submission. The completion callback computes the delta. Latencies are binned into a per-thread histogram with 4096 buckets at 10 microsecond resolution (range: 0–40.96 ms). After each test, per-thread histograms are merged and percentiles are computed by walking the cumulative distribution.

### Thread roles

The `assign_thread_role()` function configures each worker based on the workload pattern and the thread's index within the pool. Workers are fully independent — each gets its own AuraIO engine and buffer slots. The only shared state is the file descriptor (reads are safe to share across threads) and `g_interrupted` (sig_atomic_t, set by SIGINT handler).

### Data pattern

The test file and write buffers use incompressible, non-deduplicable data to ensure accurate results on data-reducing storage arrays (compression, dedup offload):

- **Test file creation**: Each 1 MiB chunk is filled with xorshift64 PRNG output, then stamped with the file offset in the first 8 bytes. This makes every chunk unique (defeats dedup) and pseudo-random (defeats compression).
- **Runtime write buffers**: Each pipeline slot is PRNG-filled at init. Before each write submission, the current file offset is stamped into the first 8 bytes, ensuring every written block is unique even across resubmissions to the same offset.

This mirrors the approach used by fio's `refill_buffers` option.

### Test file lifecycle

The test file (`.sspa.tmp`) is created by sequential writes (not fallocate) to ensure blocks are actually allocated on disk. It is registered with `atexit()` and a SIGINT handler for cleanup. The file is opened once with `O_RDWR | O_DIRECT` and the same fd is shared across all tests and threads.
