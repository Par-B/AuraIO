/**
 * @file custom_config.cpp
 * @brief Demonstrate AuraIO custom configuration options (C++ version)
 *
 * Shows how to tune the engine for different workload characteristics:
 * - Ring count and queue depth
 * - In-flight limits and target latency
 * - Ring selection strategies
 *
 * Build: make cpp-examples
 * Run:   ./examples/cpp/custom_config
 */

#include <auraio.hpp>

#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

constexpr const char *TEST_FILE = "/tmp/auraio_config_test.dat";
constexpr size_t FILE_SIZE = 4 * 1024 * 1024; // 4 MB
constexpr size_t BUF_SIZE = 4096;
constexpr int NUM_OPS = 20;
constexpr int CONCURRENT_BUFS = 16;

static std::atomic<int> completed{0};

void completion_callback(auraio::Request &, ssize_t result) {
    if (result < 0) {
        std::cerr << "I/O error: " << result << "\n";
    }
    completed.fetch_add(1, std::memory_order_relaxed);
}

void print_stats(const std::string &config_name, auraio::Engine &engine, double elapsed_ms) {
    auto stats = engine.get_stats();

    std::cout << "\n" << config_name << " Configuration:\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Elapsed time: " << elapsed_ms << " ms\n";
    std::cout << "  Operations: " << stats.ops_completed() << "\n";
    std::cout << "  Throughput: " << stats.throughput_bps() / (1024.0 * 1024.0) << " MB/s\n";
    std::cout << std::setprecision(3);
    std::cout << "  P99 Latency: " << stats.p99_latency_ms() << " ms\n";
    std::cout << "  Optimal in-flight: " << stats.optimal_in_flight() << "\n";
    std::cout << "  Optimal batch size: " << stats.optimal_batch_size() << "\n";
}

void run_workload(auraio::Engine &engine, int fd, const std::string &config_name) {
    // Allocate buffers for concurrent operations
    std::vector<auraio::Buffer> bufs;
    bufs.reserve(CONCURRENT_BUFS);
    for (int i = 0; i < CONCURRENT_BUFS; i++) {
        bufs.push_back(engine.allocate_buffer(BUF_SIZE));
    }

    completed.store(0, std::memory_order_relaxed);
    auto start = std::chrono::steady_clock::now();

    // Submit NUM_OPS reads at random offsets with proper pacing
    int submitted = 0;
    while (submitted < NUM_OPS || completed.load(std::memory_order_relaxed) < NUM_OPS) {
        // Submit new operations while under the concurrency limit
        while (submitted < NUM_OPS &&
               (submitted - completed.load(std::memory_order_relaxed)) < CONCURRENT_BUFS / 2) {
            off_t offset = (std::rand() % (FILE_SIZE / BUF_SIZE)) * BUF_SIZE;
            auto &buf = bufs[submitted % CONCURRENT_BUFS];

            try {
                (void)engine.read(fd, buf, BUF_SIZE, offset, completion_callback);
                submitted++;
            } catch (const auraio::Error &e) {
                std::cerr << "Read submission failed: " << e.what() << "\n";
                break;
            }
        }

        // Poll for completions
        engine.poll();

        // If we're done submitting, wait for remaining completions
        if (submitted >= NUM_OPS && completed.load(std::memory_order_relaxed) < NUM_OPS) {
            engine.wait(1);
        }
    }

    auto end = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration<double, std::milli>(end - start).count();

    print_stats(config_name, engine, elapsed);
}

int main() {
    std::cout << "AuraIO Custom Configuration Examples (C++)\n";
    std::cout << "===========================================\n\n";

    try {
        // Create test file
        int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd < 0) {
            throw auraio::Error(errno, "create test file");
        }

        std::vector<char> data(FILE_SIZE, 0);
        if (write(wfd, data.data(), FILE_SIZE) != static_cast<ssize_t>(FILE_SIZE)) {
            close(wfd);
            unlink(TEST_FILE);
            throw auraio::Error(errno, "write test file");
        }
        close(wfd);

        // Open for reading
        int fd = open(TEST_FILE, O_RDONLY);
        if (fd < 0) {
            unlink(TEST_FILE);
            throw auraio::Error(errno, "open test file");
        }

        // ===================================================================
        // Example 1: Default Configuration
        // ===================================================================
        std::cout << "Running with default configuration...\n";
        {
            auraio::Engine engine_default;
            run_workload(engine_default, fd, "Default");
        }

        // ===================================================================
        // Example 2: High-Throughput Configuration
        // - Larger queue depth for more pipelining
        // - Higher initial in-flight limit
        // - Round-robin ring selection for single-thread scaling
        // ===================================================================
        std::cout << "\nConfiguring for high throughput...\n";
        {
            auraio::Options opts_throughput;
            opts_throughput
                .queue_depth(512)                             // Deeper queues for more pipelining
                .initial_in_flight(128)                       // Start with high concurrency
                .ring_select(auraio::RingSelect::RoundRobin); // Max single-thread scaling

            auraio::Engine engine_throughput(opts_throughput);
            run_workload(engine_throughput, fd, "High Throughput");
        }

        // ===================================================================
        // Example 3: Low-Latency Configuration
        // - Target specific P99 latency
        // - Conservative in-flight limit to reduce queuing
        // - CPU-local ring selection for best cache locality
        // ===================================================================
        std::cout << "\nConfiguring for low latency...\n";
        {
            auraio::Options opts_latency;
            opts_latency
                .max_p99_latency_ms(1.0)                    // Target 1ms P99
                .initial_in_flight(8)                       // Start conservative
                .min_in_flight(4)                           // Never go below 4
                .ring_select(auraio::RingSelect::CpuLocal); // Best cache locality

            auraio::Engine engine_latency(opts_latency);
            run_workload(engine_latency, fd, "Low Latency");
        }

        // ===================================================================
        // Example 4: Adaptive Configuration (Recommended for Production)
        // - Let AIMD tuning find optimal settings
        // - Adaptive ring selection with congestion-based spilling
        // - Moderate queue depth for good balance
        // ===================================================================
        std::cout << "\nConfiguring for adaptive tuning (production recommended)...\n";
        {
            auraio::Options opts_adaptive;
            opts_adaptive
                .queue_depth(256)                          // Balanced queue depth
                .ring_select(auraio::RingSelect::Adaptive) // Power-of-two spilling
                .max_p99_latency_ms(5.0);                  // Reasonable latency target

            auraio::Engine engine_adaptive(opts_adaptive);
            run_workload(engine_adaptive, fd, "Adaptive (Recommended)");
        }

        // ===================================================================
        // Example 5: Custom Ring Count
        // - Useful for NUMA systems or limiting resource usage
        // ===================================================================
        std::cout << "\nConfiguring with custom ring count...\n";
        {
            auraio::Options opts_custom_rings;
            opts_custom_rings
                .ring_count(2) // Use only 2 rings (vs default per-CPU)
                .queue_depth(256);

            auraio::Engine engine_custom(opts_custom_rings);
            run_workload(engine_custom, fd, "Custom Ring Count (2)");
        }

        // Cleanup
        close(fd);
        unlink(TEST_FILE);

        std::cout << "\n======================================\n";
        std::cout << "Configuration Summary:\n";
        std::cout << "- Default: Auto-configured for system\n";
        std::cout << "- High Throughput: Large queues, round-robin, high concurrency\n";
        std::cout << "- Low Latency: CPU-local, target P99, conservative limits\n";
        std::cout << "- Adaptive: RECOMMENDED for production - automatic tuning\n";
        std::cout << "- Custom Rings: Control resource usage\n";
        std::cout << "\nChoose configuration based on your workload characteristics.\n";

        return 0;

    } catch (const auraio::Error &e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
