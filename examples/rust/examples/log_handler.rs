//! Demonstrate AuraIO custom log handler (Rust)
//!
//! Shows how to install a custom log callback that formats library
//! messages with timestamps and severity levels, and how to emit
//! application-level messages through the same pipeline using
//! `aura::log_emit()`.
//!
//! Run: cargo run --example log_handler

use aura::{clear_log_handler, log_emit, set_log_handler, Engine, LogLevel, Options, Result};
use std::fs::{self, File};
use std::io::Write;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::SystemTime;

const TEST_FILE: &str = "/tmp/aura_log_test_rust.dat";
const FILE_SIZE: usize = 64 * 1024; // 64 KB
const BUF_SIZE: usize = 4096;

fn main() -> Result<()> {
    println!("AuraIO Log Handler Example (Rust)");
    println!("=================================\n");

    // --- Step 1: Install log handler with closure ------------------------
    //
    // The Rust bindings accept any Fn(LogLevel, &str) + Send + 'static.
    // Here we format each message with a millisecond timestamp and severity.

    set_log_handler(|level: LogLevel, msg: &str| {
        let now = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap_or_default();
        let secs = now.as_secs();
        let ms = now.subsec_millis();

        // Convert to local time components (simplified — no chrono dependency)
        // In production you'd use the chrono crate for proper local time.
        eprintln!(
            "{}.{:03} [myapp] {}: {}",
            secs, ms, level, msg
        );
    });

    // --- Step 2: Emit application-level messages -------------------------
    log_emit(LogLevel::Info, "log handler installed, creating engine");

    // --- Step 3: Create engine and do I/O --------------------------------
    let opts = Options::new().queue_depth(64).ring_count(1);
    let engine = Engine::with_options(&opts)?;

    log_emit(LogLevel::Notice, "engine created (1 ring, depth 64)");

    // Create test file
    {
        let mut f = File::create(TEST_FILE).expect("create test file");
        let data = vec![b'A'; FILE_SIZE];
        f.write_all(&data).expect("write test file");
    }

    let file = File::open(TEST_FILE).expect("open test file");
    let fd = file.as_raw_fd();

    let buf = engine.allocate_buffer(BUF_SIZE)?;

    log_emit(LogLevel::Debug, "submitting read");

    let done = Arc::new(AtomicBool::new(false));
    let done_clone = done.clone();

    unsafe {
        engine.read(fd, (&buf).into(), BUF_SIZE, 0, move |result| {
            if let Err(e) = &result {
                log_emit(LogLevel::Error, &format!("I/O error: {}", e));
            }
            done_clone.store(true, Ordering::SeqCst);
        })?;
    }

    while !done.load(Ordering::SeqCst) {
        engine.wait(100)?;
    }

    log_emit(LogLevel::Info, "read completed successfully");

    // --- Step 4: Show stats ----------------------------------------------
    let stats = engine.stats()?;
    println!("\nEngine stats:");
    println!("  Operations completed: {}", stats.ops_completed());
    println!("  P99 latency: {:.3} ms", stats.p99_latency_ms());

    // --- Step 5: Clean up ------------------------------------------------
    drop(file);
    drop(buf);
    fs::remove_file(TEST_FILE).ok();

    log_emit(LogLevel::Notice, "shutting down");

    // Engine dropped here while handler is still installed,
    // so we capture any shutdown diagnostics.
    drop(engine);

    // Handler no longer needed after engine is gone.
    clear_log_handler();

    println!("\n--- Summary ---");
    println!("The log handler captured all library and application messages");
    println!("on stderr with timestamps, severity levels, and an app prefix.");
    println!("In production, replace the closure with your framework's");
    println!("logging function (tracing, log, syslog, etc.).");

    Ok(())
}
