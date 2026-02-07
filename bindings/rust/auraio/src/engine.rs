//! Core Engine type for AuraIO

use crate::buffer::{Buffer, BufferRef};
use crate::callback::{callback_trampoline, CallbackContext};
use crate::error::{Error, Result};
use crate::options::Options;
use crate::request::RequestHandle;
use crate::stats::Stats;

use auraio_sys;
use std::io;
use std::os::unix::io::RawFd;
use std::ptr::NonNull;

/// Main AuraIO engine
///
/// Manages io_uring rings and provides async I/O operations.
/// The engine automatically tunes itself for optimal performance.
///
/// # Thread Safety
///
/// The engine is thread-safe for submissions from multiple threads.
/// Each thread will preferentially use the ring associated with its CPU core.
///
/// # Example
///
/// ```no_run
/// use auraio::{Engine, BufferRef};
/// use std::fs::File;
/// use std::os::unix::io::AsRawFd;
///
/// # fn main() -> Result<(), Box<dyn std::error::Error>> {
/// let engine = Engine::new()?;
/// let mut buf = engine.allocate_buffer(4096)?;
/// let file = File::open("/etc/hostname")?;
///
/// engine.read(file.as_raw_fd(), (&buf).into(), 4096, 0, |result| {
///     match result {
///         Ok(n) => println!("Read {} bytes", n),
///         Err(e) => eprintln!("Error: {}", e),
///     }
/// })?;
///
/// engine.wait(-1)?;
/// # Ok(())
/// # }
/// ```
pub struct Engine {
    handle: NonNull<auraio_sys::auraio_engine_t>,
}

// Safety: Engine can be sent between threads
// (the underlying library handles synchronization)
unsafe impl Send for Engine {}

// Safety: Engine can be shared between threads
// (all operations are internally synchronized)
unsafe impl Sync for Engine {}

impl Engine {
    /// Create a new engine with default options
    pub fn new() -> Result<Self> {
        let handle = unsafe { auraio_sys::auraio_create() };
        NonNull::new(handle)
            .map(|h| Self { handle: h })
            .ok_or_else(|| Error::EngineCreate(io::Error::last_os_error()))
    }

    /// Create a new engine with custom options
    pub fn with_options(options: &Options) -> Result<Self> {
        let handle = unsafe { auraio_sys::auraio_create_with_options(options.as_ptr()) };
        NonNull::new(handle)
            .map(|h| Self { handle: h })
            .ok_or_else(|| Error::EngineCreate(io::Error::last_os_error()))
    }

    /// Get the raw engine handle (for advanced use)
    pub fn as_ptr(&self) -> *mut auraio_sys::auraio_engine_t {
        self.handle.as_ptr()
    }

    // =========================================================================
    // Buffer Management
    // =========================================================================

    /// Allocate an aligned buffer from the engine's pool
    ///
    /// Returns page-aligned memory suitable for O_DIRECT I/O.
    pub fn allocate_buffer(&self, size: usize) -> Result<Buffer> {
        let ptr = unsafe { auraio_sys::auraio_buffer_alloc(self.handle.as_ptr(), size) };
        NonNull::new(ptr as *mut u8)
            .map(|p| Buffer::new(self.handle.as_ptr(), p, size))
            .ok_or_else(|| Error::BufferAlloc(io::Error::last_os_error()))
    }

    /// Register buffers with the kernel for zero-copy I/O
    ///
    /// After registration, use `BufferRef::fixed()` to reference buffers by index.
    ///
    /// # Safety
    ///
    /// The registered buffers must remain valid and at stable addresses
    /// until `unregister_buffers()` is called. The borrow checker cannot
    /// enforce this lifetime across the registration boundary.
    pub unsafe fn register_buffers(&self, buffers: &[&[u8]]) -> Result<()> {
        let iovecs: Vec<libc::iovec> = buffers
            .iter()
            .map(|b| libc::iovec {
                iov_base: b.as_ptr() as *mut _,
                iov_len: b.len(),
            })
            .collect();

        let ret = unsafe {
            auraio_sys::auraio_register_buffers(
                self.handle.as_ptr(),
                iovecs.as_ptr() as *const _,
                iovecs.len() as i32,
            )
        };

        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Unregister previously registered buffers
    pub fn unregister_buffers(&self) -> Result<()> {
        let ret = unsafe { auraio_sys::auraio_unregister_buffers(self.handle.as_ptr()) };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Register file descriptors with the kernel
    pub fn register_files(&self, fds: &[RawFd]) -> Result<()> {
        let ret = unsafe {
            auraio_sys::auraio_register_files(
                self.handle.as_ptr(),
                fds.as_ptr(),
                fds.len() as i32,
            )
        };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Update a registered file descriptor
    pub fn update_file(&self, index: i32, fd: RawFd) -> Result<()> {
        let ret =
            unsafe { auraio_sys::auraio_update_file(self.handle.as_ptr(), index, fd) };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Unregister previously registered files
    pub fn unregister_files(&self) -> Result<()> {
        let ret = unsafe { auraio_sys::auraio_unregister_files(self.handle.as_ptr()) };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    // =========================================================================
    // Core I/O Operations
    // =========================================================================

    /// Submit an async read operation
    ///
    /// The callback is invoked when the read completes with either:
    /// - `Ok(bytes_read)` on success
    /// - `Err(error)` on failure or cancellation
    ///
    /// # Arguments
    ///
    /// * `fd` - Open file descriptor
    /// * `buf` - Buffer reference (use `BufferRef::from()` or `(&buffer).into()`)
    /// * `len` - Number of bytes to read
    /// * `offset` - File offset to read from
    /// * `callback` - Completion callback
    ///
    /// # Example
    ///
    /// ```no_run
    /// # use auraio::Engine;
    /// # use std::fs::File;
    /// # use std::os::unix::io::AsRawFd;
    /// # fn main() -> Result<(), Box<dyn std::error::Error>> {
    /// # let engine = Engine::new()?;
    /// # let buf = engine.allocate_buffer(4096)?;
    /// # let file = File::open("/etc/hostname")?;
    /// # let fd = file.as_raw_fd();
    /// engine.read(fd, (&buf).into(), 4096, 0, |result| {
    ///     println!("Read result: {:?}", result);
    /// })?;
    /// # Ok(())
    /// # }
    /// ```
    pub fn read<F>(
        &self,
        fd: RawFd,
        buf: BufferRef,
        len: usize,
        offset: i64,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = unsafe {
            auraio_sys::auraio_read(
                self.handle.as_ptr(),
                fd,
                buf.as_raw(),
                len,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        };

        if req.is_null() {
            // Free the context on error
            unsafe {
                drop(Box::from_raw(ctx_ptr as *mut CallbackContext));
            }
            Err(Error::Submission(io::Error::last_os_error()))
        } else {
            Ok(RequestHandle::new(req))
        }
    }

    /// Submit an async write operation
    ///
    /// The callback is invoked when the write completes.
    pub fn write<F>(
        &self,
        fd: RawFd,
        buf: BufferRef,
        len: usize,
        offset: i64,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = unsafe {
            auraio_sys::auraio_write(
                self.handle.as_ptr(),
                fd,
                buf.as_raw(),
                len,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        };

        if req.is_null() {
            unsafe {
                drop(Box::from_raw(ctx_ptr as *mut CallbackContext));
            }
            Err(Error::Submission(io::Error::last_os_error()))
        } else {
            Ok(RequestHandle::new(req))
        }
    }

    /// Submit an async fsync operation
    ///
    /// Ensures all previous writes to the file descriptor are flushed to storage.
    pub fn fsync<F>(&self, fd: RawFd, callback: F) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = unsafe {
            auraio_sys::auraio_fsync(
                self.handle.as_ptr(),
                fd,
                Some(callback_trampoline),
                ctx_ptr,
            )
        };

        if req.is_null() {
            unsafe {
                drop(Box::from_raw(ctx_ptr as *mut CallbackContext));
            }
            Err(Error::Submission(io::Error::last_os_error()))
        } else {
            Ok(RequestHandle::new(req))
        }
    }

    /// Submit an async fdatasync operation
    ///
    /// Like fsync, but may skip metadata if not needed for data integrity.
    pub fn fdatasync<F>(&self, fd: RawFd, callback: F) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = unsafe {
            auraio_sys::auraio_fsync_ex(
                self.handle.as_ptr(),
                fd,
                auraio_sys::auraio_fsync_flags_t_AURAIO_FSYNC_DATASYNC,
                Some(callback_trampoline),
                ctx_ptr,
            )
        };

        if req.is_null() {
            unsafe {
                drop(Box::from_raw(ctx_ptr as *mut CallbackContext));
            }
            Err(Error::Submission(io::Error::last_os_error()))
        } else {
            Ok(RequestHandle::new(req))
        }
    }

    /// Submit an async vectored read operation
    ///
    /// # Safety
    ///
    /// The caller must ensure the iovec buffers remain valid and
    /// exclusively borrowed until the completion callback fires.
    /// The borrow checker cannot enforce this across the async boundary.
    pub unsafe fn readv<F>(
        &self,
        fd: RawFd,
        iovecs: &[libc::iovec],
        offset: i64,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = unsafe {
            auraio_sys::auraio_readv(
                self.handle.as_ptr(),
                fd,
                iovecs.as_ptr() as *const _,
                iovecs.len() as i32,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        };

        if req.is_null() {
            unsafe {
                drop(Box::from_raw(ctx_ptr as *mut CallbackContext));
            }
            Err(Error::Submission(io::Error::last_os_error()))
        } else {
            Ok(RequestHandle::new(req))
        }
    }

    /// Submit an async vectored write operation
    ///
    /// # Safety
    ///
    /// The caller must ensure the iovec buffers remain valid and
    /// exclusively borrowed until the completion callback fires.
    /// The borrow checker cannot enforce this across the async boundary.
    pub unsafe fn writev<F>(
        &self,
        fd: RawFd,
        iovecs: &[libc::iovec],
        offset: i64,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = unsafe {
            auraio_sys::auraio_writev(
                self.handle.as_ptr(),
                fd,
                iovecs.as_ptr() as *const _,
                iovecs.len() as i32,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        };

        if req.is_null() {
            unsafe {
                drop(Box::from_raw(ctx_ptr as *mut CallbackContext));
            }
            Err(Error::Submission(io::Error::last_os_error()))
        } else {
            Ok(RequestHandle::new(req))
        }
    }

    // =========================================================================
    // Cancellation
    // =========================================================================

    /// Cancel a pending I/O operation
    ///
    /// If successful, the request's callback will be invoked with `Err(Cancelled)`.
    /// Cancellation is best-effort: the operation may complete normally instead.
    ///
    /// Returns `Ok(())` if cancellation was submitted, `Err` on failure.
    ///
    /// # Safety
    ///
    /// The caller must ensure the `RequestHandle` still refers to a valid,
    /// in-flight request. If the request has already completed, the handle
    /// may point to freed or reused memory.
    pub unsafe fn cancel(&self, request: &RequestHandle) -> Result<()> {
        let ret =
            unsafe { auraio_sys::auraio_cancel(self.handle.as_ptr(), request.as_ptr()) };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    // =========================================================================
    // Event Processing
    // =========================================================================

    /// Get a pollable file descriptor for event loop integration
    ///
    /// When this fd is readable, call `poll()` to process completions.
    pub fn poll_fd(&self) -> Result<RawFd> {
        let fd = unsafe { auraio_sys::auraio_get_poll_fd(self.handle.as_ptr()) };
        if fd >= 0 {
            Ok(fd)
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Process completed operations (non-blocking)
    ///
    /// Returns the number of completions processed.
    pub fn poll(&self) -> usize {
        let n = unsafe { auraio_sys::auraio_poll(self.handle.as_ptr()) };
        n.max(0) as usize
    }

    /// Wait for completions
    ///
    /// Blocks until at least one operation completes or timeout expires.
    ///
    /// # Arguments
    ///
    /// * `timeout_ms` - Maximum wait time in milliseconds
    ///   - `-1` = wait forever
    ///   - `0` = don't block (same as `poll()`)
    ///   - `>0` = wait up to N milliseconds
    ///
    /// Returns the number of completions processed.
    pub fn wait(&self, timeout_ms: i32) -> Result<usize> {
        let n = unsafe { auraio_sys::auraio_wait(self.handle.as_ptr(), timeout_ms) };
        if n >= 0 {
            Ok(n as usize)
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Run the event loop until `stop()` is called
    ///
    /// Blocks the calling thread, continuously processing completions.
    /// Call `stop()` from a callback or another thread to exit.
    pub fn run(&self) {
        unsafe { auraio_sys::auraio_run(self.handle.as_ptr()) };
    }

    /// Signal the event loop to stop
    ///
    /// Safe to call from any thread, including from within a callback.
    pub fn stop(&self) {
        unsafe { auraio_sys::auraio_stop(self.handle.as_ptr()) };
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    /// Get current engine statistics
    pub fn stats(&self) -> Stats {
        let mut inner: auraio_sys::auraio_stats_t = unsafe { std::mem::zeroed() };
        unsafe { auraio_sys::auraio_get_stats(self.handle.as_ptr(), &mut inner) };
        Stats::new(inner)
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        unsafe { auraio_sys::auraio_destroy(self.handle.as_ptr()) };
    }
}

/// Get the library version string
pub fn version() -> &'static str {
    unsafe {
        let ptr = auraio_sys::auraio_version();
        if ptr.is_null() {
            return "unknown";
        }
        std::ffi::CStr::from_ptr(ptr)
            .to_str()
            .unwrap_or("unknown")
    }
}

/// Get the library version as an integer
///
/// Format: (major * 10000 + minor * 100 + patch)
pub fn version_int() -> i32 {
    unsafe { auraio_sys::auraio_version_int() }
}
