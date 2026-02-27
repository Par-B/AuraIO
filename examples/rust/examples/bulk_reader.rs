// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! High-throughput bulk file reader example
//!
//! Demonstrates reading many files concurrently with adaptive tuning.
//! This is a simplified version that reads files sequentially to avoid
//! Send trait issues with raw pointers in callbacks.
//!
//! Usage: cargo run --example bulk_reader -- <directory>

use aura::{Engine, Result};
use std::env;
use std::fs::{self, File};
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicI64, AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Instant;

const MAX_FILES: usize = 1000;
const READ_SIZE: usize = 256 * 1024; // 256KB per file
const STATS_INTERVAL: usize = 100; // Print stats every N completions

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: {} <directory>", args[0]);
        eprintln!("\nReads all regular files in a directory using async I/O.");
        eprintln!("The engine self-tunes for optimal throughput.");
        std::process::exit(1);
    }

    let dirname = &args[1];

    // Create the aura engine
    println!("Creating async I/O engine...");
    let engine = Engine::new()?;

    println!("Scanning directory '{}'...", dirname);

    // Collect files to read
    let dir = fs::read_dir(dirname).map_err(|e| {
        eprintln!("Failed to open directory '{}': {}", dirname, e);
        std::process::exit(1);
    }).unwrap();

    let mut files: Vec<std::path::PathBuf> = Vec::new();
    for entry in dir {
        if files.len() >= MAX_FILES {
            break;
        }

        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };

        let path = entry.path();
        let metadata = match fs::metadata(&path) {
            Ok(m) => m,
            Err(_) => continue,
        };

        if metadata.is_file() {
            files.push(path);
        }
    }

    if files.is_empty() {
        println!("No files found to read.");
        std::process::exit(1);
    }

    println!("Found {} files to read", files.len());

    // Track progress
    let files_completed = Arc::new(AtomicUsize::new(0));
    let bytes_read = Arc::new(AtomicI64::new(0));
    let errors = Arc::new(AtomicI64::new(0));
    let files_pending = Arc::new(AtomicUsize::new(0));
    let start_time = Instant::now();

    // Submit reads for all files
    println!("Submitting async reads...\n");

    for path in &files {
        let file = match File::open(path) {
            Ok(f) => f,
            Err(_) => {
                errors.fetch_add(1, Ordering::SeqCst);
                continue;
            }
        };
        let fd = file.as_raw_fd();

        let buf = match engine.allocate_buffer(READ_SIZE) {
            Ok(b) => b,
            Err(_) => {
                errors.fetch_add(1, Ordering::SeqCst);
                continue;
            }
        };

        let files_completed_clone = files_completed.clone();
        let bytes_read_clone = bytes_read.clone();
        let errors_clone = errors.clone();
        let files_pending_clone = files_pending.clone();
        let start = start_time;

        // Get raw pointer before moving buf into the closure
        let buf_ref = unsafe { aura::BufferRef::from_ptr(buf.as_ptr() as *mut std::ffi::c_void) };

        // Submit the read - file and buf are moved into the closure to keep
        // them alive until the callback fires, then dropped for cleanup
        let result = unsafe {
            engine.read(
                fd,
                buf_ref,
                READ_SIZE,
                0,
                0,
                move |result| {
                    // file and buf are dropped when this closure completes,
                    // closing the fd and freeing the buffer automatically
                    let _file = file;
                    let _buf = buf;

                    match result {
                        Ok(n) => {
                            bytes_read_clone.fetch_add(n as i64, Ordering::SeqCst);
                        }
                        Err(_) => {
                            errors_clone.fetch_add(1, Ordering::SeqCst);
                        }
                    }

                    let completed = files_completed_clone.fetch_add(1, Ordering::SeqCst) + 1;
                    files_pending_clone.fetch_sub(1, Ordering::SeqCst);

                    // Print periodic stats
                    if completed % STATS_INTERVAL == 0 {
                        let elapsed = start.elapsed().as_secs_f64();
                        let bytes = bytes_read_clone.load(Ordering::SeqCst);
                        let mb = bytes as f64 / (1024.0 * 1024.0);
                        let mb_per_sec = mb / elapsed;

                        eprint!("\rProgress: {} files, {:.2} MB, {:.2} MB/s",
                                completed, mb, mb_per_sec);
                    }
                },
            )
        };

        match result {
            Ok(_) => {
                files_pending.fetch_add(1, Ordering::SeqCst);
            }
            Err(_) => {
                // Submission failed - file and buf were in the closure which
                // the engine already dropped, so cleanup is automatic
                errors.fetch_add(1, Ordering::SeqCst);
            }
        }
    }

    // Wait for all completions
    while files_pending.load(Ordering::SeqCst) > 0 {
        engine.wait(100)?;
    }

    // Final stats
    println!("\n\n=== Final Results ===");

    let total_elapsed = start_time.elapsed().as_secs_f64();
    let completed = files_completed.load(Ordering::SeqCst);
    let bytes = bytes_read.load(Ordering::SeqCst);
    let error_count = errors.load(Ordering::SeqCst);

    println!("Files read:       {}", completed);
    println!("Total bytes:      {:.2} MB", bytes as f64 / (1024.0 * 1024.0));
    println!("Errors:           {}", error_count);
    println!("Elapsed time:     {:.2} seconds", total_elapsed);
    if total_elapsed > 0.0 {
        println!("Average speed:    {:.2} MB/s",
                 (bytes as f64 / (1024.0 * 1024.0)) / total_elapsed);
    }

    // Engine tuning results
    let stats = engine.stats()?;
    println!("\nAdaptive tuning results:");
    println!("  Optimal in-flight: {}", stats.optimal_in_flight());
    println!("  Optimal batch:     {}", stats.optimal_batch_size());
    println!("  P99 latency:       {:.2} ms", stats.p99_latency_ms());

    println!("\nDone!");
    Ok(())
}
