# aura-hash — Parallel File Checksum Tool

## NAME

aura-hash — compute file checksums using pipelined async I/O

## SYNOPSIS

```
aura-hash [OPTIONS] FILE...
```

## DESCRIPTION

**aura-hash** computes SHA-256, SHA-1, or MD5 checksums of one or more files. It is a drop-in replacement for **sha256sum**(1), **sha1sum**(1), and **md5sum**(1) that uses AuraIO's pipelined async I/O to overlap disk reads with hash computation.

When multiple files are present, aura-hash spawns one worker thread per CPU core. Each worker gets its own AuraIO engine (one io_uring ring) and is pinned to a dedicated core via `pthread_setaffinity_np`. Files are partitioned round-robin across workers, so all cores stay busy.

Output matches the coreutils format:

```
<hex_digest>  <filename>
```

Two spaces separate the digest from the filename.

## OPTIONS

| Option | Description |
|--------|-------------|
| `-a`, `--algorithm ALG` | Hash algorithm: `sha256` (default), `sha1`, `md5` |
| `-r`, `--recursive` | Hash directories recursively (follows symlinks) |
| `-d`, `--direct` | Use O_DIRECT to bypass the page cache |
| `-b`, `--block-size N` | Read chunk size. Default: `256K`. Suffixes: `K`, `M`, `G` |
| `-p`, `--pipeline N` | In-flight read slots per worker. Default: `8`, max: `64` |
| `-q`, `--quiet` | Suppress progress output on stderr |
| `-v`, `--verbose` | Print AuraIO engine stats after hashing |
| `-h`, `--help` | Show usage help |

## EXAMPLES

Hash a single file (SHA-256):

```bash
aura-hash /path/to/file
```

Hash with MD5:

```bash
aura-hash -a md5 /path/to/file
```

Hash a directory tree recursively:

```bash
aura-hash -r /etc/ssl/certs/
```

Verify against coreutils:

```bash
diff <(aura-hash file1 file2 | sort) <(sha256sum file1 file2 | sort)
```

Large file with O_DIRECT and verbose stats:

```bash
aura-hash -d -v /dev/sda
```

Tune for NVMe (large chunks, deep pipeline):

```bash
aura-hash -b 1M -p 32 -r /data/
```

## EXIT STATUS

| Code | Meaning |
|------|---------|
| `0` | All files hashed successfully |
| `1` | Error (I/O failure, bad arguments, etc.) |
| `130` | Interrupted by SIGINT |

## BUILDING

```bash
# From AuraIO root
make aura-hash

# Or from the tool directory
cd tools/aura-hash
make
```

**Dependencies**: liburing, libcrypto (OpenSSL), pthreads

The binary is placed at `tools/bin/aura-hash`.

---

## Implementation

### Architecture

aura-hash uses a multi-threaded, event-driven architecture:

```
main thread
  ├── build file task list (nftw for -r)
  ├── partition files round-robin across N workers
  ├── spawn N worker threads
  ├── poll progress (atomic counters) and update stderr
  └── join workers, report stats

worker thread (x N, pinned to core N)
  ├── create AuraIO engine (single_thread=true, 1 ring)
  ├── allocate pipeline buffer slots
  ├── submit pipelined reads across assigned files
  ├── on completion: buffer → drain in order → hash update
  └── finalize: EVP_DigestFinal → print digest under mutex
```

The worker count equals `min(num_cpus, num_files)`, capped at 64. A single file always runs single-threaded since SHA-256/SHA-1/MD5 are inherently sequential per stream.

### Key Data Structures

**`file_task_t`** — Per-file state. Owns the file descriptor, read/hash offset tracking, and an `EVP_MD_CTX` for incremental hashing. Tracks `active_ops` to know when all in-flight reads for this file have landed.

**`buf_slot_t`** — Pipeline buffer slot. Each slot holds an AuraIO-allocated buffer and cycles through states: `FREE → READING → DONE → FREE`. The `DONE` state is critical for ordering — a slot stays `DONE` until all prior offsets for its file have been hashed.

**`hash_ctx_t`** — Per-worker context. Owns the AuraIO engine, pipeline slots, and file queue for this worker.

**`worker_ctx_t`** — Thread context. Holds the worker ID (used for CPU pinning) and its partition of the file queue.

### Completion Ordering

io_uring does not guarantee that completions arrive in submission order. With multiple reads in flight for the same file, chunk N+1 may complete before chunk N. Since hash algorithms are sequential (each `EVP_DigestUpdate` depends on prior state), feeding chunks out of order produces wrong digests.

The solution uses a two-phase approach:

1. **Buffer**: When a read completes, the slot moves to `BUF_DONE` with its offset and byte count recorded. The slot is not freed yet.

2. **Drain**: `drain_completed()` scans all slots for the current file, looking for one whose offset matches `task->hash_offset` (the next byte the hash expects). If found, it feeds the data into `EVP_DigestUpdate`, advances `hash_offset`, frees the slot, and restarts the scan. This repeats until no matching slot is found.

This ensures hash updates happen strictly in file offset order regardless of io_uring completion order, while still allowing maximum read parallelism.

### Thread Safety

Each worker thread is fully independent — it has its own AuraIO engine, buffer slots, and file set. No shared mutable state exists between workers except:

- **`g_total_bytes_read`** (atomic) — Updated by each worker for progress reporting. The main thread reads this to render the progress bar.
- **`g_output_lock`** (mutex) — Guards stdout writes in `finalize_task()` so digest output lines don't interleave.
- **`g_interrupted`** (sig_atomic_t) — Set by SIGINT handler, read by all workers to initiate graceful shutdown.

### CPU Affinity

Each worker thread pins itself to core `worker_id` via `pthread_setaffinity_np` immediately after creation. This prevents the scheduler from migrating threads between cores, which would flush L1/L2 caches and hurt hash throughput. Since each worker's hot data (pipeline buffers, hash context) fits in L1/L2, pinning provides consistent performance.

### Pipeline Tuning

The `-p` flag controls how many reads are in flight *per worker*. With the default of 8 slots at 256K each, each worker has 2 MiB of read-ahead. For NVMe devices with deep queues, increasing to `-p 32 -b 1M` (32 MiB per worker) can saturate the device bandwidth.

The total system parallelism is: `num_workers × pipeline_depth` concurrent I/O operations, each on a separate io_uring ring with AuraIO's AIMD adaptive depth control.
