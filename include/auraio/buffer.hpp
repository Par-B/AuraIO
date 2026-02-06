/**
 * @file buffer.hpp
 * @brief Buffer and BufferRef classes for AuraIO C++ bindings
 */

#ifndef AURAIO_BUFFER_HPP
#define AURAIO_BUFFER_HPP

#include <auraio.h>
#include <auraio/fwd.hpp>
#include <auraio/error.hpp>
#include <span>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace auraio {

/**
 * Lightweight buffer descriptor
 *
 * Wraps auraio_buf_t for passing to I/O operations.
 * Can represent either an unregistered buffer (raw pointer)
 * or a registered buffer (index + offset).
 *
 * This is a value type with no ownership semantics.
 */
class BufferRef {
public:
    /**
     * Construct from raw pointer (unregistered buffer)
     * @param ptr Buffer pointer
     */
    BufferRef(void* ptr) noexcept : buf_(auraio_buf(ptr)) {}

    /**
     * Construct from const raw pointer (for write operations only)
     *
     * Explicit to prevent accidental use with read operations, which
     * would write into the buffer and cause undefined behavior.
     *
     * @param ptr Buffer pointer (must not be used with read operations)
     */
    explicit BufferRef(const void* ptr) noexcept
        : buf_(auraio_buf(const_cast<void*>(ptr))) {}

    /**
     * Construct for registered buffer
     * @param index Registered buffer index
     * @param offset Offset within registered buffer
     * @return BufferRef for registered buffer
     */
    [[nodiscard]] static BufferRef fixed(int index, size_t offset = 0) noexcept {
        BufferRef ref;
        ref.buf_ = auraio_buf_fixed(index, offset);
        return ref;
    }

    /**
     * Check if this is a registered buffer
     * @return True if registered buffer
     */
    [[nodiscard]] bool is_registered() const noexcept {
        return buf_.type == AURAIO_BUF_REGISTERED;
    }

    /**
     * Get underlying C buffer descriptor
     * @return auraio_buf_t value
     */
    [[nodiscard]] auraio_buf_t c_buf() const noexcept { return buf_; }

private:
    BufferRef() noexcept = default;
    auraio_buf_t buf_{};
};

/**
 * Convenience function to create BufferRef from pointer
 * @param ptr Buffer pointer
 * @return BufferRef for unregistered buffer
 */
inline BufferRef buf(void* ptr) noexcept {
    return BufferRef(ptr);
}

/**
 * Convenience function to create BufferRef for registered buffer
 * @param index Registered buffer index
 * @param offset Offset within buffer (default: 0)
 * @return BufferRef for registered buffer
 */
inline BufferRef buf_fixed(int index, size_t offset = 0) noexcept {
    return BufferRef::fixed(index, offset);
}

/**
 * RAII buffer allocated from engine's pool
 *
 * Automatically returns buffer to pool on destruction.
 * Move-only (cannot be copied).
 *
 * Example:
 * @code
 * auto buffer = engine.allocate_buffer(4096);
 * engine.read(fd, buffer, 4096, 0, callback);
 * // buffer automatically freed when it goes out of scope
 * @endcode
 */
class Buffer {
public:
    /**
     * Default constructor - creates empty buffer
     */
    Buffer() noexcept = default;

    /**
     * Move constructor
     */
    Buffer(Buffer&& other) noexcept
        : engine_(other.engine_)
        , ptr_(other.ptr_)
        , size_(other.size_)
        , owned_(other.owned_)
    {
        other.engine_ = nullptr;
        other.ptr_ = nullptr;
        other.size_ = 0;
        other.owned_ = false;
    }

    /**
     * Move assignment
     */
    Buffer& operator=(Buffer&& other) noexcept {
        if (this != &other) {
            release_internal();
            engine_ = other.engine_;
            ptr_ = other.ptr_;
            size_ = other.size_;
            owned_ = other.owned_;
            other.engine_ = nullptr;
            other.ptr_ = nullptr;
            other.size_ = 0;
            other.owned_ = false;
        }
        return *this;
    }

    // Non-copyable
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    /**
     * Destructor - returns buffer to pool if owned
     */
    ~Buffer() {
        release_internal();
    }

    /**
     * Wrap existing memory (non-owning)
     *
     * Creates a Buffer that does NOT free the memory on destruction.
     * Caller is responsible for the memory's lifetime.
     *
     * @param ptr Pointer to existing memory
     * @param size Size of memory region
     * @return Non-owning Buffer wrapper
     */
    [[nodiscard]] static Buffer wrap(void* ptr, size_t size) noexcept {
        Buffer buf;
        buf.ptr_ = ptr;
        buf.size_ = size;
        buf.owned_ = false;
        return buf;
    }

    /**
     * Get buffer data pointer
     * @return Pointer to buffer data
     */
    [[nodiscard]] void* data() noexcept { return ptr_; }

    /**
     * Get buffer data pointer (const)
     * @return Const pointer to buffer data
     */
    [[nodiscard]] const void* data() const noexcept { return ptr_; }

    /**
     * Get buffer size
     * @return Buffer size in bytes
     */
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /**
     * Get buffer as span of bytes
     * @return std::span over buffer contents
     */
    [[nodiscard]] std::span<std::byte> span() noexcept {
        return {static_cast<std::byte*>(ptr_), size_};
    }

    /**
     * Get buffer as const span of bytes
     * @return std::span over buffer contents (const)
     */
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {static_cast<const std::byte*>(ptr_), size_};
    }

    /**
     * Get buffer as span of specific type
     * @tparam T Element type
     * @return std::span of T elements
     */
    template<typename T>
    [[nodiscard]] std::span<T> as() {
        if (reinterpret_cast<std::uintptr_t>(ptr_) % alignof(T) != 0) {
            throw Error(EINVAL, "Buffer not aligned for requested type");
        }
        // Truncates: a 4097-byte buffer as<uint32_t>() returns 1024 elements
        return {static_cast<T*>(ptr_), size_ / sizeof(T)};
    }

    /**
     * Get buffer as const span of specific type
     * @tparam T Element type
     * @return std::span of const T elements
     * @throws Error if buffer is not properly aligned for T
     * @note Trailing bytes smaller than sizeof(T) are excluded from the span
     */
    template<typename T>
    [[nodiscard]] std::span<const T> as() const {
        if (reinterpret_cast<std::uintptr_t>(ptr_) % alignof(T) != 0) {
            throw Error(EINVAL, "Buffer not aligned for requested type");
        }
        return {static_cast<const T*>(ptr_), size_ / sizeof(T)};
    }

    /**
     * Convert to BufferRef for I/O operations
     * @return BufferRef pointing to this buffer
     */
    [[nodiscard]] BufferRef ref() const noexcept {
        return BufferRef(ptr_);
    }

    /**
     * Implicit conversion to BufferRef
     */
    operator BufferRef() const noexcept {
        return ref();
    }

    /**
     * Check if buffer is valid (non-null)
     * @return True if buffer has valid data
     */
    [[nodiscard]] explicit operator bool() const noexcept {
        return ptr_ != nullptr;
    }

    /**
     * Check if buffer is owned (will be freed on destruction)
     * @return True if owned
     */
    [[nodiscard]] bool owned() const noexcept { return owned_; }

    /**
     * Result of releasing buffer ownership
     */
    struct ReleasedBuffer {
        void* data;               /**< Buffer pointer */
        size_t size;              /**< Buffer size in bytes */
        auraio_engine_t* engine;  /**< Engine that owns the pool (nullptr if not pool-allocated) */
    };

    /**
     * Release ownership of buffer
     *
     * After calling release(), the Buffer will not free the memory
     * on destruction. Caller becomes responsible for freeing via
     * auraio_buffer_free(released.engine, released.data, released.size).
     *
     * @return ReleasedBuffer with pointer, size, and engine needed for freeing
     */
    [[nodiscard]] ReleasedBuffer release() noexcept {
        ReleasedBuffer released{ptr_, size_, engine_};
        ptr_ = nullptr;
        size_ = 0;
        owned_ = false;
        engine_ = nullptr;
        return released;
    }

private:
    friend class Engine;

    // Private constructor for Engine::allocate_buffer
    Buffer(auraio_engine_t* engine, void* ptr, size_t size) noexcept
        : engine_(engine)
        , ptr_(ptr)
        , size_(size)
        , owned_(true)
    {}

    void release_internal() noexcept;

    auraio_engine_t* engine_ = nullptr;
    void* ptr_ = nullptr;
    size_t size_ = 0;
    bool owned_ = false;
};

// Implementation of release_internal (needs auraio.h)
inline void Buffer::release_internal() noexcept {
    if (owned_ && ptr_ && engine_) {
        auraio_buffer_free(engine_, ptr_, size_);
    }
    ptr_ = nullptr;
    size_ = 0;
    owned_ = false;
    engine_ = nullptr;
}

} // namespace auraio

#endif // AURAIO_BUFFER_HPP
