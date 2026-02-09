# BFFIO Usage Guide

BFFIO (Better Faster FIO) is a FIO-compatible I/O benchmark powered by AuraIO's self-tuning async I/O engine using io_uring. It replaces manual queue depth and rate tuning with an AIMD congestion controller that automatically finds optimal concurrency for your storage device.

## Synopsis

```
BFFIO [options]
BFFIO <jobfile.fio>
BFFIO --help
```

## Options Reference

| Option | Default | Description |
|---|---|---|
| `--name=NAME` | `BFFIO` | Job name |
| `--rw=PATTERN` | *(required)* | I/O pattern: `read`, `write`, `randread`, `randwrite`, `readwrite`, `randrw` |
| `--bs=SIZE` | `4K` | Block size. Suffixes: K, M, G |
| `--size=SIZE` | *(required)* | File size. Suffixes: K, M, G |
| `--filename=PATH` | *(none)* | Existing file or block device |
| `--directory=PATH` | *(none)* | Directory for auto-created test files |
| `--direct=0\|1` | `0` | Use O_DIRECT |
| `--runtime=SECONDS` | *(none)* | Run duration in seconds |
| `--time_based` | off | Run for fixed time (requires `--runtime`) |
| `--ramp_time=SECONDS` | `0` | Warmup period (stats discarded) |
| `--numjobs=N` | `1` | Number of worker threads |
| `--iodepth=N` | `256` | Max queue depth cap (AuraIO auto-tunes below this) |
| `--rwmixread=N` | `50` | Read percentage for mixed workloads |
| `--group_reporting` | off | Aggregate stats across threads |
| `--fsync=N` | `0` | fsync every N writes |
| `--target-p99=LATENCY` | *(none)* | **BFFIO-exclusive.** P99 latency ceiling. Suffixes: us, ms, s (default unit: ms). AIMD finds max concurrency under this target. |
| `--ring-select=MODE` | `adaptive` | **BFFIO-exclusive.** Ring selection: `adaptive`, `cpu_local`, `round_robin` |
| `--ioengine=ENGINE` | `auraio` | Accepted for FIO command-line compatibility; always uses AuraIO |
| `--output-format=FMT` | `normal` | Output format: `normal`, `json` |

## BFFIO-Exclusive Options

### --target-p99

Sets a P99 latency ceiling. AuraIO's AIMD controller actively manages queue depth to keep P99 latency under the specified target.

```
--target-p99=2ms
--target-p99=500us
--target-p99=1.5ms
--target-p99=0.01s
```

Suffixes `us`, `ms`, and `s` are supported. If no suffix is given, the value is interpreted as milliseconds.

**Behavior:**

- The engine starts with `initial_in_flight=4` and probes upward via additive increase. When P99 latency exceeds the target, it backs off via multiplicative decrease.
- **Auto ramp_time**: If `--target-p99` is set and no explicit `--ramp_time` is given, BFFIO adds a 5-second warmup period to allow the AIMD controller to converge before stats collection begins.
- **Auto-scaling**: When `--target-p99` is set and `--numjobs=1`, BFFIO automatically scales `numjobs` to match the engine's ring count (one thread per ring) and enables `--group_reporting`. Each ring has its own AIMD controller, and the sum of per-ring depths equals total device concurrency.
- Output includes an AuraIO status line showing converged state:
  ```
  AuraIO: max concurrency=N at p99=X.XXms (target: Y.YYms), phase=NAME
  ```

### --ring-select

Controls how I/O operations are distributed across AuraIO's io_uring rings.

| Mode | Description |
|---|---|
| `adaptive` (default) | Routes I/O based on load and ring utilization |
| `cpu_local` | Pins I/O to per-CPU rings for NUMA-aware workloads |
| `round_robin` | Distributes I/O evenly across all rings |

### Benchmark mode (no --target-p99)

When `--target-p99` is not set, BFFIO runs in pure benchmark mode:

- The engine starts at full depth (`initial_in_flight = engine_depth`) to skip AIMD ramp-up.
- `engine_depth = max(iodepth * 2, 256)` so the AIMD controller has headroom.
- Output shows:
  ```
  AuraIO: converged to depth N, phase=NAME, p99=X.XXms, spills=N
  ```

## Examples

### Basic random read benchmark

```
BFFIO --rw=randread --bs=4k --size=1G --filename=/dev/nvme0n1 --direct=1 --runtime=30 --time_based
```

Random 4K reads against an NVMe device for 30 seconds with O_DIRECT. Runs at maximum depth; no latency constraint.

### Sequential write with periodic fsync

```
BFFIO --rw=write --bs=128k --size=4G --directory=/mnt/data --direct=1 --runtime=60 --time_based --fsync=16
```

Sequential 128K writes with an fsync every 16 writes. BFFIO creates a test file in `/mnt/data`.

### Latency-constrained workload

```
BFFIO --rw=randread --bs=4k --size=1G --filename=/dev/nvme0n1 --direct=1 --runtime=60 --time_based --target-p99=2ms
```

AIMD finds the maximum concurrency that keeps P99 read latency under 2ms. A 5-second warmup is added automatically for convergence. Thread count scales to match ring count.

### Mixed read/write with custom ratio

```
BFFIO --rw=randrw --bs=8k --size=2G --filename=/dev/sda --direct=1 --runtime=30 --time_based --rwmixread=70
```

70% random reads, 30% random writes at 8K block size.

### Using a job file

```
BFFIO workload.fio
```

Runs all jobs defined in `workload.fio`. See [Job File Format](#job-file-format) below.

### JSON output for scripting

```
BFFIO --rw=randread --bs=4k --size=1G --filename=/dev/nvme0n1 --direct=1 --runtime=30 --time_based --output-format=json
```

Produces FIO 3.36-compatible JSON output suitable for parsing with `jq` or other tools.

## Job File Format

BFFIO supports FIO's INI-style job file format. A `[global]` section sets defaults inherited by all subsequent `[jobname]` sections.

```ini
[global]
bs=64k
size=1G
direct=1
runtime=30
time_based

[randread-job]
rw=randread
iodepth=128

[randwrite-job]
rw=randwrite
iodepth=64
fsync=32
```

Each `[jobname]` section defines an independent job. Jobs run sequentially, each with a fresh AuraIO engine for clean AIMD convergence. The same key=value options available on the CLI are supported in job files.

## Output

### Normal (default)

FIO-style text summary with IOPS, bandwidth, and latency percentiles, plus bandwidth and IOPS sample statistics. An additional `AuraIO:` line reports the adaptive tuning state.

Example output:

```
randread-job: (g=0): rw=randread, bs=(R) 4096B, ioengine=AuraIO, direct=1
  read: IOPS=245k, BW=958MiB/s (1005MB/s)(28.1GiB/30000msec)
    lat (usec): min=12, max=4823, avg=52.31, stdev=38.72
    clat percentiles (usec):
     |  1.00th=[   18],  5.00th=[   22], 10.00th=[   25], 20.00th=[   30],
     | 30.00th=[   35], 40.00th=[   40], 50.00th=[   45], 60.00th=[   52],
     | 70.00th=[   60], 80.00th=[   70], 90.00th=[   88], 95.00th=[  112],
     | 99.00th=[  198], 99.50th=[  260], 99.90th=[  510], 99.95th=[  750],
     | 99.99th=[ 1880]
   bw (  KiB/s): min=920480, max=995200, per=100.00%, avg=981196.80, stdev=12340.50, samples=30
   iops        : min=230120, max=248800, avg=245299.20, stdev=3085.13, samples=30

  AuraIO: converged to depth 128, phase=steady, p99=0.20ms, spills=0
```

### JSON

FIO 3.36-compatible JSON structure. The top-level object contains a `jobs` array where each job has `read`, `write`, `trim`, and `sync` sections. Latency data is in `lat_ns` with a `percentile` object. An extra `AuraIO` object is included per job:

```json
{
  "AuraIO": {
    "final_depth": 128,
    "phase": "steady",
    "p99_ms": 0.198,
    "throughput_bps": 1005000000
  }
}
```

## FIO io_uring Compatibility

### Supported options

These FIO options work identically or with equivalent behavior:

| Category | Options |
|---|---|
| Workload | `rw` (all 6 patterns), `bs`, `size`, `rwmixread` |
| Files | `filename`, `directory` |
| I/O behavior | `direct`, `iodepth`, `fsync` |
| Timing | `runtime`, `time_based`, `ramp_time` |
| Threading | `numjobs`, `group_reporting` |
| Output | `output-format` (`normal`, `json`), `name` |
| Compatibility | `ioengine` (accepted, always AuraIO) |

### Not supported

FIO io_uring options that BFFIO does not implement:

| Category | FIO Options | Notes |
|---|---|---|
| Trim | `trim`, `randtrim`, `trimwrite` | Not implemented |
| Variable block sizes | `bsrange`, `bssplit` | Fixed block size only |
| Depth batching | `iodepth_batch`, `iodepth_batch_complete`, `iodepth_low` | AuraIO manages depth automatically |
| io_uring advanced | `sqthread_poll`, `hipri`, `fixedbufs`, `registerfiles`, `io_submit_mode` | AuraIO manages ring setup |
| Rate limiting | `rate`, `rate_iops`, `rate_min`, `rate_cycle` | Use `--target-p99` for latency-based control instead |
| Data verification | `verify`, `do_verify`, `verify_pattern` | Not implemented |
| Output logs | `write_bw_log`, `write_lat_log`, `write_iops_log`, `log_avg_msec` | Not implemented |
| Output formats | `json+`, `terse`, `minimal` | Only `normal` and `json` |
| Offset control | `offset`, `offset_increment`, `number_ios` | Not implemented |
| File layout | `nrfiles`, `filesize`, `file_service_type` | Single file per job |
| Zones | `zonesize`, `zonerange`, `zoneskip` | Not implemented |
| CPU affinity | `cpus_allowed`, `cpus_allowed_policy`, `numa_cpu_nodes` | Not implemented |
| Miscellaneous | `thinktime`, `ioscheduler`, `thread`, `loops`, `stonewall` | Not implemented |

BFFIO covers the core FIO io_uring workflow that the vast majority of benchmarks use: specify a pattern, block size, depth, file, and duration. Advanced FIO options like rate limiting, verification, and output logging are not needed because BFFIO's AIMD controller replaces manual depth/rate tuning, and its built-in adaptive sampling provides accurate latency histograms without separate log files.
