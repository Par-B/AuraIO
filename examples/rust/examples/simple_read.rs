//! Simple async file read example
//!
//! Demonstrates basic usage of the AuraIO library to read a file.
//!
//! Usage: cargo run --example simple_read -- <file>

use auraio::{Engine, Result};
use std::env;
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Arc;

const READ_SIZE: usize = 1024 * 1024; // 1MB

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: {} <file>", args[0]);
        eprintln!("\nReads the first 1MB of a file asynchronously.");
        std::process::exit(1);
    }

    let filename = &args[1];

    // Create the auraio engine
    println!("Creating async I/O engine...");
    let engine = Engine::new()?;

    // Open file
    // Note: O_DIRECT requires special handling in Rust; for simplicity we use regular open
    let file = File::open(filename).map_err(|e| {
        eprintln!("Failed to open '{}': {}", filename, e);
        std::process::exit(1);
    }).unwrap();
    let fd = file.as_raw_fd();

    // Allocate aligned buffer from engine's pool
    let buf = engine.allocate_buffer(READ_SIZE)?;

    // Track completion state
    let done = Arc::new(AtomicBool::new(false));
    let result_bytes = Arc::new(AtomicIsize::new(0));

    let done_clone = done.clone();
    let result_clone = result_bytes.clone();

    // We need to share the buffer with the callback
    // In real code, you'd use channels or other synchronization
    let buf_ptr = buf.as_ptr();

    // Submit async read
    println!("Submitting async read of {} bytes...", READ_SIZE);
    engine.read(fd, (&buf).into(), READ_SIZE, 0, move |result| {
        match result {
            Ok(n) => {
                println!("Read {} bytes successfully", n);
                result_clone.store(n as isize, Ordering::SeqCst);
            }
            Err(e) => {
                eprintln!("Read failed: {}", e);
                result_clone.store(-1, Ordering::SeqCst);
            }
        }
        done_clone.store(true, Ordering::SeqCst);
    })?;

    // Wait for completion
    println!("Waiting for completion...");
    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }

    // Show first few bytes of data
    let bytes_read = result_bytes.load(Ordering::SeqCst);
    if bytes_read > 0 {
        println!("\nFirst 64 bytes of file:");
        let slice = unsafe { std::slice::from_raw_parts(buf_ptr, bytes_read as usize) };
        for (i, &byte) in slice.iter().take(64).enumerate() {
            if i > 0 && i % 16 == 0 {
                println!();
            }
            if byte >= 32 && byte < 127 {
                print!("{}", byte as char);
            } else {
                print!("\\x{:02x}", byte);
            }
        }
        println!();
    }

    // Get statistics
    let stats = engine.stats();
    println!("\nEngine statistics:");
    println!("  Ops completed:     {}", stats.ops_completed());
    println!("  Bytes transferred: {}", stats.bytes_transferred());
    println!("  Throughput:        {:.2} MB/s", stats.throughput_bps() / (1024.0 * 1024.0));
    println!("  P99 latency:       {:.2} ms", stats.p99_latency_ms());
    println!("  Optimal in-flight: {}", stats.optimal_in_flight());
    println!("  Optimal batch:     {}", stats.optimal_batch_size());

    println!("\nDone!");
    Ok(())
}
