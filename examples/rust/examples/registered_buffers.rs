//! Demonstrate registered (fixed) buffers for zero-copy I/O (Rust version)
//!
//! Pre-registering buffers with the kernel eliminates per-operation mapping
//! overhead. Best for workloads with:
//! - Same buffers reused across 1000+ I/O operations
//! - High-frequency small I/O (< 16KB) where mapping overhead is significant
//! - Zero-copy is critical for performance
//!
//! Usage: cargo run --example registered_buffers --manifest-path examples/rust/Cargo.toml

use aura::{BufferRef, Engine, Result};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::time::Instant;

const TEST_FILE: &str = "/tmp/aura_reg_buf_test.dat";
const FILE_SIZE: usize = 1 * 1024 * 1024; // 1 MB
const BUF_SIZE: usize = 4096;
const NUM_BUFFERS: usize = 4;
const NUM_OPS: usize = 50;

fn run_benchmark(
    engine: &Engine,
    fd: i32,
    use_registered: bool,
    buffers: &[aura::Buffer],
) -> Result<f64> {
    let completed = Arc::new(AtomicUsize::new(0));
    let start = Instant::now();

    // Submit NUM_OPS reads using registered or unregistered buffers with proper pacing
    let mut submitted = 0;
    let max_inflight = if use_registered { NUM_BUFFERS } else { 8 };

    while submitted < NUM_OPS || completed.load(Ordering::Relaxed) < NUM_OPS {
        // Submit new operations while under the concurrency limit
        while submitted < NUM_OPS && (submitted - completed.load(Ordering::Relaxed)) < max_inflight
        {
            let buf_idx = submitted % NUM_BUFFERS;
            let offset = (rand::random::<usize>() % (FILE_SIZE / BUF_SIZE)) * BUF_SIZE;

            let buf_ref = if use_registered {
                // Use registered buffer by index
                BufferRef::fixed(buf_idx as i32, 0)
            } else {
                // Use unregistered buffer
                (&buffers[buf_idx]).into()
            };

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
    Ok(elapsed)
}

fn main() -> Result<()> {
    println!("AuraIO Registered Buffers Example (Rust)");
    println!("=========================================\n");

    // Create test file
    println!("Creating test file ({} MB)...", FILE_SIZE / (1024 * 1024));
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
    // Part 1: Unregistered Buffers (Baseline)
    // ===================================================================
    println!("\nPart 1: Baseline with unregistered buffers");
    println!("Running {} operations...", NUM_OPS);

    let unreg_time;
    {
        let engine_unreg = Engine::new()?;

        // Allocate buffers
        let mut unreg_buffers = Vec::new();
        for _ in 0..NUM_BUFFERS {
            unreg_buffers.push(engine_unreg.allocate_buffer(BUF_SIZE)?);
        }

        unreg_time = run_benchmark(&engine_unreg, fd, false, &unreg_buffers)?;

        let unreg_stats = engine_unreg.stats()?;

        println!("  Time: {:.2} ms", unreg_time);
        println!(
            "  Throughput: {:.2} MB/s",
            unreg_stats.throughput_bps() / (1024.0 * 1024.0)
        );
        println!("  P99 Latency: {:.3} ms", unreg_stats.p99_latency_ms());
    }

    // ===================================================================
    // Part 2: Registered Buffers (Zero-Copy)
    // ===================================================================
    println!("\nPart 2: Zero-copy with registered buffers");
    println!(
        "Registering {} buffers of {} bytes each...",
        NUM_BUFFERS, BUF_SIZE
    );

    let reg_time;
    {
        let engine_reg = Engine::new()?;

        // Allocate buffers
        let mut reg_buffers = Vec::new();
        for _ in 0..NUM_BUFFERS {
            reg_buffers.push(engine_reg.allocate_buffer(BUF_SIZE)?);
        }

        // Prepare buffer slices for registration
        let buffer_slices: Vec<&mut [u8]> = reg_buffers
            .iter_mut()
            .map(|buf| buf.as_mut_slice())
            .collect();

        // Register buffers with kernel
        unsafe {
            engine_reg.register_buffers(&buffer_slices)?;
        }

        println!("Buffers registered successfully.");
        println!("Running {} operations with zero-copy I/O...", NUM_OPS);

        reg_time = run_benchmark(&engine_reg, fd, true, &reg_buffers)?;

        let reg_stats = engine_reg.stats()?;

        println!("  Time: {:.2} ms", reg_time);
        println!(
            "  Throughput: {:.2} MB/s",
            reg_stats.throughput_bps() / (1024.0 * 1024.0)
        );
        println!("  P99 Latency: {:.3} ms", reg_stats.p99_latency_ms());

        // ===================================================================
        // Part 3: Performance Comparison
        // ===================================================================
        println!("\n======================================");
        println!("Performance Comparison:");
        println!("  Unregistered: {:.2} ms", unreg_time);
        println!("  Registered:   {:.2} ms", reg_time);
        if reg_time > 0.0 {
            println!("  Speedup:      {:.2}x", unreg_time / reg_time);
            println!(
                "  Improvement:  {:.1}%",
                ((unreg_time - reg_time) / unreg_time) * 100.0
            );
        }

        // ===================================================================
        // Part 4: Deferred Buffer Unregister (Callback-Safe Pattern)
        // ===================================================================
        println!("\nPart 4: Deferred buffer unregister");
        println!("This pattern allows safe unregister from callback context...");

        // Request deferred unregister (returns immediately)
        engine_reg.request_unregister_buffers()?;
        println!("  Unregister requested (will complete when in-flight ops drain)");

        // Wait a bit to ensure unregister completes
        engine_reg.wait(100)?;

        println!("  Buffers unregistered.");

        // Buffers are automatically freed when engine_reg and reg_buffers drop
    }

    // Cleanup
    unsafe {
        libc::close(fd);
    }
    std::fs::remove_file(TEST_FILE).ok();

    println!("\n======================================");
    println!("When to Use Registered Buffers:");
    println!("  ✓ Same buffers reused 1000+ times");
    println!("  ✓ High-frequency small I/O (< 16KB)");
    println!("  ✓ Zero-copy is critical");
    println!("  ✗ One-off or infrequent operations");
    println!("  ✗ Dynamic buffer count");
    println!("  ✗ Simpler code without registration");

    Ok(())
}
