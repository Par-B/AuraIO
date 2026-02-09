# BFFIO

FIO-compatible I/O benchmark powered by AuraIO.

BFFIO ("Better Faster FIO") replaces FIO's static queue depth with AuraIO's
self-tuning AIMD concurrency controller. It speaks the same CLI options and
output formats as FIO, so existing scripts and dashboards work out of the box.

## Quick Start

```bash
# Build (automatically builds libauraio if needed)
make

# Random read benchmark — AuraIO auto-tunes concurrency
../bin/BFFIO --rw=randread --bs=4K --size=1G --runtime=30 --time_based --direct=1

# Find max throughput under a 2ms P99 latency ceiling
../bin/BFFIO --rw=randread --bs=4K --size=1G --runtime=60 --time_based --direct=1 --target-p99=2ms
```

## What Makes BFFIO Different

- **Self-tuning concurrency** -- AuraIO's AIMD controller adjusts queue depth
  in real time based on completion latency feedback. No manual `--iodepth`
  guessing; set a cap and AuraIO finds the right depth.

- **Latency-targeted benchmarking** (`--target-p99`) -- Specify a P99 latency
  ceiling and BFFIO finds the maximum sustainable throughput under it. AIMD
  probes upward (additive increase) and backs off (multiplicative decrease)
  automatically.

- **Adaptive ring selection** (`--ring-select`) -- Three io_uring routing
  strategies: `adaptive` (default, routes I/O based on load), `cpu_local`
  (pin to per-CPU rings), `round_robin` (distribute evenly).

- **Zero-allocation hot path** -- Pre-allocated I/O context pools eliminate
  malloc/free from the I/O submission and completion path.

- **FIO-compatible output** -- Text and JSON output matches FIO 3.36 format.
  Existing dashboards and `fio-plot` work unchanged. An additional "AuraIO"
  section reports adaptive tuning state (current depth, AIMD events, ring
  utilization).

## Performance Optimizations

- **Pre-allocated I/O context pool** -- `io_ctx_pool_t` backed by a
  spinlock-protected free stack. O(1) get/put, zero heap allocation on the
  hot path.

- **Adaptive latency sampling** -- Budget-based recalibration every ~65K I/Os
  with xorshift64 PRNG-based selection. Caps timestamp overhead at ~0.5% CPU
  regardless of IOPS rate. At low IOPS every I/O is sampled; at high IOPS
  sampling is statistical.

- **Full-depth initial submission** -- In benchmark mode, the pipeline is
  filled to `engine_depth` immediately instead of ramping up through AIMD.
  In target-p99 mode, submission starts at 4 in-flight and lets AIMD probe
  upward.

- **Per-thread atomic stats** -- Counters use `memory_order_relaxed` atomics.
  No false sharing, no locks on the stats hot path. IOPS and bandwidth
  counters are always updated; only the latency path is sampled.

- **Latency histogram** -- 200 buckets at 50us resolution for sub-millisecond
  percentile accuracy up to 10ms, plus an overflow bucket for outliers.

## Architecture

| File | Description |
|------|-------------|
| `main.c` | CLI entry point, engine lifecycle, job orchestration |
| `job_parser.c` | FIO-compatible config parser (CLI args + `.fio` job files) |
| `workload.c` | Worker threads, I/O submission/completion, adaptive sampling |
| `stats.c` | Per-thread atomic counters, latency histograms, result aggregation |
| `output.c` | FIO 3.36-compatible text and JSON output formatters |

One AuraIO engine is created per job for clean AIMD convergence. The engine
depth is set to `iodepth * 2` (minimum 256) so the AIMD controller has
headroom to probe above the user-specified cap.

## Building

Linux only (requires io_uring).

| Target | Description |
|--------|-------------|
| `make` | Build BFFIO binary to `../bin/BFFIO` |
| `make test` | Run `test_BFFIO.sh` test suite |
| `make baseline` | Run FIO vs BFFIO comparison (standard) |
| `make baseline-quick` | Quick FIO vs BFFIO comparison |
| `make clean` | Clean build artifacts |

**Dependencies**: libauraio.a (parent library), liburing, pthreads, libm.
The Makefile builds libauraio automatically if it is not already present.

## FIO Compatibility

BFFIO supports the most commonly used FIO options for io_uring workloads:
job names, I/O patterns, block sizes, file/device targets, O_DIRECT,
time-based and size-based runs, ramp time, multiple worker threads, mixed
read/write ratios, fsync intervals, and group reporting. The `--ioengine`
flag is accepted for compatibility but always uses AuraIO internally.
See [USAGE.md](USAGE.md) for the full compatibility matrix and option
reference.
