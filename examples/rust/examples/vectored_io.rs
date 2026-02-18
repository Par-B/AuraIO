// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Demonstrate vectored (scatter-gather) I/O operations (Rust version)
//!
//! Shows how to use readv() and writev() for efficient I/O with multiple
//! buffers in a single syscall. Useful for:
//! - Database page operations (header + data in separate buffers)
//! - Structured file formats (metadata + content sections)
//! - Log files (timestamp/metadata + message in separate buffers)
//! - Reducing syscall overhead for non-contiguous data regions
//!
//! Usage: cargo run --example vectored_io --manifest-path examples/rust/Cargo.toml

use aura::{Engine, Result};
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Arc;

const TEST_FILE: &str = "/tmp/aura_vectored_test.dat";
const HEADER_SIZE: usize = 64;
const PAYLOAD_SIZE: usize = 1024;

struct IoContext {
    done: AtomicBool,
    result: AtomicIsize,
}

impl IoContext {
    fn new() -> Self {
        Self {
            done: AtomicBool::new(false),
            result: AtomicIsize::new(0),
        }
    }
}

fn main() -> Result<()> {
    println!("AuraIO Vectored I/O Example (Rust)");
    println!("===================================\n");

    let engine = Engine::new()?;

    // ===================================================================
    // Example 1: Vectored Write (Gather)
    // Write metadata and data in a single operation (like database page writes)
    // ===================================================================
    println!("Example 1: Vectored Write (Gather)");
    println!("Writing structured data with separate metadata and payload buffers...");

    // Prepare metadata buffer (like a database page header)
    let mut header = vec![0u8; HEADER_SIZE];
    let header_text = format!(
        "METADATA: size={}, checksum=0xABCD, version=1",
        PAYLOAD_SIZE
    );
    header[..header_text.len()].copy_from_slice(header_text.as_bytes());

    // Prepare payload buffer
    let payload = vec![b'X'; PAYLOAD_SIZE];

    // Create iovec array for gather write
    let write_iovs = vec![
        libc::iovec {
            iov_base: header.as_ptr() as *mut _,
            iov_len: HEADER_SIZE,
        },
        libc::iovec {
            iov_base: payload.as_ptr() as *mut _,
            iov_len: PAYLOAD_SIZE,
        },
    ];

    // Open file for writing
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

    // Submit vectored write
    let write_ctx = Arc::new(IoContext::new());
    let write_ctx_clone = write_ctx.clone();

    unsafe {
        engine.writev(wfd, &write_iovs, 0, move |result| {
            match result {
                Ok(n) => {
                    println!("Vectored write completed: {} bytes", n);
                    write_ctx_clone.result.store(n as isize, Ordering::SeqCst);
                }
                Err(e) => {
                    eprintln!("Write error: {}", e);
                    write_ctx_clone.result.store(-1, Ordering::SeqCst);
                }
            }
            write_ctx_clone.done.store(true, Ordering::SeqCst);
        })?;
    }

    // Wait for write completion
    while !write_ctx.done.load(Ordering::SeqCst) {
        engine.wait(10)?;
    }

    println!("  Header ({} bytes): {}", HEADER_SIZE, header_text);
    println!(
        "  Payload ({} bytes): [{} bytes of 'X']",
        PAYLOAD_SIZE, PAYLOAD_SIZE
    );
    println!(
        "  Total written: {} bytes",
        write_ctx.result.load(Ordering::SeqCst)
    );

    unsafe {
        libc::close(wfd);
    }

    // ===================================================================
    // Example 2: Vectored Read (Scatter)
    // Read metadata and data into separate buffers (like database page reads)
    // ===================================================================
    println!("\nExample 2: Vectored Read (Scatter)");
    println!("Reading structured data into separate metadata and payload buffers...");

    // Allocate separate buffers for header and payload
    let mut read_header = vec![0u8; HEADER_SIZE];
    let mut read_payload = vec![0u8; PAYLOAD_SIZE];

    // Create iovec array for scatter read
    let read_iovs = vec![
        libc::iovec {
            iov_base: read_header.as_mut_ptr() as *mut _,
            iov_len: HEADER_SIZE,
        },
        libc::iovec {
            iov_base: read_payload.as_mut_ptr() as *mut _,
            iov_len: PAYLOAD_SIZE,
        },
    ];

    // Open file for reading
    let rfd = unsafe {
        libc::open(
            std::ffi::CString::new(TEST_FILE).unwrap().as_ptr(),
            libc::O_RDONLY,
        )
    };
    if rfd < 0 {
        std::fs::remove_file(TEST_FILE).ok();
        return Err(aura::Error::Io(std::io::Error::last_os_error()));
    }

    // Submit vectored read
    let read_ctx = Arc::new(IoContext::new());
    let read_ctx_clone = read_ctx.clone();

    unsafe {
        engine.readv(rfd, &read_iovs, 0, move |result| {
            match result {
                Ok(n) => {
                    println!("Vectored read completed: {} bytes", n);
                    read_ctx_clone.result.store(n as isize, Ordering::SeqCst);
                }
                Err(e) => {
                    eprintln!("Read error: {}", e);
                    read_ctx_clone.result.store(-1, Ordering::SeqCst);
                }
            }
            read_ctx_clone.done.store(true, Ordering::SeqCst);
        })?;
    }

    // Wait for read completion
    while !read_ctx.done.load(Ordering::SeqCst) {
        engine.wait(10)?;
    }

    // Convert header to string for display
    let header_str = String::from_utf8_lossy(&read_header);
    let header_display = header_str.trim_end_matches('\0');

    println!("  Header ({} bytes): {}", HEADER_SIZE, header_display);
    print!("  Payload ({} bytes): [First 20 bytes: ", PAYLOAD_SIZE);
    for &byte in read_payload.iter().take(20) {
        print!("{}", byte as char);
    }
    println!("...]");
    println!(
        "  Total read: {} bytes",
        read_ctx.result.load(Ordering::SeqCst)
    );

    // Verify data integrity
    let payload_ok = read_payload.iter().take(PAYLOAD_SIZE).all(|&b| b == b'X');

    println!(
        "\nData integrity check: {}",
        if payload_ok { "PASSED" } else { "FAILED" }
    );

    // Cleanup
    unsafe {
        libc::close(rfd);
    }
    std::fs::remove_file(TEST_FILE).ok();

    println!("\n======================================");
    println!("Vectored I/O Benefits:");
    println!("- Single syscall for multiple buffers");
    println!("- Natural separation of metadata/data (e.g., database pages)");
    println!("- Efficient for structured file formats");
    println!("- Reduces syscall overhead vs multiple read/write calls");
    println!("- Useful for log files with separate metadata and content sections");

    Ok(())
}
