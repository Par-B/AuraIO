// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Minimal working example of AuraIO async read
//!
//! This example demonstrates the basic usage pattern:
//! 1. Create an engine
//! 2. Allocate a buffer
//! 3. Submit an async read with a callback
//! 4. Wait for completion
//!
//! Run: cargo run --example quickstart

use aura::{Engine, Result};
use std::fs::{self, File};
use std::io::Write;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

const BUF_SIZE: usize = 4096;

fn main() -> Result<()> {
    let test_file = "/tmp/aura_rust_quickstart.tmp";
    let test_data = b"Hello from AuraIO Rust bindings! This is async I/O.\n";

    // Create a test file with known content
    {
        let mut file = File::create(test_file).expect("Failed to create test file");
        file.write_all(test_data)
            .expect("Failed to write test data");
    }

    // Create AuraIO engine
    let engine = Engine::new()?;

    // Allocate aligned buffer from engine's pool
    let mut buf = engine.allocate_buffer(BUF_SIZE)?;

    // Zero the buffer
    buf.as_mut_slice().fill(0);

    // Open file for reading
    let file = File::open(test_file).expect("Failed to open test file");
    let fd = file.as_raw_fd();

    // Track completion state
    let done = Arc::new(AtomicBool::new(false));
    let done_clone = done.clone();

    // Capture result in callback
    let result_bytes = Arc::new(std::sync::atomic::AtomicIsize::new(0));
    let result_clone = result_bytes.clone();

    // Submit async read
    println!("Submitting async read...");
    unsafe {
        engine.read(fd, (&buf).into(), BUF_SIZE, 0, move |result| {
            match result {
                Ok(n) => {
                    println!("Read completed: {} bytes", n);
                    result_clone.store(n as isize, Ordering::SeqCst);
                }
                Err(e) => {
                    eprintln!("Read failed: {}", e);
                    result_clone.store(-1, Ordering::SeqCst);
                }
            }
            done_clone.store(true, Ordering::SeqCst);
        })?;
    }

    // Wait for completion
    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }

    // Verify result
    let bytes_read = result_bytes.load(Ordering::SeqCst);
    if bytes_read > 0 {
        let content = std::str::from_utf8(&buf.as_slice()[..bytes_read as usize])
            .unwrap_or("<invalid utf-8>");
        println!("Data read: {}", content);
    }

    // Cleanup test file
    fs::remove_file(test_file).ok();

    println!("Success!");
    Ok(())
}
