// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Demonstrates cancelling an in-flight I/O operation
//!
//! Shows how to use engine.cancel() to abort a pending read.
//! The cancelled operation's callback receives Err(Error::Cancelled).
//!
//! Usage: cargo run --example cancel_request -- <file>

use aura::{Engine, Result};
use std::env;
use std::fs::File;
use std::os::unix::io::AsRawFd;
use std::sync::atomic::{AtomicBool, AtomicIsize, Ordering};
use std::sync::Arc;

const READ_SIZE: usize = 4096;

fn main() -> Result<()> {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: {} <file>", args[0]);
        std::process::exit(1);
    }

    // Create engine
    let engine = Engine::new()?;

    // Open file
    let file = File::open(&args[1])?;
    let fd = file.as_raw_fd();

    // Allocate buffer
    let buf = engine.allocate_buffer(READ_SIZE)?;

    // Completion state with Arc for safe sharing
    let completed = Arc::new(AtomicBool::new(false));
    let read_result = Arc::new(AtomicIsize::new(0));

    let completed_clone = Arc::clone(&completed);
    let result_clone = Arc::clone(&read_result);

    // Submit async read with closure callback
    println!("Submitting async read...");
    let req = unsafe {
        engine.read(
            fd,
            (&buf).into(),
            READ_SIZE,
            0,
            move |result| {
                match result {
                    Ok(n) => {
                        println!("Read completed: {} bytes", n);
                        result_clone.store(n as isize, Ordering::SeqCst);
                    }
                    Err(e) => {
                        match e {
                            aura::Error::Cancelled => {
                                println!("Read was cancelled (result = -ECANCELED)");
                                result_clone.store(-(libc::ECANCELED as isize), Ordering::SeqCst);
                            }
                            aura::Error::Io(io_err) => {
                                eprintln!("Read failed: {}", io_err);
                                let err_code = io_err.raw_os_error().unwrap_or(-1);
                                result_clone.store(-(err_code as isize), Ordering::SeqCst);
                            }
                            _ => {
                                eprintln!("Read failed: {}", e);
                                result_clone.store(-1, Ordering::SeqCst);
                            }
                        }
                    }
                }
                completed_clone.store(true, Ordering::SeqCst);
            },
        )?
    };

    // Attempt to cancel the request.
    // Note: cancellation is best-effort. If the I/O already completed
    // by the time cancel is processed, the original result is returned
    // instead of Err(Error::Cancelled).
    println!("Attempting to cancel...");
    unsafe {
        match engine.cancel(&req) {
            Ok(_) => println!("Cancel request submitted successfully"),
            Err(_) => println!("Cancel submission failed (operation may have already completed)"),
        }
    }

    // Wait for completion callback
    while !completed.load(Ordering::SeqCst) {
        engine.wait(100)?; // 100ms timeout
    }

    let final_result = read_result.load(Ordering::SeqCst);
    let status_str = if final_result == -(libc::ECANCELED as isize) {
        "cancelled"
    } else if final_result < 0 {
        "error"
    } else {
        "success"
    };

    println!("Final result: {} ({})", final_result, status_str);

    Ok(())
}
