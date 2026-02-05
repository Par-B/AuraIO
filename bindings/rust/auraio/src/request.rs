//! Request handle for pending I/O operations

use auraio_sys;
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
/// ```ignore
/// let handle = engine.read(fd, buf, len, 0, |result| {
///     // Handle is INVALID inside this callback!
///     // Do NOT pass the handle into the closure.
/// })?;
///
/// // Safe: handle is valid here, before callback runs
/// if handle.is_pending() {
///     engine.cancel(&handle)?;
/// }
///
/// engine.wait(-1)?;  // Callback runs during wait()
/// // UNSAFE: handle is now invalid, do not use!
/// ```
///
/// ## Dangerous Pattern
///
/// ```ignore
/// let handle = engine.read(...)?;
/// engine.wait(-1)?;  // Callback executes here
/// handle.is_pending();  // ❌ UNDEFINED BEHAVIOR - handle is dangling!
/// ```
#[derive(Debug)]
pub struct RequestHandle {
    inner: *mut auraio_sys::auraio_request_t,
}

// Safety: RequestHandle can be sent between threads
// (the underlying library handles synchronization)
unsafe impl Send for RequestHandle {}

impl RequestHandle {
    /// Create a new request handle (internal use)
    pub(crate) fn new(ptr: *mut auraio_sys::auraio_request_t) -> Self {
        Self { inner: ptr }
    }

    /// Check if the request is still pending
    ///
    /// Returns `true` if the operation is still in-flight, `false` if it
    /// has completed or was cancelled.
    pub fn is_pending(&self) -> bool {
        unsafe { auraio_sys::auraio_request_pending(self.inner) }
    }

    /// Get the file descriptor associated with this request
    pub fn fd(&self) -> RawFd {
        unsafe { auraio_sys::auraio_request_fd(self.inner) }
    }

    /// Get the raw pointer (for internal use)
    pub(crate) fn as_ptr(&self) -> *mut auraio_sys::auraio_request_t {
        self.inner
    }
}
