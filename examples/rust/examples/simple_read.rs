//! Simple async file read example
//!
//! Demonstrates basic usage of the AuraIO library to read a file.
//!
//! Usage: cargo run --example simple_read -- <file>

use aura::{Engine, Result};
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

    // Create the aura engine
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
    unsafe {
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
    }

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
    let stats = engine.stats()?;
    println!("\nEngine statistics:");
    println!("  Ops completed:     {}", stats.ops_completed());
    println!("  Bytes transferred: {}", stats.bytes_transferred());
    println!("  Throughput:        {:.2} MB/s", stats.throughput_bps() / (1024.0 * 1024.0));
    println!("  P99 latency:       {:.2} ms", stats.p99_latency_ms());
    println!("  Optimal in-flight: {}", stats.optimal_in_flight());
    println!("  Optimal batch:     {}", stats.optimal_batch_size());

    // Per-ring statistics (demonstrates ring_count() and ring_stats())
    let rings = engine.ring_count();
    for i in 0..rings {
        if let Ok(ring_stats) = engine.ring_stats(i) {
            println!(
                "  Ring {}: phase={} depth={}/{}",
                i,
                ring_stats.aimd_phase_name(),
                ring_stats.pending_count(),
                ring_stats.in_flight_limit()
            );
        }
    }

    // Latency histogram (demonstrates histogram())
    if rings > 0 {
        if let Ok(hist) = engine.histogram(0) {
            if hist.total_count() > 0 {
                println!("\nLatency Histogram (Ring 0):");
                println!("  Total samples: {}", hist.total_count());
                println!("  Bucket width:  {} μs", hist.bucket_width_us());
                println!("  Max tracked:   {} μs", hist.max_tracked_us());

                // Calculate percentiles from histogram
                let mut cumulative: u32 = 0;
                let p50_threshold = hist.total_count() / 2;
                let p90_threshold = (hist.total_count() * 90) / 100;
                let p99_threshold = (hist.total_count() * 99) / 100;
                let p999_threshold = (hist.total_count() * 999) / 1000;
                let mut p50 = -1i32;
                let mut p90 = -1i32;
                let mut p99 = -1i32;
                let mut p999 = -1i32;

                for i in 0..aura::Histogram::BUCKET_COUNT {
                    cumulative += hist.bucket(i);
                    if p50 == -1 && cumulative >= p50_threshold {
                        p50 = i as i32;
                    }
                    if p90 == -1 && cumulative >= p90_threshold {
                        p90 = i as i32;
                    }
                    if p99 == -1 && cumulative >= p99_threshold {
                        p99 = i as i32;
                    }
                    if p999 == -1 && cumulative >= p999_threshold {
                        p999 = i as i32;
                    }
                }

                if p50 >= 0 {
                    println!(
                        "  P50 latency:   {:.2} ms",
                        (p50 * hist.bucket_width_us()) as f64 / 1000.0
                    );
                }
                if p90 >= 0 {
                    println!(
                        "  P90 latency:   {:.2} ms",
                        (p90 * hist.bucket_width_us()) as f64 / 1000.0
                    );
                }
                if p99 >= 0 {
                    println!(
                        "  P99 latency:   {:.2} ms",
                        (p99 * hist.bucket_width_us()) as f64 / 1000.0
                    );
                }
                if p999 >= 0 {
                    println!(
                        "  P99.9 latency: {:.2} ms",
                        (p999 * hist.bucket_width_us()) as f64 / 1000.0
                    );
                }
                if hist.overflow() > 0 {
                    println!(
                        "  Overflow:      {} samples (> {} μs)",
                        hist.overflow(),
                        hist.max_tracked_us()
                    );
                }
            }
        }
    }

    // Buffer pool statistics (demonstrates buffer_stats())
    let buf_stats = engine.buffer_stats()?;
    println!("\nBuffer Pool Statistics:");
    println!("  Total allocated:  {} bytes", buf_stats.total_allocated_bytes());
    println!("  Buffer count:     {}", buf_stats.total_buffers());
    println!("  Shard count:      {}", buf_stats.shard_count());

    println!("\nDone!");
    Ok(())
}
