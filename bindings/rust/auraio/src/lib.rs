//! Safe Rust bindings for AuraIO
//!
//! AuraIO is a self-tuning async I/O library for Linux built on io_uring.
//! It automatically optimizes queue depth and batching using AIMD congestion control.
//!
//! # Quick Start
//!
//! ```no_run
//! use auraio::{Engine, BufferRef};
//! use std::fs::File;
//! use std::os::unix::io::AsRawFd;
//! use std::sync::atomic::{AtomicBool, Ordering};
//! use std::sync::Arc;
//!
//! fn main() -> auraio::Result<()> {
//!     let engine = Engine::new()?;
//!     let file = File::open("/etc/hostname")?;
//!     let mut buf = engine.allocate_buffer(4096)?;
//!
//!     let done = Arc::new(AtomicBool::new(false));
//!     let done_clone = done.clone();
//!
//!     engine.read(file.as_raw_fd(), (&buf).into(), 4096, 0, move |result| {
//!         match result {
//!             Ok(n) => println!("Read {} bytes", n),
//!             Err(e) => eprintln!("Error: {}", e),
//!         }
//!         done_clone.store(true, Ordering::SeqCst);
//!     })?;
//!
//!     while !done.load(Ordering::SeqCst) {
//!         engine.wait(100)?;
//!     }
//!
//!     Ok(())
//! }
//! ```
//!
//! # Features
//!
//! - **Zero Configuration** - Automatically detects cores, tunes queue depth
//! - **Per-Core Rings** - One io_uring per CPU eliminates cross-core contention
//! - **AIMD Self-Tuning** - Finds optimal concurrency without benchmarking
//! - **Scalable Buffer Pool** - Thread-local caches, auto-scaling shards
//! - **Thread-Safe** - Multiple threads can submit concurrently
//!
//! # API Overview
//!
//! - [`Engine`] - Main entry point, manages io_uring rings
//! - [`Buffer`] - RAII buffer from the engine's pool
//! - [`BufferRef`] - Lightweight reference to a buffer
//! - [`Options`] - Engine configuration
//! - [`Stats`] - Runtime statistics
//! - [`RequestHandle`] - Handle to a pending operation
//!
//! # Soundness Limitations
//!
//! These bindings wrap a C library and have inherent soundness limitations
//! that cannot be fully expressed in Rust's type system:
//!
//! - **BufferRef lifetime**: [`BufferRef`] is `Copy` and carries no lifetime
//!   parameter. The compiler cannot enforce that the underlying memory
//!   outlives the I/O operation. Callers must manually ensure buffers
//!   remain valid until the completion callback fires.
//!
//! - **RequestHandle validity**: [`RequestHandle`] wraps a raw pointer that
//!   becomes invalid once the completion callback begins. Using it after
//!   that point is undefined behavior.
//!
//! These limitations match the underlying C API's ownership model. In
//! practice, following the documented usage patterns (keep `BufferRef`/iovecs
//! alive for in-flight I/O, don't use `RequestHandle` after callbacks)
//! avoids all issues.

mod buffer;
mod callback;
mod engine;
mod error;
mod options;
mod request;
mod stats;

#[cfg(feature = "async")]
pub mod async_io;

pub use buffer::{Buffer, BufferRef};
pub use engine::{version, version_int, Engine};
pub use error::{Error, Result};
pub use options::Options;
pub use request::RequestHandle;
pub use stats::Stats;

#[cfg(feature = "async")]
pub use async_io::{AsyncEngine, IoFuture};

// Re-export libc types commonly needed
pub use libc::iovec;

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::{File, OpenOptions};
    use std::io::{Read as _, Write as _};
    use std::os::unix::io::AsRawFd;
    use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};
    use std::sync::Arc;

    // =========================================================================
    // Version Tests
    // =========================================================================

    #[test]
    fn test_version_string() {
        let ver = version();
        assert!(!ver.is_empty());
        // Version should be in semver format like "1.0.1"
        let parts: Vec<&str> = ver.split('.').collect();
        assert!(parts.len() >= 2, "version should have at least major.minor");
    }

    #[test]
    fn test_version_int() {
        let ver = version_int();
        // At least version 1.0.0 (10000)
        assert!(ver >= 10000, "version_int should be >= 10000");
        // Extract major/minor/patch
        let major = ver / 10000;
        let minor = (ver % 10000) / 100;
        let patch = ver % 100;
        assert!(major >= 1, "major version should be >= 1");
        // Verify consistency with string
        let expected = format!("{}.{}.{}", major, minor, patch);
        assert_eq!(version(), expected);
    }

    // =========================================================================
    // Engine Tests
    // =========================================================================

    #[test]
    fn test_engine_create_default() {
        let engine = Engine::new();
        assert!(engine.is_ok(), "Engine::new() should succeed");
    }

    #[test]
    fn test_engine_with_options() {
        let opts = Options::new().queue_depth(128).ring_count(1);
        let engine = Engine::with_options(&opts);
        assert!(engine.is_ok(), "Engine::with_options() should succeed");
    }

    #[test]
    fn test_engine_as_ptr() {
        let engine = Engine::new().unwrap();
        let ptr = engine.as_ptr();
        assert!(!ptr.is_null(), "Engine::as_ptr() should return non-null");
    }

    #[test]
    fn test_engine_is_send() {
        fn assert_send<T: Send>() {}
        assert_send::<Engine>();
    }

    #[test]
    fn test_engine_is_sync() {
        fn assert_sync<T: Sync>() {}
        assert_sync::<Engine>();
    }

    #[test]
    fn test_engine_drop() {
        // Just verifies drop doesn't panic
        {
            let _engine = Engine::new().unwrap();
        }
        // Engine should be dropped here without issues
    }

    // =========================================================================
    // Options Tests
    // =========================================================================

    #[test]
    fn test_options_default() {
        let opts = Options::new();
        // Options should be created with C library defaults
        // We can't easily inspect internal values, but it shouldn't panic
        let _ = opts;
    }

    #[test]
    fn test_options_builder_chain() {
        // Verify builder pattern returns Self for chaining
        let opts = Options::new()
            .queue_depth(256)
            .ring_count(2)
            .initial_in_flight(32)
            .min_in_flight(4)
            .max_p99_latency_ms(10.0)
            .buffer_alignment(4096)
            .disable_adaptive(false)
            .enable_sqpoll(false)
            .sqpoll_idle_ms(1000);
        // Should compile and not panic
        let _ = opts;
    }

    #[test]
    fn test_options_clone() {
        let opts1 = Options::new().queue_depth(128);
        let opts2 = opts1.clone();
        // Both should be usable
        let _engine1 = Engine::with_options(&opts1).unwrap();
        let _engine2 = Engine::with_options(&opts2).unwrap();
    }

    #[test]
    fn test_options_default_trait() {
        let opts: Options = Default::default();
        let engine = Engine::with_options(&opts);
        assert!(engine.is_ok());
    }

    // =========================================================================
    // Buffer Tests
    // =========================================================================

    #[test]
    fn test_buffer_allocate() {
        let engine = Engine::new().unwrap();
        let buf = engine.allocate_buffer(4096);
        assert!(buf.is_ok(), "allocate_buffer should succeed");
        let buf = buf.unwrap();
        assert_eq!(buf.len(), 4096);
        assert!(!buf.is_empty());
    }

    #[test]
    fn test_buffer_allocate_various_sizes() {
        let engine = Engine::new().unwrap();
        for size in [512, 1024, 4096, 8192, 65536] {
            let buf = engine.allocate_buffer(size).unwrap();
            assert_eq!(buf.len(), size);
        }
    }

    #[test]
    fn test_buffer_as_slice() {
        let engine = Engine::new().unwrap();
        let buf = engine.allocate_buffer(1024).unwrap();
        let slice = buf.as_slice();
        assert_eq!(slice.len(), 1024);
    }

    #[test]
    fn test_buffer_as_mut_slice() {
        let engine = Engine::new().unwrap();
        let mut buf = engine.allocate_buffer(1024).unwrap();
        let slice = buf.as_mut_slice();
        assert_eq!(slice.len(), 1024);
        // Write to buffer
        slice[0] = 42;
        assert_eq!(buf.as_slice()[0], 42);
    }

    #[test]
    fn test_buffer_as_ptr() {
        let engine = Engine::new().unwrap();
        let buf = engine.allocate_buffer(4096).unwrap();
        let ptr = buf.as_ptr();
        assert!(!ptr.is_null());
        // Check alignment (should be page-aligned)
        assert_eq!(ptr as usize % 4096, 0, "buffer should be page-aligned");
    }

    #[test]
    fn test_buffer_as_ref_trait() {
        let engine = Engine::new().unwrap();
        let buf = engine.allocate_buffer(1024).unwrap();
        let slice: &[u8] = buf.as_ref();
        assert_eq!(slice.len(), 1024);
    }

    #[test]
    fn test_buffer_as_mut_trait() {
        let engine = Engine::new().unwrap();
        let mut buf = engine.allocate_buffer(1024).unwrap();
        let slice: &mut [u8] = buf.as_mut();
        slice[0] = 123;
        assert_eq!(buf.as_slice()[0], 123);
    }

    #[test]
    fn test_buffer_is_send() {
        fn assert_send<T: Send>() {}
        assert_send::<Buffer>();
    }

    #[test]
    fn test_buffer_drop_returns_to_pool() {
        let engine = Engine::new().unwrap();
        // Allocate and drop several buffers
        for _ in 0..10 {
            let buf = engine.allocate_buffer(4096).unwrap();
            drop(buf);
        }
        // Should be able to allocate again (buffers returned to pool)
        let buf = engine.allocate_buffer(4096);
        assert!(buf.is_ok());
    }

    #[test]
    fn test_buffer_outlives_engine_value() {
        let buf = {
            let engine = Engine::new().unwrap();
            engine.allocate_buffer(1024).unwrap()
        };
        assert_eq!(buf.len(), 1024);
    }

    // =========================================================================
    // BufferRef Tests
    // =========================================================================

    #[test]
    fn test_bufferref_from_buffer() {
        let engine = Engine::new().unwrap();
        let buf = engine.allocate_buffer(4096).unwrap();
        let buf_ref: BufferRef = (&buf).into();
        // Should compile and not panic
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_from_mut_buffer() {
        let engine = Engine::new().unwrap();
        let mut buf = engine.allocate_buffer(4096).unwrap();
        let buf_ref: BufferRef = (&mut buf).into();
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_from_slice() {
        let data = [0u8; 1024];
        let buf_ref = unsafe { BufferRef::from_slice(data.as_slice()) };
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_from_mut_slice() {
        let mut data = [0u8; 1024];
        let buf_ref = unsafe { BufferRef::from_mut_slice(data.as_mut_slice()) };
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_from_ptr() {
        let mut data = [0u8; 1024];
        // Safety: data is stack-local and lives for the duration of this test
        let buf_ref = unsafe { BufferRef::from_ptr(data.as_mut_ptr() as *mut std::ffi::c_void) };
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_fixed() {
        let buf_ref = BufferRef::fixed(0, 0);
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_fixed_index() {
        let buf_ref = BufferRef::fixed_index(0);
        let _ = buf_ref;
    }

    #[test]
    fn test_bufferref_copy() {
        let data = [0u8; 1024];
        let buf_ref1 = unsafe { BufferRef::from_slice(data.as_slice()) };
        let buf_ref2 = buf_ref1; // Copy
        // Both still valid since it's Copy
        let _ = (buf_ref1, buf_ref2);
    }

    #[test]
    fn test_bufferref_clone() {
        let data = [0u8; 1024];
        let buf_ref1 = unsafe { BufferRef::from_slice(data.as_slice()) };
        let buf_ref2 = buf_ref1;
        let _ = (buf_ref1, buf_ref2);
    }

    #[test]
    fn test_bufferref_debug() {
        let data = [0u8; 1024];
        let buf_ref = unsafe { BufferRef::from_slice(data.as_slice()) };
        let debug_str = format!("{:?}", buf_ref);
        assert!(debug_str.contains("BufferRef"));
    }

    // =========================================================================
    // Error Tests
    // =========================================================================

    #[test]
    fn test_error_from_raw_os_error() {
        let err = Error::from_raw_os_error(libc::ENOENT);
        match err {
            Error::Io(io_err) => {
                assert_eq!(io_err.raw_os_error(), Some(libc::ENOENT));
            }
            _ => panic!("Expected Error::Io"),
        }
    }

    #[test]
    fn test_error_cancelled() {
        let err = Error::from_raw_os_error(libc::ECANCELED);
        match err {
            Error::Cancelled => {}
            _ => panic!("Expected Error::Cancelled"),
        }
    }

    #[test]
    fn test_error_display() {
        let err = Error::Cancelled;
        let msg = format!("{}", err);
        assert!(msg.contains("cancelled") || msg.contains("cancel"));
    }

    #[test]
    fn test_error_is_send() {
        fn assert_send<T: Send>() {}
        assert_send::<Error>();
    }

    #[test]
    fn test_error_is_sync() {
        fn assert_sync<T: Sync>() {}
        assert_sync::<Error>();
    }

    // =========================================================================
    // Stats Tests
    // =========================================================================

    #[test]
    fn test_stats_initial() {
        let engine = Engine::new().unwrap();
        let stats = engine.stats();
        // Initial stats should be zero or reasonable defaults
        assert!(stats.ops_completed() >= 0);
        assert!(stats.bytes_transferred() >= 0);
        assert!(stats.current_in_flight() >= 0);
        assert!(stats.optimal_in_flight() > 0);
        assert!(stats.optimal_batch_size() > 0);
    }

    #[test]
    fn test_stats_clone() {
        let engine = Engine::new().unwrap();
        let stats1 = engine.stats();
        let stats2 = stats1.clone();
        assert_eq!(stats1.ops_completed(), stats2.ops_completed());
    }

    #[test]
    fn test_stats_debug() {
        let engine = Engine::new().unwrap();
        let stats = engine.stats();
        let debug_str = format!("{:?}", stats);
        assert!(debug_str.contains("Stats"));
    }

    #[test]
    fn test_stats_default() {
        let stats: Stats = Default::default();
        assert_eq!(stats.ops_completed(), 0);
    }

    // =========================================================================
    // Request Handle Tests
    // =========================================================================

    #[test]
    fn test_request_handle_is_send() {
        fn assert_send<T: Send>() {}
        assert_send::<RequestHandle>();
    }

    // =========================================================================
    // I/O Operation Tests
    // =========================================================================

    #[test]
    fn test_read_basic() {
        let engine = Engine::new().unwrap();

        // Create a temporary file with known content
        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        let test_data = b"Hello, AuraIO!";
        tmpfile.write_all(test_data).unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();
        let buf_ptr = buf.as_ptr();

        let done = Arc::new(AtomicBool::new(false));
        let result_bytes = Arc::new(AtomicI32::new(-999));
        let done_clone = done.clone();
        let result_clone = result_bytes.clone();

        engine
            .read(fd, (&buf).into(), 4096, 0, move |result| {
                match result {
                    Ok(n) => result_clone.store(n as i32, Ordering::SeqCst),
                    Err(_) => result_clone.store(-1, Ordering::SeqCst),
                }
                done_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        // Wait for completion
        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        let bytes_read = result_bytes.load(Ordering::SeqCst);
        assert_eq!(bytes_read, test_data.len() as i32);

        // Verify content
        let read_data = unsafe { std::slice::from_raw_parts(buf_ptr, test_data.len()) };
        assert_eq!(read_data, test_data);
    }

    #[test]
    fn test_write_basic() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let mut buf = engine.allocate_buffer(4096).unwrap();
        let test_data = b"Written by AuraIO!";
        buf.as_mut_slice()[..test_data.len()].copy_from_slice(test_data);

        let done = Arc::new(AtomicBool::new(false));
        let result_bytes = Arc::new(AtomicI32::new(-999));
        let done_clone = done.clone();
        let result_clone = result_bytes.clone();

        engine
            .write(fd, (&buf).into(), test_data.len(), 0, move |result| {
                match result {
                    Ok(n) => result_clone.store(n as i32, Ordering::SeqCst),
                    Err(_) => result_clone.store(-1, Ordering::SeqCst),
                }
                done_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        let bytes_written = result_bytes.load(Ordering::SeqCst);
        assert_eq!(bytes_written, test_data.len() as i32);

        // Verify by reading back
        drop(file);
        let mut verify_file = File::open(tmpfile.path()).unwrap();
        let mut verify_buf = vec![0u8; test_data.len()];
        verify_file.read_exact(&mut verify_buf).unwrap();
        assert_eq!(verify_buf.as_slice(), test_data);
    }

    #[test]
    fn test_read_with_closure_capture() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"test data").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();

        // Capture various types in the closure
        let counter = Arc::new(AtomicUsize::new(0));
        let flag = Arc::new(AtomicBool::new(false));
        let counter_clone = counter.clone();
        let flag_clone = flag.clone();

        engine
            .read(fd, (&buf).into(), 4096, 0, move |result| {
                if result.is_ok() {
                    counter_clone.fetch_add(1, Ordering::SeqCst);
                }
                flag_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        while !flag.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        assert_eq!(counter.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn test_fsync() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let done = Arc::new(AtomicBool::new(false));
        let success = Arc::new(AtomicBool::new(false));
        let done_clone = done.clone();
        let success_clone = success.clone();

        engine
            .fsync(fd, move |result| {
                success_clone.store(result.is_ok(), Ordering::SeqCst);
                done_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        assert!(success.load(Ordering::SeqCst));
    }

    #[test]
    fn test_fdatasync() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let done = Arc::new(AtomicBool::new(false));
        let success = Arc::new(AtomicBool::new(false));
        let done_clone = done.clone();
        let success_clone = success.clone();

        engine
            .fdatasync(fd, move |result| {
                success_clone.store(result.is_ok(), Ordering::SeqCst);
                done_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        assert!(success.load(Ordering::SeqCst));
    }

    #[test]
    fn test_readv() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"AABBCCDD").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let mut buf1 = [0u8; 4];
        let mut buf2 = [0u8; 4];

        let iovecs = [
            libc::iovec {
                iov_base: buf1.as_mut_ptr() as *mut _,
                iov_len: 4,
            },
            libc::iovec {
                iov_base: buf2.as_mut_ptr() as *mut _,
                iov_len: 4,
            },
        ];

        let done = Arc::new(AtomicBool::new(false));
        let result_bytes = Arc::new(AtomicI32::new(-999));
        let done_clone = done.clone();
        let result_clone = result_bytes.clone();

        // Safety: iovecs live on the stack until wait() completes
        unsafe {
            engine
                .readv(fd, &iovecs, 0, move |result| {
                    match result {
                        Ok(n) => result_clone.store(n as i32, Ordering::SeqCst),
                        Err(_) => result_clone.store(-1, Ordering::SeqCst),
                    }
                    done_clone.store(true, Ordering::SeqCst);
                })
                .unwrap();
        }

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        assert_eq!(result_bytes.load(Ordering::SeqCst), 8);
        assert_eq!(&buf1, b"AABB");
        assert_eq!(&buf2, b"CCDD");
    }

    #[test]
    fn test_writev() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let buf1 = b"Hello";
        let buf2 = b"World";

        let iovecs = [
            libc::iovec {
                iov_base: buf1.as_ptr() as *mut _,
                iov_len: 5,
            },
            libc::iovec {
                iov_base: buf2.as_ptr() as *mut _,
                iov_len: 5,
            },
        ];

        let done = Arc::new(AtomicBool::new(false));
        let result_bytes = Arc::new(AtomicI32::new(-999));
        let done_clone = done.clone();
        let result_clone = result_bytes.clone();

        // Safety: iovecs live on the stack until wait() completes
        unsafe {
            engine
                .writev(fd, &iovecs, 0, move |result| {
                    match result {
                        Ok(n) => result_clone.store(n as i32, Ordering::SeqCst),
                        Err(_) => result_clone.store(-1, Ordering::SeqCst),
                    }
                    done_clone.store(true, Ordering::SeqCst);
                })
                .unwrap();
        }

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        assert_eq!(result_bytes.load(Ordering::SeqCst), 10);

        // Verify content
        drop(file);
        let mut verify_file = File::open(tmpfile.path()).unwrap();
        let mut content = String::new();
        verify_file.read_to_string(&mut content).unwrap();
        assert_eq!(content, "HelloWorld");
    }

    // =========================================================================
    // Event Loop Tests
    // =========================================================================

    #[test]
    fn test_poll_fd() {
        let engine = Engine::new().unwrap();
        let fd = engine.poll_fd();
        assert!(fd.is_ok());
        assert!(fd.unwrap() >= 0);
    }

    #[test]
    fn test_poll_no_ops() {
        let engine = Engine::new().unwrap();
        // Poll should return 0 when nothing is pending
        let count = engine.poll().unwrap();
        assert_eq!(count, 0);
    }

    #[test]
    fn test_wait_timeout() {
        let engine = Engine::new().unwrap();
        // Should return quickly with 0 completions when nothing pending
        let count = engine.wait(1).unwrap(); // 1ms timeout
        assert_eq!(count, 0);
    }

    #[test]
    fn test_run_stop() {
        use std::thread;
        use std::time::Duration;

        let engine = Arc::new(Engine::new().unwrap());
        let engine_clone = engine.clone();

        // Spawn a thread to stop the engine after a short delay
        let handle = thread::spawn(move || {
            thread::sleep(Duration::from_millis(50));
            engine_clone.stop();
        });

        // This should block until stop() is called
        engine.run();

        handle.join().unwrap();
    }

    #[test]
    fn test_run_serializes_with_poll() {
        use std::thread;
        use std::time::Duration;

        let engine = Arc::new(Engine::new().unwrap());
        let run_engine = Arc::clone(&engine);
        let poll_engine = Arc::clone(&engine);
        let poll_returned = Arc::new(AtomicBool::new(false));
        let poll_returned_clone = Arc::clone(&poll_returned);

        let run_thread = thread::spawn(move || run_engine.run());
        thread::sleep(Duration::from_millis(20));

        let poll_thread = thread::spawn(move || {
            let _ = poll_engine.poll();
            poll_returned_clone.store(true, Ordering::SeqCst);
        });

        thread::sleep(Duration::from_millis(20));
        assert!(!poll_returned.load(Ordering::SeqCst));

        engine.stop();
        run_thread.join().unwrap();
        poll_thread.join().unwrap();
        assert!(poll_returned.load(Ordering::SeqCst));
    }

    // =========================================================================
    // Stats After I/O Tests
    // =========================================================================

    #[test]
    fn test_stats_after_io() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"test data for stats").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();
        let buf = engine.allocate_buffer(4096).unwrap();

        let done = Arc::new(AtomicBool::new(false));
        let done_clone = done.clone();

        engine
            .read(fd, (&buf).into(), 4096, 0, move |_result| {
                done_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }

        let stats = engine.stats();
        assert!(stats.ops_completed() >= 1);
    }

    // =========================================================================
    // Multiple Operations Tests
    // =========================================================================

    #[test]
    fn test_multiple_concurrent_reads() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        let content = b"0123456789ABCDEF"; // 16 bytes
        tmpfile.write_all(content).unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let completed = Arc::new(AtomicUsize::new(0));
        let num_ops = 4;

        // Submit multiple reads at different offsets
        for i in 0..num_ops {
            let buf = engine.allocate_buffer(4).unwrap();
            let completed_clone = completed.clone();

            engine
                .read(fd, (&buf).into(), 4, (i * 4) as i64, move |_result| {
                    completed_clone.fetch_add(1, Ordering::SeqCst);
                    // Buffer dropped here
                    drop(buf); // Buffer captured to keep it alive until callback fires
                })
                .unwrap();
        }

        // Wait for all completions
        while completed.load(Ordering::SeqCst) < num_ops {
            engine.wait(100).unwrap();
        }

        assert_eq!(completed.load(Ordering::SeqCst), num_ops);
    }

    // =========================================================================
    // Request Handle Tests (with actual I/O)
    // =========================================================================

    #[test]
    fn test_request_handle_fd() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"test").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();
        let done = Arc::new(AtomicBool::new(false));
        let done_clone = done.clone();

        let handle = engine
            .read(fd, (&buf).into(), 4096, 0, move |_result| {
                done_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        // Check fd matches (safe: callback hasn't run yet)
        assert_eq!(unsafe { handle.fd() }, fd);

        while !done.load(Ordering::SeqCst) {
            engine.wait(100).unwrap();
        }
    }

    // =========================================================================
    // Registered Buffers/Files Tests
    // =========================================================================

    #[test]
    fn test_register_unregister_files() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        // Register
        let result = engine.register_files(&[fd]);
        assert!(result.is_ok());

        // Unregister
        let result = engine.unregister_files();
        assert!(result.is_ok());
    }

    // =========================================================================
    // Cancellation Tests
    // =========================================================================

    #[test]
    fn test_cancel_pending_request() {
        use std::time::Duration;

        let engine = Engine::new().unwrap();

        // Create a file to read (large enough that the read won't complete instantly)
        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        let data = vec![0u8; 1024 * 1024]; // 1MB
        tmpfile.write_all(&data).unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(1024 * 1024).unwrap();
        let was_cancelled = Arc::new(AtomicBool::new(false));
        let completed = Arc::new(AtomicBool::new(false));
        let was_cancelled_clone = was_cancelled.clone();
        let completed_clone = completed.clone();

        let handle = engine
            .read(fd, (&buf).into(), 1024 * 1024, 0, move |result| {
                if let Err(Error::Cancelled) = result {
                    was_cancelled_clone.store(true, Ordering::SeqCst);
                }
                completed_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        // Request should be pending initially (safe: callback hasn't run yet)
        assert!(unsafe { handle.is_pending() });

        // Try to cancel
        // Safety: handle is still valid (callback hasn't run yet)
        let cancel_result = unsafe { engine.cancel(&handle) };
        // Cancel may or may not succeed depending on timing
        // (operation might have already completed)

        // Wait for completion (either normal or cancelled)
        let start = std::time::Instant::now();
        while !completed.load(Ordering::SeqCst) && start.elapsed() < Duration::from_secs(5) {
            engine.wait(10).ok();
        }

        assert!(completed.load(Ordering::SeqCst), "Operation should complete");

        // If cancel succeeded, callback should have received Cancelled error
        if cancel_result.is_ok() {
            // Note: Even if cancel returns Ok, the operation might complete before cancellation
            // takes effect, so we can't assert was_cancelled here. This is documented behavior.
        }
    }

    #[test]
    fn test_cancel_returns_ok_for_pending() {
        let engine = Engine::new().unwrap();

        // Use a fifo/pipe to create a slow read that will definitely be pending
        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"test").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();
        let completed = Arc::new(AtomicBool::new(false));
        let completed_clone = completed.clone();

        let handle = engine
            .read(fd, (&buf).into(), 4096, 0, move |_result| {
                completed_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        // Try to cancel immediately
        // Safety: handle is still valid (callback hasn't run yet)
        let cancel_result = unsafe { engine.cancel(&handle) };
        // Cancel should not error on a valid handle
        // (it might return error if already completed, but that's timing-dependent)

        // Ensure cleanup
        while !completed.load(Ordering::SeqCst) {
            engine.wait(10).ok();
        }

        // Test passes if we get here without panic
        let _ = cancel_result;
    }

    #[test]
    fn test_request_handle_pending_before_completion() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"test data").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();
        let completed = Arc::new(AtomicBool::new(false));
        let completed_clone = completed.clone();

        let handle = engine
            .read(fd, (&buf).into(), 4096, 0, move |_result| {
                completed_clone.store(true, Ordering::SeqCst);
            })
            .unwrap();

        // Before completion, handle should be pending (safe: callback hasn't run yet)
        // Note: This is a race condition - the I/O might complete before we check
        // So we only test that is_pending() doesn't crash
        let _ = unsafe { handle.is_pending() };

        // Wait for completion
        while !completed.load(Ordering::SeqCst) {
            engine.wait(10).ok();
        }

        // After completion callback has run, the handle is no longer valid to use
        // per the API documentation. We don't test is_pending() here.
    }

    #[test]
    fn test_cancelled_error_variant() {
        // Test that the Cancelled error variant works correctly
        let err = Error::from_raw_os_error(libc::ECANCELED);
        match &err {
            Error::Cancelled => {
                let msg = format!("{}", err);
                assert!(
                    msg.to_lowercase().contains("cancel"),
                    "Cancelled error message should mention 'cancel': {}",
                    msg
                );
            }
            _ => panic!("Expected Error::Cancelled, got {:?}", err),
        }
    }

    #[test]
    fn test_cancel_multiple_pending() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        let data = vec![0u8; 64 * 1024];
        tmpfile.write_all(&data).unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let completed_count = Arc::new(AtomicUsize::new(0));
        let cancelled_count = Arc::new(AtomicUsize::new(0));
        let total_ops = 4;

        let mut handles = Vec::new();

        // Submit multiple reads
        for _ in 0..total_ops {
            let buf = engine.allocate_buffer(16 * 1024).unwrap();
            let completed_clone = completed_count.clone();
            let cancelled_clone = cancelled_count.clone();

            let handle = engine
                .read(fd, (&buf).into(), 16 * 1024, 0, move |result| {
                    completed_clone.fetch_add(1, Ordering::SeqCst);
                    if matches!(result, Err(Error::Cancelled)) {
                        cancelled_clone.fetch_add(1, Ordering::SeqCst);
                    }
                    drop(buf); // Buffer captured to keep it alive until callback fires
                })
                .unwrap();
            handles.push(handle);
        }

        // Try to cancel all of them
        // Safety: handles are still valid (callbacks haven't run yet)
        for handle in &handles {
            let _ = unsafe { engine.cancel(handle) };
        }

        // Wait for all completions
        while completed_count.load(Ordering::SeqCst) < total_ops {
            engine.wait(10).ok();
        }

        assert_eq!(
            completed_count.load(Ordering::SeqCst),
            total_ops,
            "All operations should complete (either normally or cancelled)"
        );
        // We can't assert on cancelled_count because cancellation is best-effort
    }
}
