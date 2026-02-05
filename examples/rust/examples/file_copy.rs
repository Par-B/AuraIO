//! Async file copy example
//!
//! Demonstrates copying a file using AuraIO's async I/O.
//! This is a simplified version that uses synchronous-style waiting
//! to avoid Send trait complexities with raw pointers.
//!
//! Usage: cargo run --example file_copy -- <source> <destination>

use auraio::{Engine, Result};
use std::env;
use std::fs::{self, File};
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Arc;
use std::time::Instant;

const CHUNK_SIZE: usize = 256 * 1024; // 256KB chunks

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("Usage: {} <source> <destination>", args[0]);
        eprintln!("\nCopies a file using async I/O.");
        std::process::exit(1);
    }

    let src_path = &args[1];
    let dst_path = &args[2];

    println!("AuraIO File Copy (Rust)");
    println!("=======================");
    println!("Source:      {}", src_path);
    println!("Destination: {}", dst_path);

    // Open source file
    let src_file = File::open(src_path).map_err(|e| {
        eprintln!("Failed to open source '{}': {}", src_path, e);
        std::process::exit(1);
    }).unwrap();
    let src_fd = src_file.as_raw_fd();

    // Get source file size
    let metadata = fs::metadata(src_path).map_err(|e| {
        eprintln!("Failed to stat '{}': {}", src_path, e);
        std::process::exit(1);
    }).unwrap();
    let file_size = metadata.len() as usize;

    println!("File size:   {} bytes ({:.2} MB)",
             file_size, file_size as f64 / (1024.0 * 1024.0));

    // Create destination file
    let dst_file = File::create(dst_path).map_err(|e| {
        eprintln!("Failed to create destination '{}': {}", dst_path, e);
        std::process::exit(1);
    }).unwrap();
    let dst_fd = dst_file.as_raw_fd();

    // Create engine
    let engine = Engine::new()?;

    // Allocate buffer
    let buf = engine.allocate_buffer(CHUNK_SIZE)?;

    let start_time = Instant::now();
    println!("\nCopying...");

    let mut offset: usize = 0;
    let mut total_copied: usize = 0;

    while offset < file_size {
        let chunk = std::cmp::min(CHUNK_SIZE, file_size - offset);

        // Track completion
        let done = Arc::new(AtomicBool::new(false));
        let result_bytes = Arc::new(AtomicIsize::new(0));

        let done_clone = done.clone();
        let result_clone = result_bytes.clone();

        // Submit async read
        engine.read(src_fd, (&buf).into(), chunk, offset as i64, move |result| {
            match result {
                Ok(n) => result_clone.store(n as isize, Ordering::SeqCst),
                Err(_) => result_clone.store(-1, Ordering::SeqCst),
            }
            done_clone.store(true, Ordering::SeqCst);
        })?;

        // Wait for read to complete
        while !done.load(Ordering::SeqCst) {
            engine.wait(100)?;
        }

        let bytes_read = result_bytes.load(Ordering::SeqCst);
        if bytes_read <= 0 {
            if bytes_read < 0 {
                eprintln!("\nRead failed at offset {}", offset);
                std::process::exit(1);
            }
            break; // EOF
        }

        // Track completion for write
        let done = Arc::new(AtomicBool::new(false));
        let result_bytes = Arc::new(AtomicIsize::new(0));

        let done_clone = done.clone();
        let result_clone = result_bytes.clone();

        // Submit async write
        engine.write(dst_fd, (&buf).into(), bytes_read as usize, offset as i64, move |result| {
            match result {
                Ok(n) => result_clone.store(n as isize, Ordering::SeqCst),
                Err(_) => result_clone.store(-1, Ordering::SeqCst),
            }
            done_clone.store(true, Ordering::SeqCst);
        })?;

        // Wait for write to complete
        while !done.load(Ordering::SeqCst) {
            engine.wait(100)?;
        }

        let bytes_written = result_bytes.load(Ordering::SeqCst);
        if bytes_written < 0 {
            eprintln!("\nWrite failed at offset {}", offset);
            std::process::exit(1);
        }

        total_copied += bytes_written as usize;
        offset += bytes_written as usize;

        // Progress indicator
        let progress = 100.0 * offset as f64 / file_size as f64;
        eprint!("\rProgress: {:.1}%", progress);
    }

    // Fsync
    let done = Arc::new(AtomicBool::new(false));
    let done_clone = done.clone();

    engine.fsync(dst_fd, move |_result| {
        done_clone.store(true, Ordering::SeqCst);
    })?;

    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }

    let elapsed = start_time.elapsed().as_secs_f64();

    // Results
    println!("\n\n=== Results ===");
    println!("Bytes copied: {}", total_copied);
    println!("Elapsed time: {:.3} seconds", elapsed);
    if elapsed > 0.0 {
        println!("Throughput:   {:.2} MB/s",
                 (total_copied as f64 / (1024.0 * 1024.0)) / elapsed);
    }

    // Engine stats
    let stats = engine.stats();
    println!("\nEngine statistics:");
    println!("  Ops completed:     {}", stats.ops_completed());
    println!("  P99 latency:       {:.2} ms", stats.p99_latency_ms());
    println!("  Optimal in-flight: {}", stats.optimal_in_flight());

    println!("\nDone! File copied successfully.");
    Ok(())
}
