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
    EngineCreate(#[source] io::Error),

    /// Buffer allocation failed
    #[error("Failed to allocate buffer: {0}")]
    BufferAlloc(#[source] io::Error),

    /// Submission failed (queue full or shutdown)
    #[error("Submission failed: {0}")]
    Submission(#[source] io::Error),

    /// Operation was cancelled
    #[error("Operation cancelled")]
    Cancelled,

    /// Timeout expired with operations still in flight
    #[error("Timeout expired with pending operations")]
    TimedOut,

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

#[cfg(test)]
mod tests {
    use super::*;
    use std::error::Error as StdError;

    #[test]
    fn test_engine_create_has_source() {
        let io_err = io::Error::new(io::ErrorKind::Other, "test error");
        let err = Error::EngineCreate(io_err);

        // Verify that source() returns Some(...)
        assert!(err.source().is_some(), "EngineCreate should have a source");

        // Verify the source is an io::Error
        let source = err.source().unwrap();
        assert!(source.is::<io::Error>(), "Source should be io::Error");
    }

    #[test]
    fn test_buffer_alloc_has_source() {
        let io_err = io::Error::new(io::ErrorKind::OutOfMemory, "allocation failed");
        let err = Error::BufferAlloc(io_err);

        // Verify that source() returns Some(...)
        assert!(err.source().is_some(), "BufferAlloc should have a source");

        // Verify the source is an io::Error
        let source = err.source().unwrap();
        assert!(source.is::<io::Error>(), "Source should be io::Error");
    }

    #[test]
    fn test_submission_has_source() {
        let io_err = io::Error::new(io::ErrorKind::WouldBlock, "queue full");
        let err = Error::Submission(io_err);

        // Verify that source() returns Some(...)
        assert!(err.source().is_some(), "Submission should have a source");

        // Verify the source is an io::Error
        let source = err.source().unwrap();
        assert!(source.is::<io::Error>(), "Source should be io::Error");
    }

    #[test]
    fn test_cancelled_has_no_source() {
        let err = Error::Cancelled;

        // Verify that source() returns None for variants without wrapped errors
        assert!(err.source().is_none(), "Cancelled should not have a source");
    }

    #[test]
    fn test_invalid_argument_has_no_source() {
        let err = Error::InvalidArgument("test");

        // Verify that source() returns None for variants without wrapped errors
        assert!(err.source().is_none(), "InvalidArgument should not have a source");
    }
}
