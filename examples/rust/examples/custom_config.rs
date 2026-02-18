// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Demonstrate AuraIO custom configuration options (Rust version)
//!
//! Shows how to tune the engine for different workload characteristics:
//! - Ring count and queue depth
//! - In-flight limits and target latency
//! - Ring selection strategies
//!
//! Usage: cargo run --example custom_config --manifest-path examples/rust/Cargo.toml

use aura::{Engine, Options, RingSelect, Result};
use std::os::unix::io::RawFd;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Instant;

const TEST_FILE: &str = "/tmp/aura_config_test.dat";
const FILE_SIZE: usize = 4 * 1024 * 1024; // 4 MB
const BUF_SIZE: usize = 4096;
const NUM_OPS: usize = 20;
const CONCURRENT_BUFS: usize = 16;

fn print_stats(config_name: &str, engine: &Engine, elapsed_ms: f64) {
    let stats = engine.stats().unwrap();

    println!("\n{} Configuration:", config_name);
    println!("  Elapsed time: {:.2} ms", elapsed_ms);
    println!("  Operations: {}", stats.ops_completed());
    println!(
        "  Throughput: {:.2} MB/s",
        stats.throughput_bps() / (1024.0 * 1024.0)
    );
    println!("  P99 Latency: {:.3} ms", stats.p99_latency_ms());
    println!("  Optimal in-flight: {}", stats.optimal_in_flight());
    println!("  Optimal batch size: {}", stats.optimal_batch_size());
}

fn run_workload(engine: &Engine, fd: RawFd, config_name: &str) -> Result<()> {
    // Allocate buffers for concurrent operations
    let mut bufs = Vec::new();
    for _ in 0..CONCURRENT_BUFS {
        bufs.push(engine.allocate_buffer(BUF_SIZE)?);
    }

    let completed = Arc::new(AtomicUsize::new(0));
    let start = Instant::now();

    // Submit NUM_OPS reads at random offsets with proper pacing
    let mut submitted = 0;
    while submitted < NUM_OPS || completed.load(Ordering::Relaxed) < NUM_OPS {
        // Submit new operations while under the concurrency limit
        while submitted < NUM_OPS
            && (submitted - completed.load(Ordering::Relaxed)) < CONCURRENT_BUFS / 2
        {
            let offset = (rand::random::<usize>() % (FILE_SIZE / BUF_SIZE)) * BUF_SIZE;
            let buf_ref = (&bufs[submitted % CONCURRENT_BUFS]).into();
            let completed_clone = completed.clone();

            unsafe {
                engine.read(fd, buf_ref, BUF_SIZE, offset as i64, move |result| {
                    if let Err(e) = result {
                        eprintln!("I/O error: {}", e);
                    }
                    completed_clone.fetch_add(1, Ordering::Relaxed);
                })?;
            }
            submitted += 1;
        }

        // Poll for completions
        engine.poll()?;

        // If we're done submitting, wait for remaining completions
        if submitted >= NUM_OPS && completed.load(Ordering::Relaxed) < NUM_OPS {
            engine.wait(1)?;
        }
    }

    let elapsed = start.elapsed().as_secs_f64() * 1000.0;
    print_stats(config_name, engine, elapsed);

    Ok(())
}

fn main() -> Result<()> {
    println!("AuraIO Custom Configuration Examples (Rust)");
    println!("============================================\n");

    // Create test file
    let wfd = unsafe {
        libc::open(
            std::ffi::CString::new(TEST_FILE).unwrap().as_ptr(),
            libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC,
            0o644,
        )
    };
    if wfd < 0 {
        return Err(aura::Error::Io(std::io::Error::last_os_error()));
    }

    let data = vec![0u8; FILE_SIZE];
    let written = unsafe { libc::write(wfd, data.as_ptr() as *const _, FILE_SIZE) };
    unsafe {
        libc::close(wfd);
    }
    if written != FILE_SIZE as isize {
        std::fs::remove_file(TEST_FILE).ok();
        return Err(aura::Error::Io(std::io::Error::last_os_error()));
    }

    // Open for reading
    let fd = unsafe {
        libc::open(
            std::ffi::CString::new(TEST_FILE).unwrap().as_ptr(),
            libc::O_RDONLY,
        )
    };
    if fd < 0 {
        std::fs::remove_file(TEST_FILE).ok();
        return Err(aura::Error::Io(std::io::Error::last_os_error()));
    }

    // ===================================================================
    // Example 1: Default Configuration
    // ===================================================================
    println!("Running with default configuration...");
    {
        let engine_default = Engine::new()?;
        run_workload(&engine_default, fd, "Default")?;
    }

    // ===================================================================
    // Example 2: High-Throughput Configuration
    // - Larger queue depth for more pipelining
    // - Higher initial in-flight limit
    // - Round-robin ring selection for single-thread scaling
    // ===================================================================
    println!("\nConfiguring for high throughput...");
    {
        let opts_throughput = Options::new()
            .queue_depth(512) // Deeper queues for more pipelining
            .initial_in_flight(128) // Start with high concurrency
            .ring_select(RingSelect::RoundRobin); // Max single-thread scaling

        let engine_throughput = Engine::with_options(&opts_throughput)?;
        run_workload(&engine_throughput, fd, "High Throughput")?;
    }

    // ===================================================================
    // Example 3: Low-Latency Configuration
    // - Target specific P99 latency
    // - Conservative in-flight limit to reduce queuing
    // - CPU-local ring selection for best cache locality
    // ===================================================================
    println!("\nConfiguring for low latency...");
    {
        let opts_latency = Options::new()
            .max_p99_latency_ms(1.0) // Target 1ms P99
            .initial_in_flight(8) // Start conservative
            .min_in_flight(4) // Never go below 4
            .ring_select(RingSelect::CpuLocal); // Best cache locality

        let engine_latency = Engine::with_options(&opts_latency)?;
        run_workload(&engine_latency, fd, "Low Latency")?;
    }

    // ===================================================================
    // Example 4: Adaptive Configuration (Recommended for Production)
    // - Let AIMD tuning find optimal settings
    // - Adaptive ring selection with congestion-based spilling
    // - Moderate queue depth for good balance
    // ===================================================================
    println!("\nConfiguring for adaptive tuning (production recommended)...");
    {
        let opts_adaptive = Options::new()
            .queue_depth(256) // Balanced queue depth
            .ring_select(RingSelect::Adaptive) // Power-of-two spilling
            .max_p99_latency_ms(5.0); // Reasonable latency target

        let engine_adaptive = Engine::with_options(&opts_adaptive)?;
        run_workload(&engine_adaptive, fd, "Adaptive (Recommended)")?;
    }

    // ===================================================================
    // Example 5: Custom Ring Count
    // - Useful for NUMA systems or limiting resource usage
    // ===================================================================
    println!("\nConfiguring with custom ring count...");
    {
        let opts_custom_rings = Options::new()
            .ring_count(2) // Use only 2 rings (vs default per-CPU)
            .queue_depth(256);

        let engine_custom = Engine::with_options(&opts_custom_rings)?;
        run_workload(&engine_custom, fd, "Custom Ring Count (2)")?;
    }

    // Cleanup
    unsafe {
        libc::close(fd);
    }
    std::fs::remove_file(TEST_FILE).ok();

    println!("\n======================================");
    println!("Configuration Summary:");
    println!("- Default: Auto-configured for system");
    println!("- High Throughput: Large queues, round-robin, high concurrency");
    println!("- Low Latency: CPU-local, target P99, conservative limits");
    println!("- Adaptive: RECOMMENDED for production - automatic tuning");
    println!("- Custom Rings: Control resource usage");
    println!("\nChoose configuration based on your workload characteristics.");

    Ok(())
}
