//! Async/await support for AuraIO
//!
//! This module provides Future-based async I/O operations that integrate with
//! any async runtime (tokio, async-std, smol, etc.).
//!
//! # Example
//!
//! ```ignore
//! use auraio::{Engine, async_io::AsyncEngine};
//! use std::fs::File;
//! use std::os::unix::io::AsRawFd;
//!
//! async fn read_file(engine: &Engine, fd: i32) -> auraio::Result<Vec<u8>> {
//!     let buf = engine.allocate_buffer(4096)?;
//!     let n = unsafe { engine.async_read(fd, &buf, 4096, 0) }?.await?;
//!     Ok(buf.as_slice()[..n].to_vec())
//! }
//! ```
//!
//! # Integration with Async Runtimes
//!
//! The futures returned by async methods need the engine to be polled for completions.
//! You can either:
//!
//! 1. **Spawn a background task** that calls `engine.wait()` in a loop
//! 2. **Integrate with your runtime's event loop** using `engine.poll_fd()`
//!
//! ```ignore
//! // Example: Background completion poller with tokio
//! tokio::spawn(async move {
//!     loop {
//!         engine.wait(10).ok();
//!         tokio::task::yield_now().await;
//!     }
//! });
//! ```

use crate::buffer::BufferRef;
use crate::engine::EngineInner;
use crate::error::Result;
use crate::Engine;
use std::future::Future;
use std::os::unix::io::RawFd;
use std::pin::Pin;
use std::sync::{Arc, Mutex};
use std::task::{Context, Poll, Waker};

/// Shared state between the callback and the future
struct IoState {
    /// The result from the I/O operation (set by callback)
    result: Option<Result<usize>>,
    /// The waker to notify when complete (set by future poll)
    waker: Option<Waker>,
}

/// A Future that completes when an async I/O operation finishes.
///
/// This future is created by methods like [`AsyncEngine::async_read`].
/// Poll this future while also ensuring the engine processes completions
/// (via `engine.wait()` or `engine.poll()`).
///
/// # Cancellation
///
/// Dropping an `IoFuture` will attempt to cancel the underlying kernel I/O
/// operation via `auraio_cancel()`. Cancellation is best-effort: the kernel
/// may have already completed the I/O. If cancelled successfully, the
/// callback fires with `-ECANCELED` and writes into the shared state
/// (which is kept alive by the callback's Arc clone).
///
/// Even with cancellation, callers using `select!` or similar patterns
/// should prefer keeping buffers alive until the engine is polled at
/// least once after cancellation, to ensure the kernel has observed
/// the cancel before the buffer memory is reused.
pub struct IoFuture {
    state: Arc<Mutex<IoState>>,
    engine: Arc<EngineInner>,
    request: *mut auraio_sys::auraio_request_t,
}

// Safety: IoFuture is Send because:
// - state is behind Arc<Mutex<>>, which is Send
// - engine is behind Arc, which is Send
// - request is a raw pointer only accessed for cancel (which is thread-safe in C API)
unsafe impl Send for IoFuture {}

impl Future for IoFuture {
    type Output = Result<usize>;

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let mut state = self.state.lock().unwrap_or_else(|e| e.into_inner());

        // Check if result is ready
        if let Some(result) = state.result.take() {
            return Poll::Ready(result);
        }

        // Store/update the waker
        state.waker = Some(cx.waker().clone());
        Poll::Pending
    }
}

impl Drop for IoFuture {
    fn drop(&mut self) {
        let state = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if state.result.is_none() {
            // I/O still in-flight — attempt best-effort cancellation.
            // This prevents the kernel from writing to freed buffers if
            // the future was dropped via select! or similar patterns.
            // The callback will still fire (with -ECANCELED or the real
            // result) and write into the Arc<Mutex<IoState>> harmlessly.
            unsafe {
                auraio_sys::auraio_cancel(self.engine.raw(), self.request);
            }
        }
    }
}

/// Creates a new IoFuture and its associated callback.
///
/// Returns (callback, state_arc) where the callback must be passed to the C API.
/// The IoFuture is constructed separately after the submission succeeds.
fn create_io_callback() -> (impl FnOnce(Result<usize>) + Send + 'static, Arc<Mutex<IoState>>) {
    let state = Arc::new(Mutex::new(IoState {
        result: None,
        waker: None,
    }));

    let callback_state = state.clone();
    let callback = move |result: Result<usize>| {
        let waker = {
            let mut state = callback_state.lock().unwrap_or_else(|e| e.into_inner());
            state.result = Some(result);
            state.waker.take()
        };
        // Wake outside the lock to avoid deadlock if the executor
        // re-enters poll() on the same thread.
        if let Some(waker) = waker {
            waker.wake();
        }
    };

    (callback, state)
}

/// Extension trait providing async I/O methods on [`Engine`].
///
/// This trait is automatically implemented for `Engine` when the `async` feature is enabled.
pub trait AsyncEngine {
    /// Submit an async read and return a Future for the result.
    ///
    /// # Arguments
    ///
    /// * `fd` - Open file descriptor
    /// * `buf` - Buffer reference to read into
    /// * `len` - Number of bytes to read
    /// * `offset` - File offset to read from
    ///
    /// # Returns
    ///
    /// A Future that resolves to `Ok(bytes_read)` or `Err(error)`.
    ///
    /// # Safety Contract
    ///
    /// The buffer must remain valid and exclusively borrowed until the
    /// Future resolves. Dropping the Future attempts to cancel the kernel
    /// I/O but cancellation is best-effort — see [`IoFuture`] docs.
    ///
    /// # Example
    ///
    /// ```ignore
    /// let n = unsafe { engine.async_read(fd, &buffer, 4096, 0) }?.await?;
    /// println!("Read {} bytes", n);
    /// ```
    unsafe fn async_read(
        &self,
        fd: RawFd,
        buf: impl Into<BufferRef>,
        len: usize,
        offset: i64,
    ) -> Result<IoFuture>;

    /// Submit an async write and return a Future for the result.
    ///
    /// # Arguments
    ///
    /// * `fd` - Open file descriptor
    /// * `buf` - Buffer reference containing data to write
    /// * `len` - Number of bytes to write
    /// * `offset` - File offset to write at
    ///
    /// # Returns
    ///
    /// A Future that resolves to `Ok(bytes_written)` or `Err(error)`.
    ///
    /// # Safety Contract
    ///
    /// The buffer must remain valid until the Future resolves. Dropping
    /// the Future attempts to cancel the kernel I/O — see [`IoFuture`].
    unsafe fn async_write(
        &self,
        fd: RawFd,
        buf: impl Into<BufferRef>,
        len: usize,
        offset: i64,
    ) -> Result<IoFuture>;

    /// Submit an async fsync and return a Future for the result.
    ///
    /// # Arguments
    ///
    /// * `fd` - Open file descriptor to sync
    ///
    /// # Returns
    ///
    /// A Future that resolves to `Ok(0)` or `Err(error)`.
    fn async_fsync(&self, fd: RawFd) -> Result<IoFuture>;

    /// Submit an async fdatasync and return a Future for the result.
    ///
    /// Like fsync but may skip metadata if not needed for data integrity.
    fn async_fdatasync(&self, fd: RawFd) -> Result<IoFuture>;

    /// Submit an async vectored read and return a Future.
    ///
    /// # Safety
    ///
    /// The iovec buffers must remain valid and exclusively borrowed
    /// until the returned Future completes.
    unsafe fn async_readv(
        &self,
        fd: RawFd,
        iovecs: &[libc::iovec],
        offset: i64,
    ) -> Result<IoFuture>;

    /// Submit an async vectored write and return a Future.
    ///
    /// # Safety
    ///
    /// The iovec buffers must remain valid until the returned Future completes.
    unsafe fn async_writev(
        &self,
        fd: RawFd,
        iovecs: &[libc::iovec],
        offset: i64,
    ) -> Result<IoFuture>;
}

impl AsyncEngine for Engine {
    unsafe fn async_read(
        &self,
        fd: RawFd,
        buf: impl Into<BufferRef>,
        len: usize,
        offset: i64,
    ) -> Result<IoFuture> {
        let (callback, state) = create_io_callback();
        let handle = unsafe { self.read(fd, buf.into(), len, offset, callback)? };
        Ok(IoFuture {
            state,
            engine: Arc::clone(&self.inner),
            request: handle.as_ptr(),
        })
    }

    unsafe fn async_write(
        &self,
        fd: RawFd,
        buf: impl Into<BufferRef>,
        len: usize,
        offset: i64,
    ) -> Result<IoFuture> {
        let (callback, state) = create_io_callback();
        let handle = unsafe { self.write(fd, buf.into(), len, offset, callback)? };
        Ok(IoFuture {
            state,
            engine: Arc::clone(&self.inner),
            request: handle.as_ptr(),
        })
    }

    fn async_fsync(&self, fd: RawFd) -> Result<IoFuture> {
        let (callback, state) = create_io_callback();
        let handle = self.fsync(fd, callback)?;
        Ok(IoFuture {
            state,
            engine: Arc::clone(&self.inner),
            request: handle.as_ptr(),
        })
    }

    fn async_fdatasync(&self, fd: RawFd) -> Result<IoFuture> {
        let (callback, state) = create_io_callback();
        let handle = self.fdatasync(fd, callback)?;
        Ok(IoFuture {
            state,
            engine: Arc::clone(&self.inner),
            request: handle.as_ptr(),
        })
    }

    unsafe fn async_readv(
        &self,
        fd: RawFd,
        iovecs: &[libc::iovec],
        offset: i64,
    ) -> Result<IoFuture> {
        let (callback, state) = create_io_callback();
        let handle = unsafe { self.readv(fd, iovecs, offset, callback)? };
        Ok(IoFuture {
            state,
            engine: Arc::clone(&self.inner),
            request: handle.as_ptr(),
        })
    }

    unsafe fn async_writev(
        &self,
        fd: RawFd,
        iovecs: &[libc::iovec],
        offset: i64,
    ) -> Result<IoFuture> {
        let (callback, state) = create_io_callback();
        let handle = unsafe { self.writev(fd, iovecs, offset, callback)? };
        Ok(IoFuture {
            state,
            engine: Arc::clone(&self.inner),
            request: handle.as_ptr(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::{File, OpenOptions};
    use std::io::{Read as _, Write as _};
    use std::os::unix::io::AsRawFd;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::thread;
    use std::time::Duration;

    // A minimal executor for testing - just polls until complete
    fn block_on<F: Future>(engine: &Engine, future: F) -> F::Output {
        let waker = std::task::Waker::noop();
        let mut cx = Context::from_waker(&waker);

        let mut future = std::pin::pin!(future);

        loop {
            match future.as_mut().poll(&mut cx) {
                Poll::Ready(result) => return result,
                Poll::Pending => {
                    // Poll the engine for completions
                    engine.wait(1).ok();
                }
            }
        }
    }

    #[test]
    fn test_async_read() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        let test_data = b"Hello async AuraIO!";
        tmpfile.write_all(test_data).unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();
        let future = unsafe { engine.async_read(fd, &buf, 4096, 0) }.unwrap();

        let result = block_on(&engine, future);
        assert!(result.is_ok());
        assert_eq!(result.unwrap(), test_data.len());
        assert_eq!(&buf.as_slice()[..test_data.len()], test_data);
    }

    #[test]
    fn test_async_write() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let mut buf = engine.allocate_buffer(4096).unwrap();
        let test_data = b"Written async!";
        buf.as_mut_slice()[..test_data.len()].copy_from_slice(test_data);

        let future = unsafe { engine.async_write(fd, &buf, test_data.len(), 0) }.unwrap();

        let result = block_on(&engine, future);
        assert!(result.is_ok());
        assert_eq!(result.unwrap(), test_data.len());

        // Verify
        drop(file);
        let mut verify_file = File::open(tmpfile.path()).unwrap();
        let mut content = vec![0u8; test_data.len()];
        verify_file.read_exact(&mut content).unwrap();
        assert_eq!(content.as_slice(), test_data);
    }

    #[test]
    fn test_async_fsync() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let future = engine.async_fsync(fd).unwrap();
        let result = block_on(&engine, future);
        assert!(result.is_ok());
    }

    #[test]
    fn test_async_fdatasync() {
        let engine = Engine::new().unwrap();

        let tmpfile = tempfile::NamedTempFile::new().unwrap();
        let file = OpenOptions::new()
            .write(true)
            .open(tmpfile.path())
            .unwrap();
        let fd = file.as_raw_fd();

        let future = engine.async_fdatasync(fd).unwrap();
        let result = block_on(&engine, future);
        assert!(result.is_ok());
    }

    #[test]
    fn test_async_readv() {
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

        // Safety: iovecs point to stack-local buffers that outlive the future
        let future = unsafe { engine.async_readv(fd, &iovecs, 0) }.unwrap();
        let result = block_on(&engine, future);

        assert!(result.is_ok());
        assert_eq!(result.unwrap(), 8);
        assert_eq!(&buf1, b"AABB");
        assert_eq!(&buf2, b"CCDD");
    }

    #[test]
    fn test_async_writev() {
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

        // Safety: iovecs point to stack-local buffers that outlive the future
        let future = unsafe { engine.async_writev(fd, &iovecs, 0) }.unwrap();
        let result = block_on(&engine, future);

        assert!(result.is_ok());
        assert_eq!(result.unwrap(), 10);

        // Verify
        drop(file);
        let mut verify_file = File::open(tmpfile.path()).unwrap();
        let mut content = String::new();
        verify_file.read_to_string(&mut content).unwrap();
        assert_eq!(content, "HelloWorld");
    }

    #[test]
    fn test_multiple_concurrent_async_ops() {
        let engine = Engine::new().unwrap();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        let content = b"0123456789ABCDEF";
        tmpfile.write_all(content).unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        // Submit multiple async reads
        let buf1 = engine.allocate_buffer(4).unwrap();
        let buf2 = engine.allocate_buffer(4).unwrap();

        let future1 = unsafe { engine.async_read(fd, &buf1, 4, 0) }.unwrap();
        let future2 = unsafe { engine.async_read(fd, &buf2, 4, 4) }.unwrap();

        // Poll both to completion
        let result1 = block_on(&engine, future1);
        let result2 = block_on(&engine, future2);

        assert!(result1.is_ok());
        assert!(result2.is_ok());
        assert_eq!(buf1.as_slice(), b"0123");
        assert_eq!(buf2.as_slice(), b"4567");
    }

    #[test]
    fn test_io_future_is_send() {
        fn assert_send<T: Send>() {}
        assert_send::<IoFuture>();
    }

    #[test]
    fn test_async_with_background_poller() {
        // Test pattern: background thread polls engine while main thread awaits
        let engine = Arc::new(Engine::new().unwrap());
        let engine_poller = engine.clone();

        let mut tmpfile = tempfile::NamedTempFile::new().unwrap();
        tmpfile.write_all(b"background poller test").unwrap();
        tmpfile.flush().unwrap();

        let file = File::open(tmpfile.path()).unwrap();
        let fd = file.as_raw_fd();

        let buf = engine.allocate_buffer(4096).unwrap();
        let future = unsafe { engine.async_read(fd, &buf, 4096, 0) }.unwrap();

        // Start background poller
        let stop = Arc::new(AtomicBool::new(false));
        let stop_clone = stop.clone();
        let poller_handle = thread::spawn(move || {
            while !stop_clone.load(Ordering::Relaxed) {
                engine_poller.wait(10).ok();
            }
        });

        // Wait for result using a simple spin
        let waker = std::task::Waker::noop();
        let mut cx = Context::from_waker(&waker);
        let mut pinned = std::pin::pin!(future);

        loop {
            match pinned.as_mut().poll(&mut cx) {
                Poll::Ready(result) => {
                    assert!(result.is_ok());
                    break;
                }
                Poll::Pending => {
                    thread::sleep(Duration::from_micros(100));
                }
            }
        }

        // Stop poller
        stop.store(true, Ordering::Relaxed);
        poller_handle.join().unwrap();
    }
}
