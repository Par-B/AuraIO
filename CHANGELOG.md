# Changelog

All notable changes to AuraIO will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.1] - 2025-02-05

### Added
- AIMD adaptive controller with +1 additive increase, x0.80 multiplicative decrease
- Per-core io_uring rings with CPU-aware routing
- Scalable buffer pool with thread-local caching and auto-scaling shards
- C11 API with async read/write/fsync, vectored I/O, registered buffers/files
- C++20 bindings with RAII, lambda callbacks, coroutine support (`co_await`)
- Rust bindings with async/await support (feature flag)
- Event loop integration via pollable fd
- Comprehensive benchmark suite with FIO comparison
- Symbol visibility control (`AURAIO_API` / `-fvisibility=hidden`)
- SO versioning (`libauraio.so.1.0.1`)

### Performance
- ~24K IOPS / ~1500 MB/s throughput (64K random reads, i7-1280P)
- ~105us avg latency, ~300us P99 (4K serial reads)
- ~7% latency overhead vs FIO
- ~1.66x throughput advantage over FIO for 64K reads (adaptive batching)
- ~6.3M ops/sec buffer pool (4 threads)

## [1.0.0] - 2025-01-15

Initial release.

### Added
- Core io_uring engine with adaptive queue depth tuning
- C and C++ APIs
- Basic buffer pool
- Unit tests and examples
