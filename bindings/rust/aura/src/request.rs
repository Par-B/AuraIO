// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Request handle for pending I/O operations

use std::os::unix::io::RawFd;

/// Handle to a pending I/O request.
///
/// This handle can be used to:
/// - Check if the request is still pending via [`is_pending()`](Self::is_pending)
/// - Cancel the request via [`Engine::cancel()`](crate::Engine::cancel)
/// - Get the associated file descriptor via [`fd()`](Self::fd)
///
/// # ⚠️ Lifetime Warning
///
/// **This handle becomes invalid the moment the completion callback begins.**
///
/// The underlying request memory is recycled when the callback starts executing.
/// Using the handle after this point is undefined behavior (dangling pointer).
///
/// ```text
/// Timeline:
///   submit() -----> [handle valid] -----> callback starts -----> [INVALID]
///                   can cancel here       handle is dangling
/// ```
///
/// ## Safe Pattern
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
/// let handle = unsafe { engine.read(fd, (&buf).into(), 4096, 0, |result| {
///     // Handle is INVALID inside this callback!
///     // Do NOT pass the handle into the closure.
/// }) }?;
///
/// // Safe: handle is valid here, before callback runs
/// if unsafe { handle.is_pending() } {
///     unsafe { engine.cancel(&handle)? };
/// }
///
/// engine.wait(-1)?;  // Callback runs during wait()
/// // UNSAFE: handle is now invalid, do not use!
/// # Ok(())
/// # }
/// ```
///
/// ## Dangerous Pattern
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
/// let handle = unsafe { engine.read(fd, (&buf).into(), 4096, 0, |_| {}) }?;
/// engine.wait(-1)?;  // Callback executes here
/// unsafe { handle.is_pending() };  // ❌ UNDEFINED BEHAVIOR - handle is dangling!
/// # Ok(())
/// # }
/// ```
#[derive(Debug)]
pub struct RequestHandle {
    inner: *mut aura_sys::aura_request_t,
}

// Safety: RequestHandle can be sent between threads
// (the underlying library handles synchronization)
unsafe impl Send for RequestHandle {}

impl RequestHandle {
    /// Create a new request handle (internal use)
    pub(crate) fn new(ptr: *mut aura_sys::aura_request_t) -> Self {
        Self { inner: ptr }
    }

    /// Check if the request is still pending
    ///
    /// Returns `true` if the operation is still in-flight, `false` if it
    /// has completed or was cancelled.
    ///
    /// # Safety
    ///
    /// The caller must ensure the handle is still valid (the completion
    /// callback has not yet been invoked). See the struct-level docs for
    /// the validity timeline.
    pub unsafe fn is_pending(&self) -> bool {
        unsafe { aura_sys::aura_request_pending(self.inner) }
    }

    /// Get the file descriptor associated with this request
    ///
    /// # Safety
    ///
    /// The caller must ensure the handle is still valid (the completion
    /// callback has not yet been invoked). See the struct-level docs for
    /// the validity timeline.
    pub unsafe fn fd(&self) -> RawFd {
        unsafe { aura_sys::aura_request_fd(self.inner) }
    }

    /// Get the operation type of this request
    ///
    /// Returns the operation type as a raw integer matching the `aura_op_type_t_AURA_OP_*`
    /// constants (e.g., `aura_sys::aura_op_type_t_AURA_OP_READ`).
    ///
    /// # Safety
    ///
    /// The caller must ensure the handle is still valid (the completion
    /// callback has not yet been invoked). See the struct-level docs for
    /// the validity timeline.
    pub unsafe fn op_type(&self) -> i32 {
        unsafe { aura_sys::aura_request_op_type(self.inner) }
    }

    /// Get the user data pointer associated with this request
    ///
    /// Returns the `user_data` pointer that was passed when the request was submitted.
    ///
    /// # Safety
    ///
    /// The caller must ensure the handle is still valid (the completion
    /// callback has not yet been invoked). See the struct-level docs for
    /// the validity timeline.
    pub unsafe fn user_data(&self) -> *mut std::ffi::c_void {
        unsafe { aura_sys::aura_request_user_data(self.inner) }
    }

    /// Mark this request as linked
    ///
    /// The next submission on this thread will be chained via `IOSQE_IO_LINK`.
    /// The chained operation won't start until this one completes successfully.
    /// If this operation fails, the chained operation receives `-ECANCELED`.
    ///
    /// # Safety
    ///
    /// The caller must ensure the handle is still valid (the completion
    /// callback has not yet been invoked).
    pub unsafe fn set_linked(&self) {
        unsafe { aura_sys::aura_request_set_linked(self.inner) };
    }

    /// Check if this request is marked as linked
    ///
    /// # Safety
    ///
    /// The caller must ensure the handle is still valid (the completion
    /// callback has not yet been invoked).
    pub unsafe fn is_linked(&self) -> bool {
        unsafe { aura_sys::aura_request_is_linked(self.inner) }
    }

    /// Get the raw pointer (for internal use)
    pub(crate) fn as_ptr(&self) -> *mut aura_sys::aura_request_t {
        self.inner
    }
}
