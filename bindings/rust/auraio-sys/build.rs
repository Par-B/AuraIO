//! Build script for auraio-sys
//!
//! This script:
//! 1. Locates the auraio C library (via pkg-config or environment variables)
//! 2. Generates Rust bindings using bindgen

use std::env;
use std::path::PathBuf;

fn main() {
    // Tell cargo to rerun if these change
    println!("cargo:rerun-if-env-changed=AURAIO_LIB_DIR");
    println!("cargo:rerun-if-env-changed=AURAIO_INCLUDE_DIR");
    println!("cargo:rerun-if-changed=wrapper.h");

    let mut include_path: Option<PathBuf> = None;

    // Try pkg-config first (works after `make install`)
    if let Ok(lib) = pkg_config::probe_library("libauraio") {
        println!("cargo:rustc-link-lib=auraio");
        if let Some(path) = lib.include_paths.first() {
            include_path = Some(path.clone());
        }
    } else {
        // Fall back to environment variables or relative paths
        if let Ok(lib_dir) = env::var("AURAIO_LIB_DIR") {
            println!("cargo:rustc-link-search=native={}", lib_dir);
        } else {
            // Default: assume building from bindings/rust/auraio-sys/
            // Library is at ../../lib/
            let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
            let lib_path = PathBuf::from(&manifest_dir)
                .join("../../../lib");
            if lib_path.exists() {
                println!("cargo:rustc-link-search=native={}", lib_path.display());
            }
        }

        if let Ok(inc_dir) = env::var("AURAIO_INCLUDE_DIR") {
            include_path = Some(PathBuf::from(inc_dir));
        } else {
            // Default: assume building from bindings/rust/auraio-sys/
            // Headers are at ../../../include/
            let manifest_dir = env::var("CARGO_MANIFEST_DIR").unwrap();
            let inc_path = PathBuf::from(&manifest_dir)
                .join("../../../include");
            if inc_path.exists() {
                include_path = Some(inc_path);
            }
        }

        println!("cargo:rustc-link-lib=auraio");
    }

    // Always need liburing
    println!("cargo:rustc-link-lib=uring");

    // Determine include path for bindgen
    let include_arg = include_path
        .as_ref()
        .map(|p| format!("-I{}", p.display()))
        .unwrap_or_default();

    let header_path = include_path
        .as_ref()
        .map(|p| p.join("auraio.h"))
        .unwrap_or_else(|| PathBuf::from("auraio.h"));

    // Generate bindings
    let bindings = bindgen::Builder::default()
        .header(header_path.to_string_lossy())
        .clang_arg(include_arg)
        // Only generate bindings for auraio types and functions
        .allowlist_function("auraio_.*")
        .allowlist_type("auraio_.*")
        .allowlist_var("AURAIO_.*")
        // Generate proper Rust types
        .derive_debug(true)
        .derive_default(true)
        .derive_copy(true)
        // Use core types where possible
        .use_core()
        // Generate layout tests to catch ABI mismatches
        .layout_tests(true)
        .generate()
        .expect("Unable to generate bindings");

    // Write bindings to OUT_DIR
    let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings");
}
