// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Demonstrates O_DIRECT vs buffered I/O with AuraIO
//!
//! Shows that AuraIO is agnostic to I/O mode - it works with both
//! O_DIRECT and buffered writes. The adaptive tuning measures actual
//! completion latency regardless of how the kernel handles the I/O.
//!
//! Usage: cargo run --example write_modes -- <file>

use aura::{Engine, Result};
use std::env;
use std::fs;
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Instant;

const WRITE_SIZE: usize = 64 * 1024; // 64KB per write
const NUM_WRITES: usize = 16; // Number of writes per test

/// Completion tracking
struct WriteState {
    completed: AtomicUsize,
    total: usize,
    start_time: Instant,
}

/// Run write test with specified mode.
fn run_write_test(filename: &str, use_direct: bool) -> Result<()> {
    let mode_name = if use_direct { "O_DIRECT" } else { "Buffered" };
    println!("\n=== Testing {} writes ===", mode_name);

    // Create engine
    let engine = Engine::new()?;

    // Open file with appropriate flags
    let mut flags = libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC;
    if use_direct {
        flags |= libc::O_DIRECT;
    }

    let fd: RawFd = unsafe {
        let c_path = std::ffi::CString::new(filename).unwrap();
        libc::open(c_path.as_ptr(), flags, 0o644)
    };

    if fd < 0 {
        let err = std::io::Error::last_os_error();
        if use_direct && err.raw_os_error() == Some(libc::EINVAL) {
            println!("O_DIRECT not supported on this filesystem, skipping");
            return Ok(());
        }
        eprintln!("Failed to open '{}': {}", filename, err);
        return Err(aura::Error::Io(err));
    }

    // Allocate buffer - use aligned for O_DIRECT
    let buf = if use_direct {
        println!("Using aligned buffer from engine.allocate_buffer()");
        let b = engine.allocate_buffer(WRITE_SIZE)?;
        // Fill with pattern
        // Note: we can't easily modify the buffer after allocation in this API
        b
    } else {
        println!("Using aligned buffer (Rust version always uses engine pool)");
        let b = engine.allocate_buffer(WRITE_SIZE)?;
        b
    };

    // Track completion
    let state = Arc::new(WriteState {
        completed: AtomicUsize::new(0),
        total: NUM_WRITES,
        start_time: Instant::now(),
    });

    // Submit all writes
    println!("Submitting {} async writes of {} bytes each...", NUM_WRITES, WRITE_SIZE);

    for i in 0..NUM_WRITES {
        let offset = (i * WRITE_SIZE) as i64;
        let state_clone = state.clone();

        unsafe {
            engine.write(fd, (&buf).into(), WRITE_SIZE, offset, move |result| {
                if let Err(e) = result {
                    eprintln!("Write failed: {}", e);
                }
                state_clone.completed.fetch_add(1, Ordering::SeqCst);
            })?;
        }
    }

    // Wait for all completions
    while state.completed.load(Ordering::SeqCst) < state.total {
        engine.wait(100)?;
    }

    // Calculate results
    let elapsed_sec = state.start_time.elapsed().as_secs_f64();
    let total_mb = (NUM_WRITES * WRITE_SIZE) as f64 / (1024.0 * 1024.0);
    let throughput = total_mb / elapsed_sec;

    println!("Completed {} writes in {:.3} seconds", state.total, elapsed_sec);
    println!("Total: {:.2} MB, Throughput: {:.2} MB/s", total_mb, throughput);

    // Get engine stats
    let stats = engine.stats()?;
    println!("P99 latency: {:.2} ms", stats.p99_latency_ms());
    println!("Optimal in-flight: {}", stats.optimal_in_flight());

    // Fsync to ensure data is on disk
    println!("Fsyncing...");
    unsafe { libc::fsync(fd); }

    // Cleanup
    unsafe { libc::close(fd); }

    Ok(())
}

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: {} <file>", args[0]);
        eprintln!("\nDemonstrates O_DIRECT vs buffered async writes.");
        eprintln!("The file will be created/overwritten.");
        std::process::exit(1);
    }

    let filename = &args[1];

    println!("AuraIO Write Modes Example (Rust)");
    println!("==================================");
    println!("File: {}", filename);
    println!("Write size: {} bytes", WRITE_SIZE);
    println!("Number of writes: {}", NUM_WRITES);

    // Test buffered I/O first
    run_write_test(filename, false)?;

    // Test O_DIRECT
    run_write_test(filename, true)?;

    // Cleanup test file
    fs::remove_file(filename).ok();

    println!("\n=== Summary ===");
    println!("Both modes use the same AuraIO API.");
    println!("The library is agnostic - it just submits to io_uring.");
    println!("O_DIRECT: Requires aligned buffers, bypasses page cache.");
    println!("Buffered: Any buffer works, uses page cache.");

    Ok(())
}
