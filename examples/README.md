# AuraIO Examples

This directory contains comprehensive examples demonstrating AuraIO's API across C, C++, and Rust.

## Building Examples

Each language has its own build system in a separate subdirectory:

```
examples/
├── C/             # C examples with Makefile
├── cpp/           # C++ examples with Makefile
└── rust/          # Rust examples with Cargo.toml
```

### C Examples

```bash
cd examples/C
make              # Build all C examples
make quickstart   # Build specific example
make help         # Show available targets
```

### C++ Examples

```bash
cd examples/cpp
make              # Build all C++ examples
make quickstart   # Build specific example
make help         # Show available targets
```

### Rust Examples

```bash
cd examples/rust
cargo build --examples                    # Build all examples
cargo run --example quickstart            # Run specific example
```

## Build Modes (C/C++)

Both C and C++ Makefiles **auto-detect** whether to use:

**Installed library** (via pkg-config):
```bash
sudo make install      # Install library first (from repo root)
cd examples/C && make  # Uses installed library
```

**Local development build**:
```bash
make                   # Build library (from repo root)
cd examples/C && make  # Uses local ../../lib/libauraio.so
```

Force local mode: `make USE_LOCAL=1`

## From Repository Root

```bash
# Build everything
make all               # Library + examples + exporters

# Build specific language examples
cd examples/C && make
cd examples/cpp && make
cd examples/rust && cargo build --examples
```


## C Examples (`C/`)

### Basic Examples

**[quickstart.c](C/quickstart.c)** - Minimal working example
- Creates engine with `auraio_create()`
- Allocates buffer with `auraio_buffer_alloc()`
- Submits async read with callback
- Waits for completion
- **Start here** if you're new to AuraIO

**[simple_read.c](C/simple_read.c)** - File reading with statistics
- Demonstrates error handling patterns
- O_DIRECT with fallback to buffered I/O
- Complete statistics API usage (engine, per-ring, histogram, buffer pool)
- Data display and hexdump
- **Usage**: `./C/simple_read <filename>`

### Advanced Features

**[custom_config.c](C/custom_config.c)** ⭐ NEW - Configuration tuning
- Shows `auraio_create_with_options()` for custom engine configuration
- Demonstrates 5 different configurations:
  - **Default**: Auto-configured for system
  - **High Throughput**: Large queues, round-robin selection, high concurrency
  - **Low Latency**: Target P99 latency, CPU-local rings, conservative limits
  - **Adaptive**: Recommended for production - automatic tuning with power-of-two spilling
  - **Custom Ring Count**: Control resource usage on NUMA systems
- Includes performance comparison between configurations
- **When to use**: Production deployments, workload-specific tuning

**[vectored_io.c](C/vectored_io.c)** ⭐ NEW - Scatter-gather I/O
- Demonstrates `auraio_readv()` and `auraio_writev()`
- Shows efficient multi-buffer operations in single syscall
- Example use case: Database page operations (header + data in separate buffers)
- **When to use**: Reading/writing structured file formats, log files with separate metadata/payload sections, reducing syscall overhead when accessing non-contiguous data regions

**[registered_buffers.c](C/registered_buffers.c)** ⭐ NEW - Zero-copy optimization
- Pre-registers buffers with `auraio_register_buffers()`
- Uses `auraio_buf_fixed()` for zero-copy I/O
- Demonstrates deferred unregister pattern (`auraio_request_unregister_buffers()`)
- Includes performance comparison vs unregistered buffers
- **When to use**:
  - ✓ Same buffers reused 1000+ times
  - ✓ High-frequency small I/O (< 16KB)
  - ✓ Zero-copy is critical
  - ✗ One-off operations or dynamic buffer count

### Concurrency & Control

**[bulk_reader.c](C/bulk_reader.c)** - High-throughput concurrent reading
- Reads multiple files concurrently (up to 10,000 files)
- Uses `auraio_run()` blocking event loop
- Demonstrates `auraio_stop()` to signal completion
- Per-operation context passing through `user_data`
- Callback-based resource cleanup
- **Usage**: `./C/bulk_reader <directory>`

**[cancel_request.c](C/cancel_request.c)** - Operation cancellation
- Demonstrates `auraio_cancel()` API
- Shows best-effort cancellation semantics
- Callback receives `-ECANCELED` on successful cancellation
- **Note**: Cancellation may not succeed if operation already completed

### Write Operations

**[write_modes.c](C/write_modes.c)** - Write performance comparison
- Compares O_DIRECT vs buffered async writes
- Shows buffer alignment requirements for O_DIRECT
- Multiple sequential writes from same buffer
- Time measurement with `clock_gettime()`

---

## C++ Examples (`cpp/`)

### Basic Examples

**[quickstart.cpp](cpp/quickstart.cpp)** - Minimal C++20 async read
- Uses RAII `auraio::Engine` wrapper
- Returns managed `auraio::Buffer` object
- Lambda callbacks with local variable capture
- Exception handling with try/catch
- Automatic cleanup via destructors

**[simple_read.cpp](cpp/simple_read.cpp)** - File reading with statistics (C++)
- C++ Statistics object API (`stats.ops_completed()`, `stats.throughput_bps()`, etc.)
- Exception-based error handling
- Lambda closures with reference capture

### Advanced Features

**[custom_config.cpp](cpp/custom_config.cpp)** ⭐ NEW - Configuration tuning (C++)
- Uses `auraio::Options` builder pattern for custom engine configuration
- Demonstrates 5 different configurations:
  - **Default**: Auto-configured for system
  - **High Throughput**: Large queues, round-robin selection, high concurrency
  - **Low Latency**: Target P99 latency, CPU-local rings, conservative limits
  - **Adaptive**: Recommended for production - automatic tuning with power-of-two spilling
  - **Custom Ring Count**: Control resource usage on NUMA systems
- Includes performance comparison between configurations
- **When to use**: Production deployments, workload-specific tuning

**[vectored_io.cpp](cpp/vectored_io.cpp)** ⭐ NEW - Scatter-gather I/O (C++)
- Demonstrates `engine.readv()` and `engine.writev()` with `std::span<const iovec>`
- Shows efficient multi-buffer operations in single syscall
- Example use case: Database page operations (header + data in separate buffers)
- Uses `std::array` and `std::vector` for buffer management
- **When to use**: Reading/writing structured file formats, log files with separate metadata/payload sections

**[registered_buffers.cpp](cpp/registered_buffers.cpp)** ⭐ NEW - Zero-copy optimization (C++)
- Pre-registers buffers with `engine.register_buffers(std::span<const iovec>)`
- Uses `auraio::buf_fixed(idx, offset)` for zero-copy I/O
- Demonstrates deferred unregister pattern (`engine.request_unregister_buffers()`)
- Includes performance comparison vs unregistered buffers
- RAII automatic cleanup when `Engine` and `Buffer` objects go out of scope
- **When to use**:
  - ✓ Same buffers reused 1000+ times
  - ✓ High-frequency small I/O (< 16KB)
  - ✓ Zero-copy is critical
  - ✗ One-off operations or dynamic buffer count

**[bulk_reader.cpp](cpp/bulk_reader.cpp)** - Concurrent bulk read
- Uses `std::atomic<>` for thread-safe counters
- RAII FileContext with move semantics
- Callback closure captures raw pointers (with safety notes)
- `std::filesystem::directory_iterator` for directory scanning
- **Requires**: C++20

**[write_modes.cpp](cpp/write_modes.cpp)** - Write modes comparison (C++)
- Shows both `auraio::Buffer` (managed) and `std::vector<char>` (unmanaged) patterns
- Uses `auraio::buf()` wrapper for non-managed buffers
- High-resolution clock for timing

**[coroutine_copy.cpp](cpp/coroutine_copy.cpp)** - C++20 coroutines
- File copy using `co_await` syntax
- `co_await engine.async_read()` and `co_await engine.async_write()`
- `co_await engine.async_fsync()` for data durability
- `auraio::Task<T>` coroutine return type
- Manual coroutine lifecycle management (resume, done, get)
- **Requires**: C++20 with `-fcoroutines`

---

## Rust Examples (`rust/examples/`)

### Basic Examples

**[quickstart.rs](rust/examples/quickstart.rs)** - Minimal Rust async read
- `Engine::new()` for engine creation
- `engine.allocate_buffer(size)` returns Buffer object
- Unsafe closure callbacks (FFI boundary)
- `Arc<AtomicBool>` for completion synchronization

**[simple_read.rs](rust/examples/simple_read.rs)** - File reading with stats
- Rust Statistics API (method-based: `stats.ops_completed()`, etc.)
- `File::open()` with `AsRawFd` trait
- Result<T> error handling

### Advanced Features

**[custom_config.rs](rust/examples/custom_config.rs)** ⭐ NEW - Configuration tuning (Rust)
- Uses `Options::new()` builder pattern for custom engine configuration
- Demonstrates 5 different configurations:
  - **Default**: Auto-configured for system
  - **High Throughput**: Large queues, round-robin selection, high concurrency
  - **Low Latency**: Target P99 latency, CPU-local rings, conservative limits
  - **Adaptive**: Recommended for production - automatic tuning with power-of-two spilling
  - **Custom Ring Count**: Control resource usage on NUMA systems
- Includes performance comparison between configurations
- **When to use**: Production deployments, workload-specific tuning

**[vectored_io.rs](rust/examples/vectored_io.rs)** ⭐ NEW - Scatter-gather I/O (Rust)
- Demonstrates `engine.readv()` and `engine.writev()` with `&[libc::iovec]`
- Shows efficient multi-buffer operations in single syscall
- Example use case: Database page operations (header + data in separate buffers)
- Uses `Vec<u8>` for buffer management
- **When to use**: Reading/writing structured file formats, log files with separate metadata/payload sections

**[registered_buffers.rs](rust/examples/registered_buffers.rs)** ⭐ NEW - Zero-copy optimization (Rust)
- Pre-registers buffers with `engine.register_buffers(&[&mut [u8]])`
- Uses `BufferRef::fixed(idx, offset)` for zero-copy I/O
- Demonstrates deferred unregister pattern (`engine.request_unregister_buffers()`)
- Includes performance comparison vs unregistered buffers
- Automatic cleanup when `Engine` and buffers drop
- **When to use**:
  - ✓ Same buffers reused 1000+ times
  - ✓ High-frequency small I/O (< 16KB)
  - ✓ Zero-copy is critical
  - ✗ One-off operations or dynamic buffer count

**[file_copy.rs](rust/examples/file_copy.rs)** - Sequential file copy
- Read-then-write pattern for file copy
- `engine.fsync()` operation
- Chunk-based reading/writing loop
- Progress indicator updates

**[async_copy.rs](rust/examples/async_copy.rs)** - Futures-based async copy
- Uses `engine.async_read()`, `engine.async_write()`, `engine.async_fsync()` (return Futures)
- Background poller thread
- Custom `block_on()` executor
- Future polling with `Poll::Ready/Pending`
- Shows async/await abstraction over callback-based API

**[bulk_reader.rs](rust/examples/bulk_reader.rs)** - Concurrent bulk read
- `Arc<Atomic*>` for concurrent progress tracking
- Uses `std::mem::forget()` to prevent premature cleanup
- Raw pointer handling with `BufferRef::from_ptr()`
- **Note**: Simplified compared to C version due to Send trait limitations

**[write_modes.rs](rust/examples/write_modes.rs)** - Write modes (Rust)
- Uses `libc` crate for O_DIRECT flag
- Shows aligned buffer allocation strategy
- Time measurement with `Instant`

---

## Feature Example Matrix

**Note**: All core library features are available in C, C++, and Rust through their respective bindings. The matrix below shows which features have **example code** in each language, not feature availability. Features marked "—" are available but lack example code.

| Feature | C Examples | C++ Examples | Rust Examples | Notes |
|---------|-----------|--------------|---------------|-------|
| Basic async I/O | ✓ (quickstart, simple_read) | ✓ (quickstart, simple_read) | ✓ (quickstart, simple_read) | Core functionality |
| Statistics API | ✓ (simple_read) | ✓ (simple_read) | ✓ (simple_read) | Available in all |
| Histogram monitoring | ✓ (simple_read) | — | — | **TODO**: Add C++/Rust examples |
| Buffer pool stats | ✓ (simple_read) | — | — | **TODO**: Add C++/Rust examples |
| Custom configuration | ✓ (custom_config) | ✓ (custom_config) | ✓ (custom_config) | All languages covered |
| Vectored I/O | ✓ (vectored_io) | ✓ (vectored_io) | ✓ (vectored_io) | All languages covered |
| Registered buffers | ✓ (registered_buffers) | ✓ (registered_buffers) | ✓ (registered_buffers) | All languages covered |
| Cancellation | ✓ (cancel_request) | — | — | Available, needs examples |
| Bulk concurrent I/O | ✓ (bulk_reader) | ✓ (bulk_reader) | ✓ (bulk_reader) | All languages covered |
| Write operations | ✓ (write_modes) | ✓ (write_modes) | ✓ (write_modes) | All languages covered |
| File copy | — | — | ✓ (file_copy) | Rust-specific pattern |
| Coroutines/async-await | — | ✓ (coroutine_copy) | ✓ (async_copy) | Language-specific features |

**Well covered**: All three new features (custom configuration, vectored I/O, registered buffers) now have examples in C, C++, and Rust!

---

## Performance Notes

All examples automatically benefit from recent performance optimizations in AuraIO v0.1.0:

- **Cache-line alignment** eliminates false sharing between concurrent operations
- **Atomic fast-paths** skip lock overhead when features are unused
- **Thread-local caching** improves buffer allocation performance
- **Result**: ~16% higher throughput with zero code changes

**No API changes required** - existing code runs faster automatically.

---

## Choosing the Right Example

**Just getting started?**
- Start with [quickstart.c](C/quickstart.c) (C) or [quickstart.cpp](cpp/quickstart.cpp) (C++) or [quickstart.rs](rust/examples/quickstart.rs) (Rust)

**Need to tune for production?**
- See [custom_config.c](C/custom_config.c) for configuration options
- Recommended: Use `AURAIO_SELECT_ADAPTIVE` ring selection with custom `max_p99_latency_ms`

**Working with protocols or structured data?**
- Use [vectored_io.c](C/vectored_io.c) for efficient multi-buffer operations

**Optimizing for ultra-high frequency I/O?**
- Check [registered_buffers.c](C/registered_buffers.c) for zero-copy patterns
- Best for 1000+ operations on the same buffers

**Building event-driven applications?**
- See [bulk_reader.c](C/bulk_reader.c) for `auraio_run()` event loop pattern
- Or integrate with existing event loops using `auraio_get_poll_fd()` + `auraio_poll()`

**Using modern C++ or Rust?**
- See [coroutine_copy.cpp](cpp/coroutine_copy.cpp) for C++20 coroutines
- See [async_copy.rs](rust/examples/async_copy.rs) for Rust futures

---

## Building Custom Applications

### Common Patterns

1. **Fire-and-forget writes**:
   ```c
   auraio_write(engine, fd, buf, len, offset, NULL, NULL);  /* NULL callback */
   ```

2. **Graceful shutdown**:
   ```c
   auraio_drain(engine, -1);  /* Wait for all pending ops */
   auraio_destroy(engine);
   ```

3. **Event loop integration**:
   ```c
   int poll_fd = auraio_get_poll_fd(engine);
   /* Add poll_fd to epoll/kqueue */
   /* When readable, call auraio_poll(engine) */
   ```

4. **Per-operation contexts**:
   ```c
   typedef struct {
       int fd;
       void *buffer;
       size_t size;
   } my_context_t;

   auraio_read(engine, fd, buf, len, offset, callback, &my_context);
   ```

### Safety Guidelines

- **Never** call `auraio_destroy()` from a callback
- **Never** use request handle after callback starts
- **Always** keep buffers valid until callback completes
- **Prefer** `auraio_request_unregister_buffers()` over `auraio_unregister_buffers()` in callbacks

---

## Additional Resources

- **API Reference**: See [include/auraio.h](../include/auraio.h) for complete API documentation
- **Architecture**: See [docs/CODEBASE_MAP.md](../docs/CODEBASE_MAP.md) for internal design
- **Performance**: See [docs/performance.md](../docs/performance.md) for benchmarks and tuning guide

---

**Questions or Issues?** Check `/help` in Claude Code or report issues at https://github.com/anthropics/claude-code/issues
