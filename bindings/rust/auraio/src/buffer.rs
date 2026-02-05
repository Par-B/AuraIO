//! Buffer management for AuraIO

use auraio_sys;
use std::fmt;
use std::ptr::NonNull;

/// RAII buffer allocated from the engine's pool
///
/// Automatically returned to the pool when dropped.
/// Provides page-aligned memory suitable for O_DIRECT I/O.
///
/// # Example
///
/// ```no_run
/// use auraio::Engine;
///
/// # fn main() -> Result<(), Box<dyn std::error::Error>> {
/// let engine = Engine::new()?;
/// let buf = engine.allocate_buffer(4096)?;
///
/// // Use buf.as_mut_slice() for writing
/// // Use buf.as_slice() for reading
///
/// // Buffer automatically returned to pool when dropped
/// # Ok(())
/// # }
/// ```
pub struct Buffer {
    engine: *mut auraio_sys::auraio_engine_t,
    ptr: NonNull<u8>,
    len: usize,
}

// Safety: Buffer can be sent between threads (the underlying engine handles synchronization)
unsafe impl Send for Buffer {}

impl Buffer {
    /// Create a new buffer (called by Engine::allocate_buffer)
    pub(crate) fn new(
        engine: *mut auraio_sys::auraio_engine_t,
        ptr: NonNull<u8>,
        len: usize,
    ) -> Self {
        Self { engine, ptr, len }
    }

    /// Get the buffer as a byte slice
    pub fn as_slice(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }

    /// Get the buffer as a mutable byte slice
    pub fn as_mut_slice(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr.as_ptr(), self.len) }
    }

    /// Get the raw pointer to the buffer
    pub fn as_ptr(&self) -> *const u8 {
        self.ptr.as_ptr()
    }

    /// Get the raw mutable pointer to the buffer
    pub fn as_mut_ptr(&mut self) -> *mut u8 {
        self.ptr.as_ptr()
    }

    /// Get the buffer size
    pub fn len(&self) -> usize {
        self.len
    }

    /// Check if the buffer is empty
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Create a BufferRef from this buffer
    pub fn to_ref(&self) -> BufferRef {
        BufferRef::from_ptr(self.ptr.as_ptr() as *mut std::ffi::c_void)
    }
}

impl Drop for Buffer {
    fn drop(&mut self) {
        unsafe {
            auraio_sys::auraio_buffer_free(
                self.engine,
                self.ptr.as_ptr() as *mut std::ffi::c_void,
                self.len,
            );
        }
    }
}

impl AsRef<[u8]> for Buffer {
    fn as_ref(&self) -> &[u8] {
        self.as_slice()
    }
}

impl AsMut<[u8]> for Buffer {
    fn as_mut(&mut self) -> &mut [u8] {
        self.as_mut_slice()
    }
}

/// Lightweight buffer reference for I/O operations
///
/// Can reference either:
/// - An unregistered (regular) buffer pointer
/// - A registered buffer by index
///
/// This is a small value type that can be copied cheaply.
#[derive(Clone, Copy)]
pub struct BufferRef {
    inner: auraio_sys::auraio_buf_t,
}

impl fmt::Debug for BufferRef {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("BufferRef")
            .field("type", &self.inner.type_)
            .finish_non_exhaustive()
    }
}

/// Helper function to create an unregistered buffer descriptor
/// (equivalent to the C inline function auraio_buf)
fn make_buf(ptr: *mut std::ffi::c_void) -> auraio_sys::auraio_buf_t {
    let mut buf: auraio_sys::auraio_buf_t = unsafe { std::mem::zeroed() };
    buf.type_ = auraio_sys::auraio_buf_type_t_AURAIO_BUF_UNREGISTERED;
    buf.u.ptr = ptr;
    buf
}

/// Helper function to create a registered buffer descriptor
/// (equivalent to the C inline function auraio_buf_fixed)
fn make_buf_fixed(index: i32, offset: usize) -> auraio_sys::auraio_buf_t {
    let mut buf: auraio_sys::auraio_buf_t = unsafe { std::mem::zeroed() };
    buf.type_ = auraio_sys::auraio_buf_type_t_AURAIO_BUF_REGISTERED;
    buf.u.fixed.index = index;
    buf.u.fixed.offset = offset;
    buf
}

impl BufferRef {
    /// Create a buffer reference from a raw pointer
    pub fn from_ptr(ptr: *mut std::ffi::c_void) -> Self {
        Self {
            inner: make_buf(ptr),
        }
    }

    /// Create a buffer reference from a byte slice
    pub fn from_slice(slice: &[u8]) -> Self {
        Self::from_ptr(slice.as_ptr() as *mut std::ffi::c_void)
    }

    /// Create a buffer reference from a mutable byte slice
    pub fn from_mut_slice(slice: &mut [u8]) -> Self {
        Self::from_ptr(slice.as_mut_ptr() as *mut std::ffi::c_void)
    }

    /// Create a reference to a registered (fixed) buffer
    ///
    /// The buffer must have been previously registered with `Engine::register_buffers()`.
    pub fn fixed(index: i32, offset: usize) -> Self {
        Self {
            inner: make_buf_fixed(index, offset),
        }
    }

    /// Create a reference to a registered buffer at offset 0
    pub fn fixed_index(index: i32) -> Self {
        Self::fixed(index, 0)
    }

    /// Get the underlying C buffer descriptor
    pub(crate) fn as_raw(&self) -> auraio_sys::auraio_buf_t {
        self.inner
    }
}

impl From<&Buffer> for BufferRef {
    fn from(buf: &Buffer) -> Self {
        buf.to_ref()
    }
}

impl From<&mut Buffer> for BufferRef {
    fn from(buf: &mut Buffer) -> Self {
        buf.to_ref()
    }
}

impl<'a> From<&'a [u8]> for BufferRef {
    fn from(slice: &'a [u8]) -> Self {
        Self::from_slice(slice)
    }
}

impl<'a> From<&'a mut [u8]> for BufferRef {
    fn from(slice: &'a mut [u8]) -> Self {
        Self::from_mut_slice(slice)
    }
}
