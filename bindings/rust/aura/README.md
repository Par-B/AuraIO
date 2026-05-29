# aura

Safe Rust bindings for [AuraIO](https://github.com/Par-B/AuraIO) — a self-tuning
async I/O library for Linux built on `io_uring` with AIMD congestion control.

This crate provides a safe, idiomatic wrapper around the AuraIO C engine. For the
raw FFI layer, see the [`aura-sys`](https://crates.io/crates/aura-sys) crate.

## Requirements

- **Linux 5.10+** (io_uring)
- **liburing** (`apt install liburing-dev`) — any 2.x release; liburing 2.7+
  additionally enables `Engine::ftruncate`, which otherwise returns `ENOSYS`
- The AuraIO C engine (`libaura`) available at build/link time

## Quick start

```rust
use aura::{Engine, Result};
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

fn main() -> Result<()> {
    let engine = Engine::new()?;
    let mut buf = engine.allocate_buffer(4096)?;

    let file = File::open("/etc/hostname").unwrap();
    let fd = file.as_raw_fd();

    let done = Arc::new(AtomicBool::new(false));
    let done_cb = done.clone();

    // SAFETY: `buf` and `file` outlive the in-flight operation below.
    unsafe {
        engine.read(fd, (&buf).into(), 4096, 0, 0, move |result| {
            match result {
                Ok(n) => println!("read {n} bytes"),
                Err(e) => eprintln!("read failed: {e}"),
            }
            done_cb.store(true, Ordering::SeqCst);
        })?;
    }

    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }
    Ok(())
}
```

## Features

- `async` — enables runtime-agnostic `async`/`await` support (`engine.async_read(..).await`, etc.)

## License

Licensed under the Apache License, Version 2.0. See
[LICENSE](https://github.com/Par-B/AuraIO/blob/main/LICENSE) for details.
