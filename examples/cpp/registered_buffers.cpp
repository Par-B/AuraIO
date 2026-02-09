/**
 * @file registered_buffers.cpp
 * @brief Demonstrate registered (fixed) buffers for zero-copy I/O (C++ version)
 *
 * Pre-registering buffers with the kernel eliminates per-operation mapping
 * overhead. Best for workloads with:
 * - Same buffers reused across 1000+ I/O operations
 * - High-frequency small I/O (< 16KB) where mapping overhead is significant
 * - Zero-copy is critical for performance
 *
 * Build: make cpp-examples
 * Run:   ./examples/cpp/registered_buffers
 */

#include <auraio.hpp>

#include <iostream>
#include <iomanip>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

constexpr const char *TEST_FILE = "/tmp/auraio_reg_buf_test.dat";
constexpr size_t FILE_SIZE = 1 * 1024 * 1024; // 1 MB
constexpr size_t BUF_SIZE = 4096;
constexpr int NUM_BUFFERS = 4;
constexpr int NUM_OPS = 50;

static std::atomic<int> completed{0};

void completion_callback(auraio::Request &, ssize_t result) {
    if (result < 0) {
        std::cerr << "I/O error: " << result << "\n";
    }
    completed.fetch_add(1, std::memory_order_relaxed);
}

double run_benchmark(auraio::Engine &engine, int fd, bool use_registered,
                     std::vector<auraio::Buffer> &buffers) {
    completed.store(0, std::memory_order_relaxed);
    auto start = std::chrono::steady_clock::now();

    // Submit NUM_OPS reads using registered or unregistered buffers with proper pacing
    int submitted = 0;
    int max_inflight = use_registered ? NUM_BUFFERS : 8;

    while (submitted < NUM_OPS || completed.load(std::memory_order_relaxed) < NUM_OPS) {
        // Submit new operations while under the concurrency limit
        while (submitted < NUM_OPS &&
               (submitted - completed.load(std::memory_order_relaxed)) < max_inflight) {
            int buf_idx = submitted % NUM_BUFFERS;
            off_t offset = (std::rand() % (FILE_SIZE / BUF_SIZE)) * BUF_SIZE;

            try {
                if (use_registered) {
                    // Use registered buffer by index
                    (void)engine.read(fd, auraio::buf_fixed(buf_idx, 0), BUF_SIZE, offset,
                                      completion_callback);
                } else {
                    // Use unregistered buffer
                    (void)engine.read(fd, buffers[buf_idx], BUF_SIZE, offset, completion_callback);
                }
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
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main() {
    std::cout << "AuraIO Registered Buffers Example (C++)\n";
    std::cout << "========================================\n\n";

    try {
        // Create test file
        std::cout << "Creating test file (" << FILE_SIZE / (1024 * 1024) << " MB)...\n";
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
        // Part 1: Unregistered Buffers (Baseline)
        // ===================================================================
        std::cout << "\nPart 1: Baseline with unregistered buffers\n";
        std::cout << "Running " << NUM_OPS << " operations...\n";

        {
            auraio::Engine engine_unreg;

            // Allocate buffers
            std::vector<auraio::Buffer> unreg_buffers;
            unreg_buffers.reserve(NUM_BUFFERS);
            for (int i = 0; i < NUM_BUFFERS; i++) {
                unreg_buffers.push_back(engine_unreg.allocate_buffer(BUF_SIZE));
            }

            double unreg_time = run_benchmark(engine_unreg, fd, false, unreg_buffers);

            auto unreg_stats = engine_unreg.get_stats();

            std::cout << std::fixed << std::setprecision(2);
            std::cout << "  Time: " << unreg_time << " ms\n";
            std::cout << "  Throughput: " << unreg_stats.throughput_bps() / (1024.0 * 1024.0)
                      << " MB/s\n";
            std::cout << std::setprecision(3);
            std::cout << "  P99 Latency: " << unreg_stats.p99_latency_ms() << " ms\n";
        }

        // ===================================================================
        // Part 2: Registered Buffers (Zero-Copy)
        // ===================================================================
        std::cout << "\nPart 2: Zero-copy with registered buffers\n";
        std::cout << "Registering " << NUM_BUFFERS << " buffers of " << BUF_SIZE
                  << " bytes each...\n";

        {
            auraio::Engine engine_reg;

            // Allocate buffers
            std::vector<auraio::Buffer> reg_buffers;
            reg_buffers.reserve(NUM_BUFFERS);
            for (int i = 0; i < NUM_BUFFERS; i++) {
                reg_buffers.push_back(engine_reg.allocate_buffer(BUF_SIZE));
            }

            // Prepare iovec array for registration
            std::vector<iovec> iovs;
            iovs.reserve(NUM_BUFFERS);
            for (auto &buf : reg_buffers) {
                iovs.emplace_back(iovec{buf.data(), BUF_SIZE});
            }

            // Register buffers with kernel
            engine_reg.register_buffers(iovs);

            std::cout << "Buffers registered successfully.\n";
            std::cout << "Running " << NUM_OPS << " operations with zero-copy I/O...\n";

            double reg_time = run_benchmark(engine_reg, fd, true, reg_buffers);

            auto reg_stats = engine_reg.get_stats();

            std::cout << std::fixed << std::setprecision(2);
            std::cout << "  Time: " << reg_time << " ms\n";
            std::cout << "  Throughput: " << reg_stats.throughput_bps() / (1024.0 * 1024.0)
                      << " MB/s\n";
            std::cout << std::setprecision(3);
            std::cout << "  P99 Latency: " << reg_stats.p99_latency_ms() << " ms\n";

            // ===================================================================
            // Part 3: Performance Comparison
            // ===================================================================
            std::cout << "\n======================================\n";
            std::cout << "Performance Comparison:\n";
            std::cout << std::fixed << std::setprecision(2);
            std::cout << "  Unregistered: " << run_benchmark(engine_reg, fd, false, reg_buffers)
                      << " ms\n";
            std::cout << "  Registered:   " << reg_time << " ms\n";
            std::cout << "  Speedup:      "
                      << run_benchmark(engine_reg, fd, false, reg_buffers) / reg_time << "x\n";

            // ===================================================================
            // Part 4: Deferred Buffer Unregister (Callback-Safe Pattern)
            // ===================================================================
            std::cout << "\nPart 4: Deferred buffer unregister\n";
            std::cout << "This pattern allows safe unregister from callback context...\n";

            // Request deferred unregister (returns immediately)
            engine_reg.request_unregister_buffers();
            std::cout << "  Unregister requested (will complete when in-flight ops drain)\n";

            // Wait a bit to ensure unregister completes
            engine_reg.wait(100);

            std::cout << "  Buffers unregistered.\n";

            // Buffers are automatically freed when engine_reg and reg_buffers go out of scope
        }

        // Cleanup
        close(fd);
        unlink(TEST_FILE);

        std::cout << "\n======================================\n";
        std::cout << "When to Use Registered Buffers:\n";
        std::cout << "  ✓ Same buffers reused 1000+ times\n";
        std::cout << "  ✓ High-frequency small I/O (< 16KB)\n";
        std::cout << "  ✓ Zero-copy is critical\n";
        std::cout << "  ✗ One-off or infrequent operations\n";
        std::cout << "  ✗ Dynamic buffer count\n";
        std::cout << "  ✗ Simpler code without registration\n";

        return 0;

    } catch (const auraio::Error &e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        unlink(TEST_FILE);
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        unlink(TEST_FILE);
        return 1;
    }
}
