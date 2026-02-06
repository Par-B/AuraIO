/**
 * @file callback_storage.hpp
 * @brief Internal callback storage for AuraIO C++ bindings
 *
 * This is an internal header - not part of the public API.
 */

#ifndef AURAIO_DETAIL_CALLBACK_STORAGE_HPP
#define AURAIO_DETAIL_CALLBACK_STORAGE_HPP

#include <auraio.h>
#include <auraio/request.hpp>
#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace auraio::detail {

/**
 * Callback context for type-erased callback storage
 *
 * Stores the C++ callback and provides a trampoline function
 * that can be passed to the C API.
 */
struct CallbackContext {
    std::function<void(Request&, ssize_t)> callback;
    std::function<void()> on_complete;  // Called after callback to release context (O(1) cleanup)
    auraio_engine_t* engine = nullptr;
    Request request{nullptr};  // Per-operation Request storage (avoids thread-local static)
    int next_free = -1;   // Index of next free slot (-1 = in use or end of list)
    int pool_index = -1;  // Own index within shard (for O(1) release)
    int shard_index = -1; // Which shard this context belongs to
};

/**
 * Pool of callback contexts with sharding for reduced contention
 *
 * Uses multiple shards to reduce mutex contention on many-core systems.
 * Each thread hashes to a specific shard, so threads on different cores
 * typically don't contend with each other.
 *
 * Uses std::deque internally, which guarantees that existing element
 * pointers remain valid when the container grows. This is critical
 * for high-concurrency scenarios where growth may occur while
 * callbacks are in flight.
 *
 * Thread-safe.
 */
class CallbackPool {
public:
    /**
     * Number of shards (power of 2 for efficient modulo)
     *
     * 8 shards provides good balance:
     * - Small systems (2-4 cores): minimal overhead, ~2 threads per shard max
     * - Medium systems (8-32 cores): ~1-4 threads per shard
     * - Large systems (64+ cores): ~8+ threads per shard, still much better than 1 lock
     */
    static constexpr size_t kShardCount = 8;
    static constexpr size_t kShardMask = kShardCount - 1;  // For fast modulo

    explicit CallbackPool(auraio_engine_t* engine, size_t initial_size_per_shard = 16)
        : engine_(engine)
    {
        for (size_t s = 0; s < kShardCount; ++s) {
            init_shard(s, initial_size_per_shard);
        }
    }

    /**
     * Allocate a callback context - O(1) amortized
     * @return Pointer to allocated context (never null - grows as needed)
     *
     * Thread selection is based on thread ID hash, so threads typically
     * access different shards and don't contend.
     */
    CallbackContext* allocate() {
        size_t shard_idx = get_shard_index();
        Shard& shard = shards_[shard_idx];
        std::lock_guard<std::mutex> lock(shard.mutex);

        // Pop from free list head
        if (shard.free_head >= 0) {
            int idx = shard.free_head;
            CallbackContext* ctx = &shard.contexts[idx];
            shard.free_head = ctx->next_free;
            ctx->next_free = -1;  // Mark as in-use
            return ctx;
        }

        // Free list empty - grow this shard
        return grow_shard(shard, shard_idx);
    }

    /**
     * Release a callback context back to pool - O(1)
     * @param ctx Context to release (uses stored shard_index for routing)
     */
    void release(CallbackContext* ctx) {
        if (!ctx || ctx->shard_index < 0) {
            return;
        }

        Shard& shard = shards_[ctx->shard_index];
        std::lock_guard<std::mutex> lock(shard.mutex);

        ctx->callback = nullptr;
        ctx->on_complete = nullptr;
        // Push to free list head
        ctx->next_free = shard.free_head;
        shard.free_head = ctx->pool_index;
    }

private:
    struct Shard {
        std::deque<CallbackContext> contexts;  // deque: pointers stable on growth
        std::mutex mutex;
        int free_head = -1;  // Index of first free slot (-1 = empty)
    };

    void init_shard(size_t shard_idx, size_t initial_size) {
        Shard& shard = shards_[shard_idx];
        if (initial_size == 0) {
            shard.free_head = -1;
            return;
        }
        shard.contexts.resize(initial_size);
        shard.free_head = 0;

        // Initialize free list: each slot points to the next
        for (size_t i = 0; i < initial_size; ++i) {
            CallbackContext& ctx = shard.contexts[i];
            ctx.engine = engine_;
            ctx.pool_index = static_cast<int>(i);
            ctx.shard_index = static_cast<int>(shard_idx);
            ctx.next_free = static_cast<int>(i + 1);
        }
        // Last slot marks end of free list
        shard.contexts[initial_size - 1].next_free = -1;
    }

    // Must be called with shard.mutex held
    CallbackContext* grow_shard(Shard& shard, size_t shard_idx) {
        size_t old_size = shard.contexts.size();
        size_t new_size = old_size * 2;
        shard.contexts.resize(new_size);

        // Initialize new slots as a free list
        for (size_t i = old_size; i < new_size; ++i) {
            CallbackContext& ctx = shard.contexts[i];
            ctx.engine = engine_;
            ctx.pool_index = static_cast<int>(i);
            ctx.shard_index = static_cast<int>(shard_idx);
            ctx.next_free = static_cast<int>(i + 1);
        }
        shard.contexts[new_size - 1].next_free = -1;

        // Return first new slot, rest become the free list
        shard.free_head = static_cast<int>(old_size + 1);
        shard.contexts[old_size].next_free = -1;  // Mark as in-use
        return &shard.contexts[old_size];
    }

    /**
     * Get shard index for current thread
     *
     * Uses cached thread-local value to avoid repeated hashing.
     * The hash spreads threads across shards for good distribution.
     */
    static size_t get_shard_index() {
        // Thread-local cache avoids repeated hashing
        thread_local size_t cached_shard = compute_shard_index();
        return cached_shard;
    }

    static size_t compute_shard_index() {
        // Hash thread ID and mask to shard count
        auto tid = std::this_thread::get_id();
        size_t hash = std::hash<std::thread::id>{}(tid);
        return hash & kShardMask;
    }

    auraio_engine_t* engine_;
    std::array<Shard, kShardCount> shards_;
};

/**
 * C callback trampoline function
 *
 * This function is passed to the C API and converts the callback
 * to the C++ callback stored in the context.
 */
inline void callback_trampoline(auraio_request_t* req, ssize_t result, void* user_data) {
    auto* ctx = static_cast<CallbackContext*>(user_data);
    if (ctx && ctx->callback) {
        ctx->request = Request(req);
        ctx->callback(ctx->request, result);
    }
    // Release context immediately via on_complete (O(1) instead of polling)
    if (ctx && ctx->on_complete) {
        ctx->on_complete();
    }
}

} // namespace auraio::detail

#endif // AURAIO_DETAIL_CALLBACK_STORAGE_HPP
