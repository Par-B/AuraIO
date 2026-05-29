# aura-sys

Raw FFI bindings for [AuraIO](https://github.com/Par-B/AuraIO) — a self-tuning
async I/O library for Linux built on `io_uring` with AIMD congestion control.

This is a `*-sys` crate: it exposes the unsafe C ABI of the AuraIO engine
(`libaura`) and performs no abstraction. **Most users should depend on the safe
[`aura`](https://crates.io/crates/aura) crate instead**, which wraps these
bindings with RAII, lifetimes, and idiomatic error handling.

The bindings are generated at build time with [bindgen](https://crates.io/crates/bindgen),
and the engine library is located via `pkg-config`.

## Requirements

- **Linux 5.10+** (io_uring)
- **liburing** (`apt install liburing-dev`) — any 2.x release; liburing 2.7+
  additionally enables `aura_ftruncate`, which otherwise returns `ENOSYS`
- The AuraIO C engine (`libaura`) discoverable via `pkg-config` (`libaura.pc`)
- `clang`/`libclang` for bindgen at build time

## License

Licensed under the Apache License, Version 2.0. See
[LICENSE](https://github.com/Par-B/AuraIO/blob/main/LICENSE) for details.
