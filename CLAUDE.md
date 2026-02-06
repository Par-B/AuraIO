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
```

## Codebase Overview

**Stack**: C11 core library + C++20 bindings, io_uring via liburing

**Structure**:
- `src/` - Core C library (auraio.c, adaptive_ring.c, adaptive_engine.c, adaptive_buffer.c)
- `include/auraio.h` - Public C API
- `include/auraio.hpp` - C++ bindings with RAII, coroutines, and concepts
- `examples/` - C and C++ examples
- `tests/` - Unit tests and stress tests

For detailed architecture, see [docs/CODEBASE_MAP.md](docs/CODEBASE_MAP.md).

## Code Style

- C11, 4-space indent, snake_case
- Prefixes: `auraio_` (public), `ring_`/`adaptive_`/`buffer_` (internal)
- Public API in `include/auraio.h`, implementation in `src/auraio.c`
- Tuning constants at top of `src/adaptive_engine.h`
