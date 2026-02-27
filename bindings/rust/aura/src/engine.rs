// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Core Engine type for AuraIO

use crate::buffer::{Buffer, BufferRef};
use crate::callback::{callback_trampoline, drop_context, CallbackContext};
use crate::error::{Error, Result};
use crate::options::Options;
use crate::request::RequestHandle;
use crate::stats::Stats;

use std::io;
use std::os::unix::io::RawFd;
use std::ptr::NonNull;
use std::sync::{Arc, Mutex};

pub(crate) struct EngineInner {
    handle: NonNull<aura_sys::aura_engine_t>,
}

// Safety: The underlying aura_engine_t is thread-safe for submissions
// from multiple threads. The C library uses internal locking (per-ring
// mutexes) to synchronize access. Polling/waiting is guarded by
// Engine::poll_lock at the Rust level.
unsafe impl Send for EngineInner {}
unsafe impl Sync for EngineInner {}

impl EngineInner {
    #[inline]
    pub(crate) fn raw(&self) -> *mut aura_sys::aura_engine_t {
        self.handle.as_ptr()
    }
}

impl Drop for EngineInner {
    fn drop(&mut self) {
        unsafe { aura_sys::aura_destroy(self.handle.as_ptr()) };
    }
}

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
/// use aura::{Engine, BufferRef};
/// use std::fs::File;
/// use std::os::unix::io::AsRawFd;
///
/// # fn main() -> Result<(), Box<dyn std::error::Error>> {
/// let engine = Engine::new()?;
/// let mut buf = engine.allocate_buffer(4096)?;
/// let file = File::open("/etc/hostname")?;
///
/// unsafe { engine.read(file.as_raw_fd(), (&buf).into(), 4096, 0, |result| {
///     match result {
///         Ok(n) => println!("Read {} bytes", n),
///         Err(e) => eprintln!("Error: {}", e),
///     }
/// }) }?;
///
/// engine.wait(-1)?;
/// # Ok(())
/// # }
/// ```
pub struct Engine {
    pub(crate) inner: Arc<EngineInner>,
    /// Guards poll/wait/run which must not be called concurrently.
    /// Submissions (read/write/fsync etc.) do NOT acquire this lock.
    poll_lock: Mutex<()>,
}

// Safety: Engine can be sent between threads
// (the underlying C library handles synchronization for submissions)
unsafe impl Send for Engine {}

// Safety: Engine can be shared between threads for submissions.
// Polling (poll/wait/run) is guarded by poll_lock to prevent concurrent
// CQ access, which the C library does not internally synchronize.
unsafe impl Sync for Engine {}

impl Engine {
    /// Create a new engine with default options
    pub fn new() -> Result<Self> {
        let handle = unsafe { aura_sys::aura_create() };
        // Capture errno immediately before any other operation can clobber it
        let err = io::Error::last_os_error();
        NonNull::new(handle)
            .map(|h| Self {
                inner: Arc::new(EngineInner { handle: h }),
                poll_lock: Mutex::new(()),
            })
            .ok_or(Error::EngineCreate(err))
    }

    /// Create a new engine with custom options
    pub fn with_options(options: &Options) -> Result<Self> {
        let handle = unsafe { aura_sys::aura_create_with_options(options.as_ptr()) };
        let err = io::Error::last_os_error();
        NonNull::new(handle)
            .map(|h| Self {
                inner: Arc::new(EngineInner { handle: h }),
                poll_lock: Mutex::new(()),
            })
            .ok_or(Error::EngineCreate(err))
    }

    /// Get the raw engine handle (for advanced use)
    pub fn as_ptr(&self) -> *mut aura_sys::aura_engine_t {
        self.inner.raw()
    }

    // =========================================================================
    // Buffer Management
    // =========================================================================

    /// Allocate an aligned buffer from the engine's pool
    ///
    /// Returns page-aligned memory suitable for O_DIRECT I/O.
    pub fn allocate_buffer(&self, size: usize) -> Result<Buffer> {
        let ptr = unsafe { aura_sys::aura_buffer_alloc(self.inner.raw(), size) };
        let err = io::Error::last_os_error();
        NonNull::new(ptr as *mut u8)
            .map(|p| Buffer::new(Arc::clone(&self.inner), p, size))
            .ok_or(Error::BufferAlloc(err))
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
    pub unsafe fn register_buffers(&self, buffers: &[&mut [u8]]) -> Result<()> {
        if buffers.len() > i32::MAX as usize {
            return Err(Error::Io(io::Error::from_raw_os_error(libc::EINVAL)));
        }
        let iovecs: Vec<libc::iovec> = buffers
            .iter()
            .map(|b| libc::iovec {
                iov_base: b.as_ptr() as *mut _,
                iov_len: b.len(),
            })
            .collect();

        let ret = unsafe {
            aura_sys::aura_register_buffers(
                self.inner.raw(),
                iovecs.as_ptr() as *const _,
                iovecs.len() as u32,
            )
        };

        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Unregister previously registered buffers
    ///
    /// If called from a completion callback, automatically degrades to the
    /// deferred (non-blocking) path.
    pub fn unregister_buffers(&self) -> Result<()> {
        let ret = unsafe {
            aura_sys::aura_unregister(self.inner.raw(), aura_sys::aura_reg_type_t_AURA_REG_BUFFERS)
        };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Request deferred unregister of registered buffers (callback-safe)
    pub fn request_unregister_buffers(&self) -> Result<()> {
        let ret = unsafe {
            aura_sys::aura_request_unregister(
                self.inner.raw(),
                aura_sys::aura_reg_type_t_AURA_REG_BUFFERS,
            )
        };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Register file descriptors with the kernel
    pub fn register_files(&self, fds: &[RawFd]) -> Result<()> {
        if fds.len() > i32::MAX as usize {
            return Err(Error::Io(io::Error::from_raw_os_error(libc::EINVAL)));
        }
        let ret = unsafe {
            aura_sys::aura_register_files(
                self.inner.raw(),
                fds.as_ptr(),
                fds.len() as u32,
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
            unsafe { aura_sys::aura_update_file(self.inner.raw(), index, fd) };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Unregister previously registered files
    ///
    /// If called from a completion callback, automatically degrades to the
    /// deferred (non-blocking) path.
    pub fn unregister_files(&self) -> Result<()> {
        let ret = unsafe {
            aura_sys::aura_unregister(self.inner.raw(), aura_sys::aura_reg_type_t_AURA_REG_FILES)
        };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Request deferred unregister of registered files (callback-safe)
    pub fn request_unregister_files(&self) -> Result<()> {
        let ret = unsafe {
            aura_sys::aura_request_unregister(
                self.inner.raw(),
                aura_sys::aura_reg_type_t_AURA_REG_FILES,
            )
        };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    // =========================================================================
    // Private Helpers
    // =========================================================================

    /// Submit an I/O operation with callback handling (internal helper).
    ///
    /// Eliminates boilerplate for callback context creation, error handling,
    /// and resource cleanup across all I/O submission methods.
    ///
    /// # Arguments
    ///
    /// * `callback` - User callback to wrap in CallbackContext
    /// * `ffi_call` - Closure that calls the FFI function with ctx_ptr
    ///
    /// # Returns
    ///
    /// `Ok(RequestHandle)` on successful submission, `Err(Error::Submission)` on failure.
    fn submit_with_callback<F>(
        &self,
        callback: F,
        ffi_call: impl FnOnce(*mut std::ffi::c_void) -> *mut aura_sys::aura_request_t,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        let ctx = CallbackContext::new(callback);
        let ctx_ptr = Box::into_raw(ctx) as *mut std::ffi::c_void;

        let req = ffi_call(ctx_ptr);

        if req.is_null() {
            // Capture errno before drop, which could clobber it
            let err = io::Error::last_os_error();
            unsafe { drop_context(ctx_ptr) };
            Err(Error::Submission(err))
        } else {
            Ok(RequestHandle::new(req))
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
    /// # use aura::Engine;
    /// # use std::fs::File;
    /// # use std::os::unix::io::AsRawFd;
    /// # fn main() -> Result<(), Box<dyn std::error::Error>> {
    /// # let engine = Engine::new()?;
    /// # let buf = engine.allocate_buffer(4096)?;
    /// # let file = File::open("/etc/hostname")?;
    /// # let fd = file.as_raw_fd();
    /// unsafe { engine.read(fd, (&buf).into(), 4096, 0, |result| {
    ///     println!("Read result: {:?}", result);
    /// }) }?;
    /// # Ok(())
    /// # }
    /// ```
    /// # Safety Contract
    ///
    /// The memory referenced by `buf` must remain valid and exclusively
    /// borrowed until the callback fires. `BufferRef` carries no lifetime,
    /// so the compiler cannot enforce this. See [`BufferRef`] docs.
    pub unsafe fn read<F>(
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
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_read(
                self.inner.raw(),
                fd,
                buf.as_raw(),
                len,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async write operation
    ///
    /// The callback is invoked when the write completes.
    ///
    /// # Safety Contract
    ///
    /// The memory referenced by `buf` must remain valid until the callback
    /// fires. `BufferRef` carries no lifetime, so the compiler cannot
    /// enforce this. See [`BufferRef`] docs.
    pub unsafe fn write<F>(
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
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_write(
                self.inner.raw(),
                fd,
                buf.as_raw(),
                len,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async fsync operation
    ///
    /// Ensures all previous writes to the file descriptor are flushed to storage.
    pub fn fsync<F>(&self, fd: RawFd, callback: F) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_fsync(
                self.inner.raw(),
                fd,
                aura_sys::AURA_FSYNC_DEFAULT,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async fdatasync operation
    ///
    /// Like fsync, but may skip metadata if not needed for data integrity.
    pub fn fdatasync<F>(&self, fd: RawFd, callback: F) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_fsync(
                self.inner.raw(),
                fd,
                aura_sys::AURA_FSYNC_DATASYNC,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async vectored read operation
    ///
    /// # Safety
    ///
    /// The caller must ensure both the iovec **array** and the buffers
    /// it points to remain valid and exclusively borrowed until the
    /// completion callback fires. The kernel reads the iovec descriptors
    /// and accesses the buffers asynchronously after submission.
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
        if iovecs.len() > i32::MAX as usize {
            return Err(Error::Io(io::Error::from_raw_os_error(libc::EINVAL)));
        }
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_readv(
                self.inner.raw(),
                fd,
                iovecs.as_ptr() as *const _,
                iovecs.len() as i32,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async vectored write operation
    ///
    /// # Safety
    ///
    /// The caller must ensure both the iovec **array** and the buffers
    /// it points to remain valid and exclusively borrowed until the
    /// completion callback fires. The kernel reads the iovec descriptors
    /// and accesses the buffers asynchronously after submission.
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
        if iovecs.len() > i32::MAX as usize {
            return Err(Error::Io(io::Error::from_raw_os_error(libc::EINVAL)));
        }
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_writev(
                self.inner.raw(),
                fd,
                iovecs.as_ptr() as *const _,
                iovecs.len() as i32,
                offset,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    // =========================================================================
    // Lifecycle Metadata Operations
    // =========================================================================

    /// Submit an async openat operation
    ///
    /// Opens a file relative to a directory fd. The callback receives
    /// `Ok(fd)` with the new file descriptor on success, or `Err(error)` on failure.
    /// The `pathname` is copied into the io_uring SQE synchronously during
    /// submission, so it only needs to remain valid for the duration of this call.
    ///
    /// # Arguments
    ///
    /// * `dirfd` - Directory fd (`libc::AT_FDCWD` for current directory)
    /// * `pathname` - Null-terminated path (relative to dirfd)
    /// * `flags` - Open flags (`libc::O_RDONLY`, `libc::O_CREAT`, etc.)
    /// * `mode` - File mode (used when `O_CREAT` is set)
    /// * `callback` - Completion callback
    pub fn openat<F>(
        &self,
        dirfd: RawFd,
        pathname: &std::ffi::CStr,
        flags: i32,
        mode: u32,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_openat(
                self.inner.raw(),
                dirfd,
                pathname.as_ptr(),
                flags,
                mode,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async close operation
    ///
    /// Closes a file descriptor asynchronously. The callback receives
    /// `Ok(0)` on success or `Err(error)` on failure.
    pub fn close<F>(&self, fd: RawFd, callback: F) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_close(self.inner.raw(), fd, Some(callback_trampoline), ctx_ptr)
        })
    }

    /// Submit an async statx operation
    ///
    /// Retrieves file metadata. The callback receives `Ok(0)` on success
    /// (the `statxbuf` is filled) or `Err(error)` on failure.
    ///
    /// # Safety
    ///
    /// The caller must ensure `statxbuf` points to valid memory that remains
    /// valid until the completion callback fires. The `pathname` is copied
    /// into the io_uring SQE synchronously during submission, so it only
    /// needs to remain valid for the duration of this call.
    pub unsafe fn statx<F>(
        &self,
        dirfd: RawFd,
        pathname: &std::ffi::CStr,
        flags: i32,
        mask: u32,
        statxbuf: *mut aura_sys::statx,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_statx(
                self.inner.raw(),
                dirfd,
                pathname.as_ptr(),
                flags,
                mask,
                statxbuf,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async fallocate operation
    ///
    /// Preallocates or deallocates file space.
    ///
    /// # Arguments
    ///
    /// * `fd` - File descriptor
    /// * `mode` - Allocation mode (0, `libc::FALLOC_FL_KEEP_SIZE`, etc.)
    /// * `offset` - Starting byte offset
    /// * `len` - Length of region
    /// * `callback` - Completion callback
    pub fn fallocate<F>(
        &self,
        fd: RawFd,
        mode: i32,
        offset: i64,
        len: i64,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_fallocate(
                self.inner.raw(),
                fd,
                mode,
                offset,
                len,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async ftruncate operation
    ///
    /// Truncates a file to the specified length. Requires kernel 6.9+;
    /// returns `ENOSYS` on older kernels.
    pub fn ftruncate<F>(
        &self,
        fd: RawFd,
        length: i64,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_ftruncate(
                self.inner.raw(),
                fd,
                length,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
    }

    /// Submit an async sync_file_range operation
    ///
    /// Syncs a byte range without flushing metadata.
    ///
    /// # Arguments
    ///
    /// * `fd` - File descriptor
    /// * `offset` - Starting byte offset
    /// * `nbytes` - Number of bytes to sync (0 = to end of file)
    /// * `flags` - Combination of `AURA_SYNC_RANGE_WAIT_BEFORE`, `_WRITE`, `_WAIT_AFTER`
    /// * `callback` - Completion callback
    pub fn sync_file_range<F>(
        &self,
        fd: RawFd,
        offset: i64,
        nbytes: i64,
        flags: u32,
        callback: F,
    ) -> Result<RequestHandle>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        self.submit_with_callback(callback, |ctx_ptr| unsafe {
            aura_sys::aura_sync_file_range(
                self.inner.raw(),
                fd,
                offset,
                nbytes,
                flags,
                Some(callback_trampoline),
                ctx_ptr,
            )
        })
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
            unsafe { aura_sys::aura_cancel(self.inner.raw(), request.as_ptr()) };
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
        let fd = unsafe { aura_sys::aura_get_poll_fd(self.inner.raw()) };
        if fd >= 0 {
            Ok(fd)
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Force-flush all pending SQEs across all rings
    ///
    /// Submits any queued SQEs to the kernel immediately. Useful after
    /// building a linked chain to ensure it is submitted.
    pub fn flush(&self) -> Result<()> {
        let ret = unsafe { aura_sys::aura_flush(self.inner.raw()) };
        if ret == 0 {
            Ok(())
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Process completed operations (non-blocking)
    ///
    /// Returns the number of completions processed.
    ///
    /// # Deadlock Warning
    ///
    /// Do **not** call `poll()` from within a completion callback.
    /// Callbacks are invoked while the internal poll lock is held,
    /// so calling `poll()` (or `wait()`/`run()`) from a callback
    /// will deadlock.
    ///
    /// # Thread Safety
    ///
    /// Must not be called concurrently with `wait()` or `run()`.
    /// This is enforced by an internal lock; concurrent calls will
    /// serialize rather than cause undefined behavior.
    pub fn poll(&self) -> Result<usize> {
        let _guard = self.poll_lock.lock().unwrap_or_else(|e| e.into_inner());
        let n = unsafe { aura_sys::aura_poll(self.inner.raw()) };
        if n >= 0 {
            Ok(n as usize)
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
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
    /// Returns the number of completions processed, `Ok(0)` if nothing
    /// is pending, or `Err(ETIMEDOUT)` if the timeout expired with
    /// operations still in flight.
    ///
    /// # Deadlock Warning
    ///
    /// Do **not** call `wait()` from within a completion callback.
    /// Callbacks are invoked while the internal poll lock is held,
    /// so calling `wait()` (or `poll()`/`run()`) from a callback
    /// will deadlock.
    ///
    /// # Thread Safety
    ///
    /// Must not be called concurrently with `poll()` or `run()`.
    /// This is enforced by an internal lock; concurrent calls will
    /// serialize rather than cause undefined behavior.
    pub fn wait(&self, timeout_ms: i32) -> Result<usize> {
        let _guard = self.poll_lock.lock().unwrap_or_else(|e| e.into_inner());
        let n = unsafe { aura_sys::aura_wait(self.inner.raw(), timeout_ms) };
        if n >= 0 {
            Ok(n as usize)
        } else {
            let err = io::Error::last_os_error();
            if err.raw_os_error() == Some(libc::ETIMEDOUT) {
                Err(Error::TimedOut)
            } else {
                Err(Error::Io(err))
            }
        }
    }

    /// Run the event loop until `stop()` is called
    ///
    /// Blocks the calling thread, continuously processing completions.
    /// Call `stop()` from a callback or another thread to exit.
    ///
    /// # Deadlock Warning
    ///
    /// Do **not** call `run()` from within a completion callback.
    /// Callbacks are invoked while the internal poll lock is held,
    /// so calling `run()` (or `poll()`/`wait()`) from a callback
    /// will deadlock. Call `stop()` from a callback instead.
    ///
    /// # Thread Safety
    ///
    /// Must not be called concurrently with `poll()` or `wait()`.
    /// This is enforced by the same internal lock used by those methods.
    pub fn run(&self) {
        let _guard = self.poll_lock.lock().unwrap_or_else(|e| e.into_inner());
        unsafe { aura_sys::aura_run(self.inner.raw()) };
    }

    /// Signal the event loop to stop
    ///
    /// Safe to call from any thread, including from within a callback.
    pub fn stop(&self) {
        unsafe { aura_sys::aura_stop(self.inner.raw()) };
    }

    /// Drain all pending I/O operations
    ///
    /// Blocks until all in-flight operations have completed or the timeout
    /// expires. Useful for graceful shutdown.
    ///
    /// # Arguments
    ///
    /// * `timeout_ms` - Maximum wait time in milliseconds
    ///   - `-1` = wait forever
    ///   - `0` = don't block
    ///   - `>0` = wait up to N milliseconds
    ///
    /// Returns the total number of completions processed.
    ///
    /// # Deadlock Warning
    ///
    /// Do **not** call `drain()` from within a completion callback.
    /// Must not be called concurrently with `poll()`, `wait()`, or `run()`.
    pub fn drain(&self, timeout_ms: i32) -> Result<usize> {
        let _guard = self.poll_lock.lock().unwrap_or_else(|e| e.into_inner());
        let n = unsafe { aura_sys::aura_drain(self.inner.raw(), timeout_ms) };
        if n >= 0 {
            Ok(n as usize)
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    /// Get current engine statistics
    pub fn stats(&self) -> Result<Stats> {
        let mut inner: aura_sys::aura_stats_t = unsafe { std::mem::zeroed() };
        let ret = unsafe {
            aura_sys::aura_get_stats(
                self.inner.raw(),
                &mut inner,
                std::mem::size_of::<aura_sys::aura_stats_t>(),
            )
        };
        if ret == 0 {
            Ok(Stats::new(inner))
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    /// Get number of rings in the engine
    pub fn ring_count(&self) -> i32 {
        unsafe { aura_sys::aura_get_ring_count(self.inner.raw()) }
    }

    /// Get statistics for a specific ring
    ///
    /// # Arguments
    ///
    /// * `ring_idx` - Ring index (0 to ring_count()-1)
    ///
    /// # Returns
    ///
    /// `Ok(RingStats)` on success, `Err` if ring_idx is invalid
    pub fn ring_stats(&self, ring_idx: i32) -> Result<crate::stats::RingStats> {
        let mut inner: aura_sys::aura_ring_stats_t = unsafe { std::mem::zeroed() };
        let ret = unsafe { aura_sys::aura_get_ring_stats(self.inner.raw(), ring_idx, &mut inner, std::mem::size_of::<aura_sys::aura_ring_stats_t>()) };
        if ret == 0 {
            Ok(crate::stats::RingStats::new(inner))
        } else {
            Err(Error::InvalidArgument("Invalid ring index"))
        }
    }

    /// Get latency histogram for a specific ring
    ///
    /// # Arguments
    ///
    /// * `ring_idx` - Ring index (0 to ring_count()-1)
    ///
    /// # Returns
    ///
    /// `Ok(Histogram)` on success, `Err` if ring_idx is invalid
    pub fn histogram(&self, ring_idx: i32) -> Result<crate::stats::Histogram> {
        let mut inner: aura_sys::aura_histogram_t = unsafe { std::mem::zeroed() };
        let ret = unsafe {
            aura_sys::aura_get_histogram(
                self.inner.raw(),
                ring_idx,
                &mut inner,
                std::mem::size_of::<aura_sys::aura_histogram_t>(),
            )
        };
        if ret == 0 {
            Ok(crate::stats::Histogram::new(inner))
        } else {
            Err(Error::InvalidArgument("Invalid ring index"))
        }
    }

    /// Get buffer pool statistics
    pub fn buffer_stats(&self) -> Result<crate::stats::BufferStats> {
        let mut inner: aura_sys::aura_buffer_stats_t = unsafe { std::mem::zeroed() };
        let ret = unsafe { aura_sys::aura_get_buffer_stats(self.inner.raw(), &mut inner, std::mem::size_of::<aura_sys::aura_buffer_stats_t>()) };
        if ret == 0 {
            Ok(crate::stats::BufferStats::new(inner))
        } else {
            Err(Error::Io(io::Error::last_os_error()))
        }
    }

    // =========================================================================
    // Diagnostics
    // =========================================================================

    /// Check if the engine has a fatal error
    ///
    /// Once a fatal error is latched (e.g., io_uring ring fd becomes invalid),
    /// all subsequent submissions fail with `ESHUTDOWN`. Use this to distinguish
    /// a permanently broken engine from transient `EAGAIN`.
    ///
    /// Returns `None` if healthy, `Some(io::Error)` if fatally broken.
    pub fn get_fatal_error(&self) -> Option<io::Error> {
        let ret = unsafe { aura_sys::aura_get_fatal_error(self.inner.raw()) };
        if ret > 0 {
            Some(io::Error::from_raw_os_error(ret))
        } else {
            None
        }
    }
}

/// Get the library version string
pub fn version() -> &'static str {
    unsafe {
        let ptr = aura_sys::aura_version();
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
    unsafe { aura_sys::aura_version_int() }
}

/// Check if the current thread is inside a completion callback
///
/// Useful for libraries building on AuraIO to choose between synchronous
/// and deferred code paths (e.g., `unregister_buffers` vs `request_unregister_buffers`).
pub fn in_callback_context() -> bool {
    unsafe { aura_sys::aura_in_callback_context() }
}
