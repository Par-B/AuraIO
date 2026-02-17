# Changelog

All notable changes to AuraIO will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Breaking Changes
- **Registration API consolidated**: Removed `aura_unregister_buffers()`, `aura_unregister_files()`, `aura_request_unregister_buffers()`, `aura_request_unregister_files()`. Use `aura_unregister(engine, type)` and `aura_request_unregister(engine, type)` with `aura_reg_type_t` enum (`AURA_REG_BUFFERS`, `AURA_REG_FILES`)
- **`aura_get_stats()` returns `int`**: Was `void`. Now returns `0` on success, `-1` with `errno=EINVAL` on NULL/invalid input

### Added
- **auracp file copy tool** (`tools/auracp/`): Production-quality async pipelined file copy with cross-file I/O pipeline, recursive directory support, O_DIRECT mode, progress bar, and AIMD stats reporting
- **C++ lifecycle bindings**: `Engine::openat()`, `close()`, `statx()`, `fallocate()`, `ftruncate()`, `sync_file_range()` with callback and coroutine variants (`async_openat`, `async_close`, `async_statx`, `async_fallocate`, `async_ftruncate`, `async_sync_file_range`)
- **C++ accessor completions**: `Request::op_type()`, `Options::single_thread()`, `Stats::peak_in_flight()`, `RingStats::peak_in_flight()`, `aura::version()`, `aura::version_int()`
- **Diagnostic APIs**: `aura_get_fatal_error()` (check if engine has latched fatal error), `aura_in_callback_context()` (detect if current thread is inside a completion callback), `aura_histogram_percentile()` (compute latency percentiles from histogram data)
- **C++ Engine movability**: `aura::Engine` is now move-constructible and move-assignable (must not move while event loop methods are executing on another thread)
- **C++ wrappers**: `Engine::fatal_error()`, `Engine::in_callback_context()`, `Engine::unregister()`, `Engine::request_unregister()`, `Histogram::percentile()`
- **`[[nodiscard]]`** on `async_read`, `async_write`, `async_fsync`, `async_fdatasync` return types — prevents accidentally discarding awaitables

### Changed
- **Documentation overhaul**: Added THREADING MODEL section, CALLBACK SAFETY section (allowed/forbidden operations), per-errno documentation for submission functions, corrected request handle lifetime docs, improved `aura_drain()` and `aura_update_file()` partial-failure docs
- C++ `Engine::get_stats()` now throws `Error` on failure instead of silently succeeding on invalid input
- Rust `Engine::stats()` now returns `Result<Stats>` instead of `Stats`

## [0.4.0] - 2026-02-15

### Breaking Changes
- **`aura_buffer_free()`** no longer requires a `size` parameter. The pool now tracks buffer sizes internally via a side hash table. Old: `aura_buffer_free(engine, buf, size)` → New: `aura_buffer_free(engine, buf)`
- **`aura_get_stats()`** and **`aura_get_ring_stats()`** now accept a `size_t struct_size` parameter for forward-compatible struct versioning. Pass `sizeof(aura_stats_t)` / `sizeof(aura_ring_stats_t)` as the last argument
- **`aura_fsync()`** flags parameter changed from `aura_fsync_flags_t` enum to `unsigned int`. Use `AURA_FSYNC_DEFAULT` (0) or `AURA_FSYNC_DATASYNC` (1)

### Added
- `AURA_*` flag constants for all operations: `AURA_FSYNC_*`, `AURA_FALLOC_*`, `AURA_SYNC_RANGE_*`, `AURA_STATX_*`, `AURA_O_*` — provides a validated whitelist instead of exposing raw kernel flags
- `aura_request_op_type()` introspection function — returns the operation type of a request, enabling generic completion handlers to dispatch by op type
- `aura_op_type_t` enum exposed in public header with stable integer values and reserved slots for future ops
- Internal `buf_size_map_t` hash table for O(1) buffer size tracking (open addressing, Fibonacci hashing, striped locks)

### Changed
- `linux/stat.h` include guarded with `#ifdef __linux__` for better header portability
- `aura_statx()` declaration guarded with `#ifdef __linux__`
- Documentation: added "When to use which" guidance for `aura_poll()` vs `aura_wait()` vs `aura_drain()`

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
