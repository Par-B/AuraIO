# BFFIO — Better Faster FIO

**BFFIO** is an FIO-compatible I/O benchmark that uses AuraIO as its sole I/O engine. It accepts the same CLI flags and `.fio` job files as FIO, but replaces FIO's static io_uring engine with AuraIO's AIMD adaptive tuning. This lets you run your existing FIO workloads on AuraIO and compare results directly.

## Why BFFIO?

FIO requires you to pick a fixed `iodepth`. Too low and you leave throughput on the table. Too high and latency balloons (bufferbloat). BFFIO eliminates this trade-off — AuraIO's AIMD controller automatically finds the optimal queue depth for your hardware and workload.

| Aspect | FIO | BFFIO |
|--------|-----|-------|
| Queue depth | Fixed (you guess) | Auto-tuned by AIMD |
| Latency at high depth | Scales linearly | Capped by backoff |
| Configuration | Manual tuning per workload | Zero config |
| Output format | FIO text + JSON | Identical (drop-in compatible) |

## Building

```bash
# From AuraIO root
make BFFIO

# Or from the tool directory
cd tools/BFFIO
make
```

## Usage

BFFIO accepts FIO-compatible CLI flags or `.fio` job files.

### CLI Mode

```bash
# Random 4K reads with O_DIRECT for 10 seconds
BFFIO --name=test --rw=randread --bs=4k --size=1G \
      --directory=/tmp/bffio --direct=1 --runtime=10 --time_based

# Sequential 64K writes
BFFIO --name=seqwr --rw=write --bs=64k --size=512M \
      --directory=/tmp/bffio --runtime=10 --time_based

# Mixed 70/30 read/write
BFFIO --name=mixed --rw=randrw --rwmixread=70 --bs=4k --size=1G \
      --directory=/tmp/bffio --direct=1 --runtime=10 --time_based

# Multi-threaded with aggregated stats
BFFIO --name=mt --rw=randread --bs=4k --size=1G \
      --directory=/tmp/bffio --numjobs=4 --group_reporting \
      --direct=1 --runtime=10 --time_based

# JSON output (pipe to jq, fio-plot, etc.)
BFFIO --name=test --rw=randread --bs=4k --size=1G \
      --directory=/tmp/bffio --direct=1 --runtime=10 --time_based \
      --output-format=json
```

### Job File Mode

BFFIO parses standard `.fio` INI files:

```ini
[global]
direct=1
runtime=10
time_based
directory=/tmp/bffio

[randread-4k]
rw=randread
bs=4k
size=1G

[seqwrite-64k]
rw=write
bs=64k
size=512M
```

```bash
BFFIO workload.fio
```

Each `[section]` runs as a separate job with a fresh AuraIO engine for clean AIMD convergence.

## Supported Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `rw` | (required) | `read`, `write`, `randread`, `randwrite`, `readwrite`, `randrw` |
| `bs` | `4K` | Block size. Suffixes: `K`, `M`, `G` |
| `size` | (required*) | File size. *Not required if `time_based` + `runtime` set |
| `filename` | — | Explicit file or block device path |
| `directory` | — | Directory for auto-created test files |
| `direct` | `0` | `1` for O_DIRECT |
| `runtime` | `0` | Run duration in seconds |
| `time_based` | `0` | Run for fixed time (requires `runtime`) |
| `ramp_time` | `0` | Warmup seconds — stats are discarded during ramp |
| `numjobs` | `1` | Worker threads |
| `iodepth` | `256` | Max queue depth cap (AuraIO auto-tunes below this) |
| `rwmixread` | `50` | Read percentage for mixed workloads (`randrw`, `readwrite`) |
| `group_reporting` | `0` | Aggregate stats across all threads into one result |
| `fsync` | `0` | Issue fsync every N writes |
| `target-p99` | `0` (disabled) | P99 latency ceiling. Suffixes: `us`, `ms`, `s`. Bare number = ms |
| `ioengine` | `AuraIO` | Accepted (case-insensitive) but always uses AuraIO |
| `nrfiles` | `1` | Number of files per job (directory mode only) |
| `filesize` | `size/nrfiles` | Per-file size. Suffixes: `K`, `M`, `G` |
| `file_service_type` | `roundrobin` | File selection: `roundrobin`, `sequential`, `random` |
| `ring-select` | `adaptive` | Ring selection: `adaptive`, `cpu_local`, `round_robin` |
| `output-format` | `normal` | `normal` (FIO text) or `json` (FIO 3.36 JSON) |
| `output` | — | Write output to FILE instead of stdout |

## Latency-Constrained Throughput (`--target-p99`)

FIO has no built-in way to answer: "What is the maximum throughput my device delivers while keeping P99 latency under X?" BFFIO can, because AuraIO's AIMD controller has a hard P99 ceiling — when violated, it immediately backs off.

```bash
# Find max throughput at P99 < 2ms (standard SSD)
BFFIO --name=sla --rw=randread --bs=4k --size=1G --directory=/tmp/test \
      --direct=1 --runtime=10 --time_based --target-p99=2ms

# Fast NVMe: P99 < 500 microseconds
BFFIO --name=sla --rw=randrw --rwmixread=70 --bs=4k --size=1G \
      --directory=/tmp/test --direct=1 --runtime=10 --time_based --target-p99=500us

# Fractional milliseconds
BFFIO --name=sla --rw=randread --bs=4k --size=1G --directory=/tmp/test \
      --direct=1 --runtime=10 --time_based --target-p99=1.5ms
```

The output reports the maximum concurrency (parallel I/Os) that the device sustained under the latency constraint:

```
  AuraIO: max concurrency=42 at p99=1.82ms (target: 2.00ms), phase=CONVERGED
```

Supported suffixes: `us` (microseconds), `ms` (milliseconds), `s` (seconds). A bare number is treated as milliseconds. Also accepted as `target_p99` in `.fio` job files.

### How It Works

When `--target-p99` is set, BFFIO orchestrates a three-phase lifecycle to find the maximum device concurrency under your latency ceiling:

**1. Warmup (ramp period)** — BFFIO auto-scales `numjobs` to match the CPU core count so every io_uring ring has a dedicated thread driving I/O. Each ring's AIMD controller starts at a low depth (4) and begins probing upward. All stats are discarded during this phase. If no explicit `--ramp_time` is given, BFFIO defaults to 5 seconds.

**2. AIMD convergence** — After the ramp, AIMD continues probing: increasing depth when latency is under the ceiling, backing off when P99 exceeds it. BFFIO polls each ring every second. Stats collected during this probing phase are provisional — throughput is lower than steady-state because depth is still ramping.

**3. Steady-state measurement** — When all active rings reach `STEADY` or `CONVERGED` phase, BFFIO resets all stats (IOPS, bandwidth, latency histograms) and begins fresh measurement. This ensures the reported numbers reflect the throughput *at the converged depth*, not the average across the probing ramp-up. If convergence is not detected (e.g., very short runtime), BFFIO falls back to reporting the full post-ramp measurement.

```
Timeline:

  |--- ramp (5s) ---|--- probing ---|--- steady-state measurement ---|
  stats discarded    stats collected   stats RESET, fresh measurement
                     (provisional)     (reported in output)
```

This means the IOPS and bandwidth in the output are per-second rates measured after AIMD has settled — they represent the real throughput your device delivers at the found concurrency level, not a diluted average that includes the low-depth warm-up period.

### Auto-Scaling Behavior

When `--target-p99` is set:

- **`numjobs` auto-scales** to match the number of CPU cores (one thread per io_uring ring). This ensures every ring's AIMD controller sees real I/O and converges independently. The sum of per-ring depths equals total device concurrency.
- **`group_reporting` auto-enables** so output shows device-level aggregate numbers rather than per-thread splits.
- **`ramp_time` defaults to 5s** if not explicitly set, giving AIMD time to converge across all rings.
- **`initial_in_flight` starts at 4** per ring (not the default `queue_depth/4`), so AIMD probes upward from a known-good latency point rather than backing off from an already-violating depth.

## Output

### Normal Text

FIO-style output with an added AuraIO tuning line:

```
test: (g=0): rw=randread, bs=(R) 4096B, ioengine=AuraIO, direct=1
  read: IOPS=142k, BW=555MiB/s (582MB/s)(16.3GiB/30001msec)
    lat (usec): min=5, max=18600, avg=51.29, stdev=16.79
    clat percentiles (usec):
     |  1.00th=[   42],  5.00th=[   45], 10.00th=[   46], ...
   bw (  KiB/s): min=524288, max=589824, per=100.00%, avg=555123.45, stdev=12345.67, samples=60
   iops        : min=131072, max=147456, avg=138780.86, stdev=3086.42, samples=60

  AuraIO: converged to depth 64, phase=CONVERGED, p99=0.08ms
```

### JSON

Matches the FIO 3.36 JSON structure (`jobs[].read/write` with `lat_ns`, percentiles, BW/IOPS samples). Compatible with `fio-plot`, `jq`, and any tool that parses FIO JSON.

Added `"Aura"` section per job:

```json
{
  "Aura": {
    "final_depth": 64,
    "phase": "CONVERGED",
    "p99_ms": 0.08,
    "throughput_bps": 610000000
  }
}
```

## Testing

### Functional Tests

```bash
make BFFIO-test
# or
cd tools/BFFIO && ./test_BFFIO.sh [--quick]
```

Runs 18 test cases covering: basic I/O patterns (randread, randwrite, seqread, seqwrite, mixed), JSON output validation, job file parsing, multi-job files, multi-thread with group_reporting, ramp time, size-based mode (no runtime), error handling, target-p99 latency mode (ms/us suffixes, concurrency output, JSON fields), and AuraIO branding.

### FIO Baseline Comparison

```bash
make BFFIO-baseline
# or
cd tools/BFFIO && ./run_baseline.sh [--quick|--standard|--full]
```

Runs matched workloads on both FIO and BFFIO with identical parameters, parses JSON from both, and generates a delta report showing IOPS, bandwidth, and latency differences. Results go to `tools/BFFIO/baseline_results/`.

Options:
- `--quick` — Short runs for rapid iteration
- `--standard` — 5s per test (default)
- `--full` — Longer runs for statistical confidence
- `--skip-fio` — Reuse previous FIO results

### Interpreting Baseline Results

BFFIO will typically show higher IOPS than FIO on small random I/O workloads (4K randread/randwrite), while large sequential workloads converge to identical throughput. This is a real structural difference, not a measurement error.

FIO's io_uring engine calls `io_uring_submit()` after preparing each individual SQE. AuraIO batches multiple SQEs into a single submit call, amortizing the kernel entry cost across many operations. On a 3-second randwrite test, FIO makes ~4,500 syscalls while BFFIO makes ~225 for the same workload — a 20× reduction.

This matters most on **low-latency storage** (NVMe, tmpfs, RAM-backed VM disks) where per-I/O device latency is microseconds and syscall overhead is a significant fraction of total time. On slower media (SATA SSDs, HDDs), device latency dominates and the gap narrows. On bandwidth-saturated workloads (large sequential I/O), both tools hit the same device limit regardless of submission strategy.

FIO's `sqthread_poll=1` option partially closes the gap by offloading submission to a kernel thread, but it requires `CAP_SYS_NICE` and still doesn't batch SQE preparation. The baseline comparison uses FIO's default configuration since that's what most users run.

## Architecture

```
main.c
 ├─ job_parser.c    Parse CLI or .fio → bench_config_t (1+ jobs)
 │
 ├─ For each job:
 │    ├─ Create fresh AuraIO engine (iodepth as max cap)
 │    ├─ workload.c   Spawn numjobs threads, run I/O loop
 │    │    └─ io_ctx_pool_t   Pre-allocated, zero-malloc hot path
 │    ├─ stats.c      Aggregate per-thread stats, compute percentiles
 │    └─ Destroy engine
 │
 └─ output.c    Render results as text or JSON
```

**Key design decisions:**

- **One engine per job** — Each `.fio` section gets a fresh AuraIO engine so AIMD converges cleanly per workload with no stale state.
- **Pre-allocated callback pool** — Each thread owns an `io_ctx_pool_t` (free-stack of `iodepth` slots). Zero `malloc`/`free` on the I/O hot path.
- **Per-thread atomics** — `thread_stats_t` uses `_Atomic` counters. No shared lock for stats recording.
- **Cross-thread callback safety** — AuraIO callbacks can fire on any thread calling `aura_wait()`. All state is passed via the `user_data` pointer, not thread-local storage.

## File Reference

| File | Lines | Purpose |
|------|-------|---------|
| `main.c` | ~265 | CLI parsing, help text, engine lifecycle |
| `job_parser.h` | ~105 | `job_config_t`, `bench_config_t`, parse declarations |
| `job_parser.c` | ~740 | .fio INI parser + CLI arg parser |
| `workload.h` | ~100 | `io_ctx_t`, `io_ctx_pool_t`, `thread_ctx_t`, `file_set_t` |
| `workload.c` | ~930 | I/O loop, callbacks, worker threads, file management |
| `stats.h` | ~140 | `thread_stats_t`, `direction_result_t`, `job_result_t` |
| `stats.c` | ~460 | Histogram, percentiles, BW/IOPS sampling, aggregation |
| `output.h` | ~40 | `output_normal()`, `output_json()` declarations |
| `output.c` | ~670 | FIO-compatible text + JSON formatters |
| `test_BFFIO.sh` | ~300 | Functional test suite |
| `run_baseline.sh` | ~550 | FIO comparison with delta report |

---

Licensed under the Apache License, Version 2.0. See [LICENSE](../LICENSE) for details.
