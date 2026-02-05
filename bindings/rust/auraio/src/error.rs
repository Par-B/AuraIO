//! Error types for AuraIO

use std::io;
use thiserror::Error;

/// Error type for AuraIO operations
#[derive(Debug, Error)]
pub enum Error {
    /// I/O error from the kernel
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),

    /// Engine creation failed
    #[error("Failed to create engine: {0}")]
    EngineCreate(io::Error),

    /// Buffer allocation failed
    #[error("Failed to allocate buffer: {0}")]
    BufferAlloc(io::Error),

    /// Submission failed (queue full or shutdown)
    #[error("Submission failed: {0}")]
    Submission(io::Error),

    /// Operation was cancelled
    #[error("Operation cancelled")]
    Cancelled,

    /// Invalid argument
    #[error("Invalid argument: {0}")]
    InvalidArgument(&'static str),
}

/// Result type alias for AuraIO operations
pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    /// Create an error from a raw errno value (negative result from callback)
    pub fn from_raw_os_error(code: i32) -> Self {
        if code == libc::ECANCELED {
            Error::Cancelled
        } else {
            Error::Io(io::Error::from_raw_os_error(code))
        }
    }
}
