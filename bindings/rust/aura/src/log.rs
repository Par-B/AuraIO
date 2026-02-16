//! Logging interface for AuraIO
//!
//! Provides type-safe wrappers around the C log handler API.
//! The log handler is process-wide and thread-safe.
//!
//! # Example
//!
//! ```no_run
//! use aura::{LogLevel, set_log_handler, clear_log_handler, log_emit};
//!
//! set_log_handler(|level, msg| {
//!     eprintln!("[{:?}] {}", level, msg);
//! });
//!
//! log_emit(LogLevel::Info, "engine started");
//!
//! clear_log_handler();
//! ```

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::Mutex;

/// Log severity levels (match syslog priorities 1:1)
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum LogLevel {
    /// Error condition (syslog LOG_ERR = 3)
    Error = 3,
    /// Warning condition (syslog LOG_WARNING = 4)
    Warning = 4,
    /// Normal but significant (syslog LOG_NOTICE = 5)
    Notice = 5,
    /// Informational (syslog LOG_INFO = 6)
    Info = 6,
    /// Debug-level (syslog LOG_DEBUG = 7)
    Debug = 7,
}

impl LogLevel {
    fn to_c(self) -> c_int {
        self as c_int
    }

    fn from_c(level: c_int) -> Self {
        match level as u32 {
            aura_sys::AURA_LOG_ERR => LogLevel::Error,
            aura_sys::AURA_LOG_WARN => LogLevel::Warning,
            aura_sys::AURA_LOG_NOTICE => LogLevel::Notice,
            aura_sys::AURA_LOG_INFO => LogLevel::Info,
            _ => LogLevel::Debug,
        }
    }

    /// Short name for the log level ("ERR", "WARN", etc.)
    pub fn name(self) -> &'static str {
        match self {
            LogLevel::Error => "ERR",
            LogLevel::Warning => "WARN",
            LogLevel::Notice => "NOTICE",
            LogLevel::Info => "INFO",
            LogLevel::Debug => "DEBUG",
        }
    }
}

impl std::fmt::Display for LogLevel {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.name())
    }
}

/// Process-wide handler storage
static LOG_HANDLER: Mutex<Option<Box<dyn Fn(LogLevel, &str) + Send + 'static>>> =
    Mutex::new(None);

/// C callback trampoline — bridges the C log handler to the Rust closure.
extern "C" fn log_trampoline(level: c_int, msg: *const c_char, _userdata: *mut c_void) {
    if msg.is_null() {
        return;
    }

    // catch_unwind prevents panics from unwinding across the FFI boundary
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let msg_str = unsafe { CStr::from_ptr(msg) }.to_str().unwrap_or("");
        let level = LogLevel::from_c(level);

        if let Ok(guard) = LOG_HANDLER.lock() {
            if let Some(handler) = guard.as_ref() {
                handler(level, msg_str);
            }
        }
    }));
}

/// Install a process-wide log handler.
///
/// Replaces any previously installed handler. The handler is called from
/// whichever thread emits the log message; it must be thread-safe.
///
/// # Example
///
/// ```no_run
/// aura::set_log_handler(|level, msg| {
///     eprintln!("[{}] {}", level, msg);
/// });
/// ```
pub fn set_log_handler<F>(handler: F)
where
    F: Fn(LogLevel, &str) + Send + 'static,
{
    {
        let mut guard = LOG_HANDLER.lock().unwrap_or_else(|e| e.into_inner());
        *guard = Some(Box::new(handler));
    }
    unsafe {
        aura_sys::aura_set_log_handler(Some(log_trampoline), std::ptr::null_mut());
    }
}

/// Remove the current log handler.
///
/// After this call the library is silent (default state).
pub fn clear_log_handler() {
    unsafe {
        aura_sys::aura_set_log_handler(None, std::ptr::null_mut());
    }
    let mut guard = LOG_HANDLER.lock().unwrap_or_else(|e| e.into_inner());
    *guard = None;
}

/// Emit a log message through the registered handler (if any).
///
/// No-op when no handler is installed. Thread-safe.
pub fn log_emit(level: LogLevel, msg: &str) {
    if let Ok(c_msg) = CString::new(msg) {
        let fmt = b"%s\0".as_ptr() as *const c_char;
        unsafe {
            aura_sys::aura_log_emit(level.to_c(), fmt, c_msg.as_ptr());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, AtomicI32, AtomicUsize, Ordering};
    use std::sync::Arc;

    /// Serializes tests that mutate the process-wide log handler.
    /// Rust runs tests in parallel by default; without this guard,
    /// concurrent set/clear calls corrupt each other's handler state.
    static LOG_TEST_LOCK: Mutex<()> = Mutex::new(());

    #[test]
    fn test_log_level_ordering() {
        assert!(LogLevel::Error < LogLevel::Warning);
        assert!(LogLevel::Warning < LogLevel::Notice);
        assert!(LogLevel::Notice < LogLevel::Info);
        assert!(LogLevel::Info < LogLevel::Debug);
    }

    #[test]
    fn test_log_level_name() {
        assert_eq!(LogLevel::Error.name(), "ERR");
        assert_eq!(LogLevel::Warning.name(), "WARN");
        assert_eq!(LogLevel::Notice.name(), "NOTICE");
        assert_eq!(LogLevel::Info.name(), "INFO");
        assert_eq!(LogLevel::Debug.name(), "DEBUG");
    }

    #[test]
    fn test_log_level_display() {
        assert_eq!(format!("{}", LogLevel::Error), "ERR");
        assert_eq!(format!("{}", LogLevel::Info), "INFO");
    }

    #[test]
    fn test_log_level_from_c() {
        assert_eq!(LogLevel::from_c(3), LogLevel::Error);
        assert_eq!(LogLevel::from_c(4), LogLevel::Warning);
        assert_eq!(LogLevel::from_c(5), LogLevel::Notice);
        assert_eq!(LogLevel::from_c(6), LogLevel::Info);
        assert_eq!(LogLevel::from_c(7), LogLevel::Debug);
        // Unknown levels default to Debug
        assert_eq!(LogLevel::from_c(99), LogLevel::Debug);
    }

    #[test]
    fn test_log_level_to_c() {
        assert_eq!(LogLevel::Error.to_c(), 3);
        assert_eq!(LogLevel::Warning.to_c(), 4);
        assert_eq!(LogLevel::Notice.to_c(), 5);
        assert_eq!(LogLevel::Info.to_c(), 6);
        assert_eq!(LogLevel::Debug.to_c(), 7);
    }

    #[test]
    fn test_log_emit_no_handler() {
        let _lock = LOG_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        // Should be a no-op, not crash
        clear_log_handler();
        log_emit(LogLevel::Info, "test message");
    }

    #[test]
    fn test_set_and_clear_handler() {
        let _lock = LOG_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let called = Arc::new(AtomicBool::new(false));
        let called_clone = called.clone();

        set_log_handler(move |_level, _msg| {
            called_clone.store(true, Ordering::SeqCst);
        });

        log_emit(LogLevel::Info, "hello");
        assert!(called.load(Ordering::SeqCst));

        clear_log_handler();
    }

    #[test]
    fn test_handler_receives_level_and_message() {
        let _lock = LOG_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let received_level = Arc::new(AtomicI32::new(-1));
        let received_msg = Arc::new(Mutex::new(String::new()));
        let level_clone = received_level.clone();
        let msg_clone = received_msg.clone();

        set_log_handler(move |level, msg| {
            level_clone.store(level.to_c(), Ordering::SeqCst);
            *msg_clone.lock().unwrap() = msg.to_string();
        });

        log_emit(LogLevel::Warning, "test warning");

        assert_eq!(received_level.load(Ordering::SeqCst), 4);
        assert_eq!(*received_msg.lock().unwrap(), "test warning");

        clear_log_handler();
    }

    #[test]
    fn test_handler_all_levels() {
        let _lock = LOG_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let count = Arc::new(AtomicUsize::new(0));
        let count_clone = count.clone();

        set_log_handler(move |_level, _msg| {
            count_clone.fetch_add(1, Ordering::SeqCst);
        });

        log_emit(LogLevel::Error, "e");
        log_emit(LogLevel::Warning, "w");
        log_emit(LogLevel::Notice, "n");
        log_emit(LogLevel::Info, "i");
        log_emit(LogLevel::Debug, "d");

        assert_eq!(count.load(Ordering::SeqCst), 5);

        clear_log_handler();
    }

    #[test]
    fn test_replace_handler() {
        let _lock = LOG_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let first_count = Arc::new(AtomicUsize::new(0));
        let second_count = Arc::new(AtomicUsize::new(0));

        let first_clone = first_count.clone();
        set_log_handler(move |_level, _msg| {
            first_clone.fetch_add(1, Ordering::SeqCst);
        });
        log_emit(LogLevel::Info, "first");

        let second_clone = second_count.clone();
        set_log_handler(move |_level, _msg| {
            second_clone.fetch_add(1, Ordering::SeqCst);
        });
        log_emit(LogLevel::Info, "second");

        assert_eq!(first_count.load(Ordering::SeqCst), 1);
        assert_eq!(second_count.load(Ordering::SeqCst), 1);

        clear_log_handler();
    }
}
