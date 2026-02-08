//! Raw FFI bindings for AuraIO
//!
//! This crate provides unsafe, low-level bindings to the AuraIO C library.
//! For a safe, idiomatic Rust API, use the `auraio` crate instead.
//!
//! # Safety
//!
//! All functions in this crate are unsafe and follow C calling conventions.
//! The caller is responsible for:
//! - Ensuring pointers are valid and properly aligned
//! - Managing buffer lifetimes (buffers must remain valid until callbacks complete)
//! - Not calling `auraio_destroy` from within a callback
//!
//! # Example
//!
//! ```no_run
//! use auraio_sys::*;
//!
//! unsafe {
//!     let engine = auraio_create();
//!     if engine.is_null() {
//!         panic!("Failed to create engine");
//!     }
//!
//!     // ... use the engine ...
//!
//!     auraio_destroy(engine);
//! }
//! ```

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]

// Include the generated bindings
include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_version() {
        unsafe {
            let version = auraio_version();
            assert!(!version.is_null());
            let version_int = auraio_version_int();
            assert!(version_int >= 100); // At least version 0.1.0
        }
    }

    #[test]
    fn test_create_destroy() {
        unsafe {
            let engine = auraio_create();
            assert!(!engine.is_null(), "Failed to create engine");
            auraio_destroy(engine);
        }
    }

    #[test]
    fn test_options_init() {
        unsafe {
            let mut options: auraio_options_t = std::mem::zeroed();
            auraio_options_init(&mut options);
            assert!(options.queue_depth > 0);
        }
    }

    // Note: auraio_buf() and auraio_buf_fixed() are inline C functions
    // and are not available through bindgen. Use the safe auraio crate's
    // BufferRef type instead.
}
