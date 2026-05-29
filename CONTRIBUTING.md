# Contributing to AuraIO

Thanks for your interest in contributing! AuraIO is a self-tuning async I/O
library for Linux built on `io_uring` with AIMD congestion control.

## Requirements

- Linux with a recent kernel (io_uring; some features need 5.6+, a few need
  6.x — see the README for the feature/version matrix).
- `liburing` (2.0+; 2.7+ unlocks `ftruncate` support), `pthreads`.
- `gcc`/`clang` with C11 for the engine, C++20 for the C++ bindings.
- A Rust toolchain (stable) for the Rust bindings.

io_uring is Linux-only. On macOS, run all build/test commands inside a Linux
container (see `AGENTS.md`).

## Building and Testing

```bash
make            # Build the libraries
make test       # Build and run the full test suite (C, C++, Rust)
make examples   # Build the examples
make clean      # Clean build artifacts
```

Run a single C test:

```bash
cd tests && make test_ring && ./test_ring
```

Rust bindings:

```bash
cd bindings/rust && cargo test            # default features
cd bindings/rust && cargo test --all-features
```

## Before Submitting a Pull Request

Please make sure your change passes the same checks CI runs:

1. **Full test suite** is green: `make test`.
2. **Sanitizers** are clean for changes to the engine or bindings:
   ```bash
   make test-sanitizers   # AddressSanitizer, ThreadSanitizer, UBSan
   ```
3. **Release build compiles** (the engine must build with `NDEBUG`):
   ```bash
   make CFLAGS_EXTRA=-DNDEBUG
   ```
4. **No new compiler warnings** under `-Wall -Wextra -Wpedantic`.
5. **Rust**: `cargo build --all-features` and `cargo test --all-features` pass,
   and `cargo clippy` introduces no new warnings.
6. Update **CHANGELOG.md** (under `[Unreleased]`) for any user-visible change,
   following the [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) format.

## Code Style

- **C**: C11, 4-space indentation, `snake_case`. Prefixes: `aura_` for public
  API, `ring_`/`adaptive_`/`buffer_` for internal symbols. Public API lives in
  `engine/include/aura.h`; the implementation lives in `engine/src/`.
- **C++**: C++20, RAII, in `engine/include/aura.hpp` and `engine/include/aura/`.
- Tuning constants live at the top of `engine/src/adaptive_engine.h`.
- Keep new code consistent with the surrounding style (naming, comment density,
  idioms).

## Versioning

AuraIO follows [Semantic Versioning](https://semver.org/). When bumping the
version, update all of: `Makefile` (`VERSION_*`), `engine/include/aura.h`
(`AURA_VERSION_*`), `cmake/CMakeLists.txt`, both `bindings/rust/*/Cargo.toml`,
and `CHANGELOG.md` together.

## Reporting Bugs and Security Issues

- Functional bugs: open a GitHub issue with a minimal reproducer, your kernel
  version, and your liburing version.
- Security vulnerabilities: please follow [SECURITY.md](SECURITY.md) — do not
  open a public issue.

## License

By contributing, you agree that your contributions will be licensed under the
[Apache License 2.0](LICENSE), consistent with the rest of the project.
