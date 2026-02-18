// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 AuraIO Contributors


//! Raw FFI bindings for AuraIO
//!
//! This crate provides unsafe, low-level bindings to the AuraIO C library.
//! For a safe, idiomatic Rust API, use the `aura` crate instead.
//!
//! # Safety
//!
//! All functions in this crate are unsafe and follow C calling conventions.
//! The caller is responsible for:
//! - Ensuring pointers are valid and properly aligned
//! - Managing buffer lifetimes (buffers must remain valid until callbacks complete)
//! - Not calling `aura_destroy` from within a callback
//!
//! # Example
//!
//! ```no_run
//! use aura_sys::*;
//!
//! unsafe {
//!     let engine = aura_create();
//!     if engine.is_null() {
//!         panic!("Failed to create engine");
//!     }
//!
//!     // ... use the engine ...
//!
//!     aura_destroy(engine);
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
            let version = aura_version();
            assert!(!version.is_null());
            let version_int = aura_version_int();
            assert!(version_int >= 100); // At least version 0.1.0
        }
    }

    #[test]
    fn test_create_destroy() {
        unsafe {
            let engine = aura_create();
            assert!(!engine.is_null(), "Failed to create engine");
            aura_destroy(engine);
        }
    }

    #[test]
    fn test_options_init() {
        unsafe {
            let mut options: aura_options_t = std::mem::zeroed();
            aura_options_init(&mut options);
            assert!(options.queue_depth > 0);
        }
    }

    // Note: aura_buf() and aura_buf_fixed() are inline C functions
    // and are not available through bindgen. Use the safe aura crate's
    // BufferRef type instead.
}
