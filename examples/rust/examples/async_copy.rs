// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Async file copy example using futures
//!
//! Demonstrates AuraIO's async/await support for file operations.
//! This example uses a simple background poller pattern that works
//! with any async runtime.
//!
//! Usage: cargo run --example async_copy -- <source> <destination>

use aura::{async_io::AsyncEngine, Engine, Result};
use std::env;
use std::fs::{self, File, OpenOptions};
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Instant;

const CHUNK_SIZE: usize = 256 * 1024; // 256KB chunks

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("Usage: {} <source> <destination>", args[0]);
        eprintln!("\nCopies a file using async I/O with futures.");
        std::process::exit(1);
    }

    let src_path = &args[1];
    let dst_path = &args[2];

    println!("AuraIO Async File Copy (Rust)");
    println!("=============================");
    println!("Source:      {}", src_path);
    println!("Destination: {}", dst_path);

    // Get source file size
    let metadata = fs::metadata(src_path).map_err(|e| {
        eprintln!("Failed to stat '{}': {}", src_path, e);
        std::process::exit(1);
    }).unwrap();
    let file_size = metadata.len() as usize;

    println!("File size:   {} bytes ({:.2} MB)",
             file_size, file_size as f64 / (1024.0 * 1024.0));

    // Open files
    let src_file = File::open(src_path).map_err(|e| {
        eprintln!("Failed to open source '{}': {}", src_path, e);
        std::process::exit(1);
    }).unwrap();
    let src_fd = src_file.as_raw_fd();

    let dst_file = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open(dst_path)
        .map_err(|e| {
            eprintln!("Failed to create destination '{}': {}", dst_path, e);
            std::process::exit(1);
        }).unwrap();
    let dst_fd = dst_file.as_raw_fd();

    // Create engine (wrapped in Arc for sharing with poller thread)
    let engine = Arc::new(Engine::new()?);
    let engine_poller = engine.clone();

    // Start background completion poller
    let stop_poller = Arc::new(AtomicBool::new(false));
    let stop_clone = stop_poller.clone();
    let poller_thread = thread::spawn(move || {
        while !stop_clone.load(Ordering::Relaxed) {
            engine_poller.wait(10).ok();
        }
    });

    println!("\nCopying...");
    let start_time = Instant::now();

    // Perform the copy using async operations
    let total_copied = copy_file(&engine, src_fd, dst_fd, file_size, CHUNK_SIZE)?;

    // Fsync the destination
    let fsync_future = engine.async_fsync(dst_fd)?;
    block_on(&engine, fsync_future)?;

    let elapsed = start_time.elapsed().as_secs_f64();

    // Stop the poller
    stop_poller.store(true, Ordering::Relaxed);
    poller_thread.join().unwrap();

    // Results
    println!("\n\n=== Results ===");
    println!("Bytes copied: {}", total_copied);
    println!("Elapsed time: {:.3} seconds", elapsed);
    if elapsed > 0.0 {
        println!("Throughput:   {:.2} MB/s",
                 (total_copied as f64 / (1024.0 * 1024.0)) / elapsed);
    }

    // Engine stats
    let stats = engine.stats()?;
    println!("\nEngine statistics:");
    println!("  Ops completed:     {}", stats.ops_completed());
    println!("  P99 latency:       {:.2} ms", stats.p99_latency_ms());
    println!("  Optimal in-flight: {}", stats.optimal_in_flight());

    println!("\nDone! File copied successfully.");
    Ok(())
}

fn copy_file(
    engine: &Engine,
    src_fd: i32,
    dst_fd: i32,
    file_size: usize,
    chunk_size: usize,
) -> Result<usize> {
    let mut offset: usize = 0;
    let mut total_copied: usize = 0;

    while offset < file_size {
        let chunk = std::cmp::min(chunk_size, file_size - offset);

        // Allocate buffer for this chunk
        let buf = engine.allocate_buffer(chunk)?;

        // Async read
        let read_future = unsafe { engine.async_read(src_fd, &buf, chunk, offset as i64)? };
        let bytes_read = block_on(engine, read_future)?;

        if bytes_read == 0 {
            break; // EOF
        }

        // Async write
        let write_future = unsafe { engine.async_write(dst_fd, &buf, bytes_read, offset as i64)? };
        let bytes_written = block_on(engine, write_future)?;

        total_copied += bytes_written;
        offset += bytes_written;

        // Progress indicator
        let progress = 100.0 * offset as f64 / file_size as f64;
        eprint!("\rProgress: {:.1}%", progress);
    }

    Ok(total_copied)
}

/// Simple blocking executor for futures
fn block_on<F: std::future::Future>(_engine: &Engine, future: F) -> F::Output {
    use std::task::{Context, Poll};

    let waker = std::task::Waker::noop();
    let mut cx = Context::from_waker(&waker);
    let mut pinned = std::pin::pin!(future);

    loop {
        match pinned.as_mut().poll(&mut cx) {
            Poll::Ready(result) => return result,
            Poll::Pending => {
                // The background poller handles completions
                thread::yield_now();
            }
        }
    }
}
