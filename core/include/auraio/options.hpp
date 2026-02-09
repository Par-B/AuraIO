/**
 * @file options.hpp
 * @brief Options builder class for AuraIO C++ bindings
 */

#ifndef AURAIO_OPTIONS_HPP
#define AURAIO_OPTIONS_HPP

#include <auraio.h>

namespace auraio {

/**
 * Ring selection mode
 *
 * Controls how submissions are distributed across io_uring rings.
 */
enum class RingSelect {
    Adaptive = AURAIO_SELECT_ADAPTIVE,     ///< CPU-local with overflow spilling (default)
    CpuLocal = AURAIO_SELECT_CPU_LOCAL,    ///< CPU-affinity only (best NUMA locality)
    RoundRobin = AURAIO_SELECT_ROUND_ROBIN ///< Atomic round-robin (max single-thread scaling)
};

/**
 * Engine configuration options
 *
 * Uses builder pattern for fluent configuration.
 *
 * Example:
 * @code
 * auraio::Options opts;
 * opts.queue_depth(512)
 *     .ring_count(4)
 *     .ring_select(auraio::RingSelect::Adaptive);
 *
 * auraio::Engine engine(opts);
 * @endcode
 */
class Options {
  public:
    /**
     * Initialize with default options
     */
    Options() noexcept { auraio_options_init(&opts_); }

    /**
     * Set queue depth per ring
     * @param depth Queue depth (default: 256, valid range: 1-32768)
     * @return Reference to this for chaining
     * @note Validated at engine creation time; invalid values cause Engine() to fail
     */
    Options &queue_depth(int depth) noexcept {
        opts_.queue_depth = depth;
        return *this;
    }

    /**
     * Set number of io_uring rings
     * @param count Number of rings (0 = auto, one per CPU)
     * @return Reference to this for chaining
     */
    Options &ring_count(int count) noexcept {
        opts_.ring_count = count;
        return *this;
    }

    /**
     * Set initial in-flight operation limit
     * @param limit Initial limit
     * @return Reference to this for chaining
     */
    Options &initial_in_flight(int limit) noexcept {
        opts_.initial_in_flight = limit;
        return *this;
    }

    /**
     * Set minimum in-flight operation limit
     * @param limit Minimum limit
     * @return Reference to this for chaining
     */
    Options &min_in_flight(int limit) noexcept {
        opts_.min_in_flight = limit;
        return *this;
    }

    /**
     * Set target maximum P99 latency
     * @param ms Maximum P99 latency in milliseconds (0 = auto)
     * @return Reference to this for chaining
     */
    Options &max_p99_latency_ms(double ms) noexcept {
        opts_.max_p99_latency_ms = ms;
        return *this;
    }

    /**
     * Set buffer alignment
     * @param align Buffer alignment in bytes (default: 4096)
     * @return Reference to this for chaining
     */
    Options &buffer_alignment(size_t align) noexcept {
        opts_.buffer_alignment = align;
        return *this;
    }

    /**
     * Disable adaptive tuning
     * @param disable True to disable adaptive tuning
     * @return Reference to this for chaining
     */
    Options &disable_adaptive(bool disable = true) noexcept {
        opts_.disable_adaptive = disable;
        return *this;
    }

    /**
     * Enable SQPOLL mode
     *
     * Requires root or CAP_SYS_NICE capability.
     *
     * @param enable True to enable SQPOLL mode
     * @return Reference to this for chaining
     */
    Options &enable_sqpoll(bool enable = true) noexcept {
        opts_.enable_sqpoll = enable;
        return *this;
    }

    /**
     * Set SQPOLL idle timeout
     * @param ms Idle timeout in milliseconds
     * @return Reference to this for chaining
     */
    Options &sqpoll_idle_ms(int ms) noexcept {
        opts_.sqpoll_idle_ms = ms;
        return *this;
    }

    /**
     * Set ring selection mode
     * @param mode Ring selection mode (default: Adaptive)
     * @return Reference to this for chaining
     */
    Options &ring_select(RingSelect mode) noexcept {
        opts_.ring_select = static_cast<auraio_ring_select_t>(mode);
        return *this;
    }

    // Getters
    [[nodiscard]] int queue_depth() const noexcept { return opts_.queue_depth; }
    [[nodiscard]] int ring_count() const noexcept { return opts_.ring_count; }
    [[nodiscard]] int initial_in_flight() const noexcept { return opts_.initial_in_flight; }
    [[nodiscard]] int min_in_flight() const noexcept { return opts_.min_in_flight; }
    [[nodiscard]] double max_p99_latency_ms() const noexcept { return opts_.max_p99_latency_ms; }
    [[nodiscard]] size_t buffer_alignment() const noexcept { return opts_.buffer_alignment; }
    [[nodiscard]] bool disable_adaptive() const noexcept { return opts_.disable_adaptive; }
    [[nodiscard]] bool enable_sqpoll() const noexcept { return opts_.enable_sqpoll; }
    [[nodiscard]] int sqpoll_idle_ms() const noexcept { return opts_.sqpoll_idle_ms; }
    [[nodiscard]] RingSelect ring_select() const noexcept {
        auto raw = opts_.ring_select;
        if (raw >= AURAIO_SELECT_ADAPTIVE && raw <= AURAIO_SELECT_ROUND_ROBIN) {
            return static_cast<RingSelect>(raw);
        }
        return RingSelect::Adaptive; // Defensive fallback
    }

    /**
     * Get underlying C options struct
     * @return Reference to auraio_options_t
     */
    [[nodiscard]] const auraio_options_t &c_options() const noexcept { return opts_; }

  private:
    auraio_options_t opts_;
};

} // namespace auraio

#endif // AURAIO_OPTIONS_HPP
