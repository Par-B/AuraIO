# AuraIO

Self-tuning async I/O library for Linux using io_uring with AIMD congestion control.

## Build Environment

- **Linux**: Run commands directly
- **macOS**: Run commands in the Linux container: `orb -m linux bash -c "<command>"`

## Quick Reference

```bash
make           # Build libraries
make test      # Run all tests
make clean     # Clean build
make examples  # Build examples
make install   # Install to PREFIX=/usr/local (supports DESTDIR)
```

**Dependencies**: liburing, pthreads, gcc (C11) / g++ (C++20 for bindings)

**Running individual tests** (on Linux or via `orb -m linux bash -c`):
```bash
cd tests && make test_ring && ./test_ring
```

**Rust bindings** (on Linux or via `orb -m linux bash -c`):
```bash
cd bindings/rust && cargo build          # Build bindings
cd bindings/rust && cargo test           # Run Rust tests
```

## Codebase Overview

**Stack**: C11 engine library + C++20 bindings + Rust bindings, io_uring via liburing

**Structure**:
- `engine/src/` - Engine C library (aura.c, adaptive_ring.c, adaptive_engine.c, adaptive_buffer.c)
- `engine/include/aura.h` - Public C API
- `engine/include/aura.hpp` - C++ bindings with RAII, coroutines, and concepts
- `bindings/rust/` - Rust bindings (aura-sys FFI + safe aura crate with async support)
- `integrations/` - External system integrations (Prometheus, OpenTelemetry, syslog) with C/C++/Rust examples
- `tools/BFFIO/` - FIO-compatible benchmark with AIMD auto-tuning
- `tools/auracp/` / `tools/auracp_cpp/` - File copy utilities (C and C++ versions)
- `tools/aura-hash/` - Parallel file checksum tool (sha256/sha1/md5)
- `tools/aura-check/` - Simple storage performance analyzer (8 workload patterns)
- `examples/` - C, C++, and Rust examples
- `tests/` - Unit tests, stress tests, benchmarks, and analysis scripts


## Code Style

- C11, 4-space indent, snake_case
- Prefixes: `aura_` (public), `ring_`/`adaptive_`/`buffer_` (internal)
- Public API in `engine/include/aura.h`, implementation in `engine/src/aura.c`
- Tuning constants at top of `engine/src/adaptive_engine.h`

## Development Workflow

- **CHANGELOG.md**: Update before committing major changes (new features, breaking changes, significant refactors). Follow [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format.
- **Version bumping**: Update `Makefile` (VERSION_*), `engine/include/aura.h` (AURA_VERSION_*), `bindings/rust/aura/Cargo.toml`, and `bindings/rust/aura-sys/Cargo.toml` together with CHANGELOG.md.

## Non-Obvious Patterns

- **Dual-lock strategy** in `adaptive_ring.c`: `lock` protects SQ/request pool, `cq_lock` protects CQ access. Completions are processed outside `cq_lock` to avoid holding it during callbacks.
- **macOS development**: All build/test commands must run inside `orb -m linux bash -c "..."` — native macOS builds are not supported (io_uring is Linux-only).

## Agent Preferences

- Prefer launching parallel subagents for independent tasks (code review, exploration, builds)
- Use Explore subagents for any codebase search spanning more than 2-3 files
- Use Plan subagents for non-trivial implementation design
- When reviewing code or investigating issues, fan out parallel subagents by area (e.g., C engine, C++ bindings, Rust bindings) rather than searching sequentially
