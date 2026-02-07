# AuraIO

Self-tuning async I/O library for Linux using io_uring with AIMD congestion control.

## Build Environment

- **Linux**: Run commands directly
- **macOS**: Run commands in the Linux container: `orb -m Caliente-dev bash -c "<command>"`

## Quick Reference

```bash
make           # Build libraries
make test      # Run all tests
make clean     # Clean build
make examples  # Build examples
make install   # Install to PREFIX=/usr/local (supports DESTDIR)
```

**Dependencies**: liburing, pthreads, gcc (C11) / g++ (C++20 for bindings)

**Running individual tests** (on Linux or via `orb -m Caliente-dev bash -c`):
```bash
cd tests && make test_ring && ./test_ring
```

## Codebase Overview

**Stack**: C11 core library + C++20 bindings + Rust bindings, io_uring via liburing

**Structure**:
- `src/` - Core C library (auraio.c, adaptive_ring.c, adaptive_engine.c, adaptive_buffer.c)
- `include/auraio.h` - Public C API
- `include/auraio.hpp` - C++ bindings with RAII, coroutines, and concepts
- `bindings/rust/` - Rust bindings (auraio-sys FFI + safe auraio crate with async support)
- `exporters/prometheus/` - Prometheus metrics exporter
- `tools/BFFIO/` - FIO-compatible benchmark with AIMD auto-tuning
- `examples/` - C, C++, and Rust examples
- `tests/` - Unit tests, stress tests, benchmarks, and analysis scripts

For detailed architecture, see [docs/CODEBASE_MAP.md](docs/CODEBASE_MAP.md).

## Code Style

- C11, 4-space indent, snake_case
- Prefixes: `auraio_` (public), `ring_`/`adaptive_`/`buffer_` (internal)
- Public API in `include/auraio.h`, implementation in `src/auraio.c`
- Tuning constants at top of `src/adaptive_engine.h`

## Non-Obvious Patterns

- **Dual-lock strategy** in `adaptive_ring.c`: `lock` protects SQ/request pool, `cq_lock` protects CQ access. Completions are processed outside `cq_lock` to avoid holding it during callbacks.
- **macOS development**: All build/test commands must run inside `orb -m Caliente-dev bash -c "..."` — native macOS builds are not supported (io_uring is Linux-only).

## Agent Preferences

- Prefer launching parallel subagents for independent tasks (code review, exploration, builds)
- Use Explore subagents for any codebase search spanning more than 2-3 files
- Use Plan subagents for non-trivial implementation design
- When reviewing code or investigating issues, fan out parallel subagents by area (e.g., C core, C++ bindings, Rust bindings) rather than searching sequentially
