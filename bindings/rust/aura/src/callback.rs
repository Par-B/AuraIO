//! Callback type erasure for I/O completions

use crate::error::{Error, Result};

/// Type-erased callback storage
///
/// We box the callback to store it as user_data in the C API.
/// The trampoline function unboxes and invokes it.
pub(crate) type BoxedCallback = Box<dyn FnOnce(Result<usize>) + Send + 'static>;

/// Context stored as user_data for each I/O operation
pub(crate) struct CallbackContext {
    pub callback: Option<BoxedCallback>,
}

impl CallbackContext {
    pub fn new<F>(callback: F) -> Box<Self>
    where
        F: FnOnce(Result<usize>) + Send + 'static,
    {
        Box::new(Self {
            callback: Some(Box::new(callback)),
        })
    }
}

/// C callback trampoline
///
/// This function is passed to the C API. It converts the result and
/// invokes the Rust callback.
///
/// # Safety
///
/// - `user_data` must be a valid pointer to a `Box<CallbackContext>`
/// - This function takes ownership of the context and drops it after invocation
pub(crate) extern "C" fn callback_trampoline(
    _req: *mut aura_sys::aura_request_t,
    result: isize,
    user_data: *mut std::ffi::c_void,
) {
    if user_data.is_null() {
        return;
    }

    // Take ownership of the context
    let ctx = unsafe { Box::from_raw(user_data as *mut CallbackContext) };

    if let Some(callback) = ctx.callback {
        let rust_result = if result < 0 {
            // Use wrapping_neg to avoid overflow panic on isize::MIN in debug builds
            let neg = result.wrapping_neg();
            let errno = i32::try_from(neg).unwrap_or(5); // EIO if out of i32 range
            Err(Error::from_raw_os_error(errno))
        } else {
            Ok(result as usize)
        };

        // catch_unwind prevents panics from unwinding across the FFI boundary,
        // which would be undefined behavior.
        if std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            callback(rust_result);
        }))
        .is_err()
        {
            eprintln!("aura: panic in I/O callback (caught at FFI boundary, request={:p})", _req);
        }
    }
    // Context is dropped here, freeing the memory
}
