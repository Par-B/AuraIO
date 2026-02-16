# Changelog

All notable changes to AuraIO will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.0] - 2026-02-12

### Added
- Lifecycle metadata operations: `aura_openat()`, `aura_close()`, `aura_statx()`, `aura_fallocate()`, `aura_ftruncate()`, `aura_sync_file_range()`
- Metadata ops skip AIMD latency sampling (optimized for data I/O path)
- Full lifecycle test suite (`tests/test_metadata_ops.c`)

### Changed
- `aura_request_t` internal struct extended with `meta` union for operation-specific parameters (no public API change)

## [0.2.1] - 2026-02-12

### Added
- Public `aura_log_emit()` API and log level constants (`AURA_LOG_ERR` through `AURA_LOG_DEBUG`)
- C++ log bindings (`engine/include/auraio/log.hpp`) - `auraio::set_log_handler()`, `auraio::log_emit()`, `LogLevel` enum
- Rust log bindings (`bindings/rust/auraio/src/log.rs`) - `set_log_handler()`, `log_emit()`, `LogLevel` enum with 10 unit tests
- Log handler examples in all three languages:
  - C example (`examples/C/log_handler.c`) with custom stderr logging, timestamps, and severity filtering
  - C++ example (`examples/cpp/log_handler.cpp`) with lambda handler and `std::chrono` timestamps
  - Rust example (`examples/rust/examples/log_handler.rs`) with closure handler and `SystemTime` timestamps
- Syslog integration (`integrations/syslog/C/`) forwarding AuraIO logs to syslog(3)
- Unit tests for `aura_log_emit()` covering all levels, formatting, truncation, and no-handler case

### Fixed
- C `log_handler.c` example: renamed confusing `min_level` field to `max_level` for clarity
- C `log_handler.c`: I/O errors now routed through `aura_log_emit()` instead of raw `fprintf`
- C++ `log_handler.cpp`: fixed fd leak in exception handlers by adding `close(fd)` to catch blocks
- C++ `log_handler.cpp`: added `catch (const std::exception &e)` for comprehensive exception handling
- C++ `log_handler.cpp`: removed unused `#include <cstring>`
- Rust `log_handler.rs`: I/O errors now routed through `log_emit()` for consistency

## [0.2.0] - 2025-02-08

### Changed
- **Directory structure reorganization** for improved clarity and scalability
  - Moved C library and C++ bindings to `engine/` directory
    - `engine/src/` - C implementation
    - `engine/include/` - C and C++ public headers
    - `engine/lib/` - Compiled libraries
    - `engine/pkg/` - Packaging templates
  - Renamed `exporters/` to `integrations/` to better reflect general-purpose integration support
    - `integrations/prometheus/C/` - Prometheus exporter (C implementation)
    - `integrations/opentelemetry/C/` - OpenTelemetry exporter (C implementation)
    - Added placeholder directories for C++ and Rust variants
  - Relocated coverage reports to `tests/coverage/` to keep project root clean
  - Updated all build systems, Makefiles, and scripts to use new paths
  - Library naming updated: `libauraio.so.0.2.0` (from `libauraio.so.0.1.0`)

### Fixed
- Fixed TSAN/ASAN library paths in root Makefile
- Updated sanitizer build rules to use `engine/` structure

### Notes
- **No API changes** - existing code continues to work without modification
- Build system paths updated - source builds need updated include/lib paths
- All 369 tests passing (240 C + 44 C++ + 85 Rust)

## [0.1.0] - 2025-02-05

### Added
- AIMD adaptive controller with +1 additive increase, x0.80 multiplicative decrease
- Per-core io_uring rings with CPU-aware routing
- Scalable buffer pool with thread-local caching and auto-scaling shards
- C11 API with async read/write/fsync, vectored I/O, registered buffers/files
- C++20 bindings with RAII, lambda callbacks, coroutine support (`co_await`)
- Rust bindings with async/await support (feature flag)
- Event loop integration via pollable fd
- Comprehensive benchmark suite with FIO comparison
- Symbol visibility control (`AURA_API` / `-fvisibility=hidden`)
- SO versioning (`libauraio.so.0.1.0`)

### Performance
- ~24K IOPS / ~1500 MB/s throughput (64K random reads, i7-1280P)
- ~105us avg latency, ~300us P99 (4K serial reads)
- ~7% latency overhead vs FIO
- ~1.66x throughput advantage over FIO for 64K reads (adaptive batching)
- ~6.3M ops/sec buffer pool (4 threads)
