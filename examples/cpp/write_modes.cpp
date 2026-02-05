/**
 * @file write_modes.cpp
 * @brief Demonstrates O_DIRECT vs buffered I/O with AuraIO (C++ version)
 *
 * Shows that AuraIO is agnostic to I/O mode - it works with both
 * O_DIRECT and buffered writes. The adaptive tuning measures actual
 * completion latency regardless of how the kernel handles the I/O.
 *
 * Usage: ./write_modes <file>
 */

#include <auraio.hpp>

#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

using Clock = std::chrono::high_resolution_clock;

constexpr size_t WRITE_SIZE = 64 * 1024;  // 64KB per write
constexpr int NUM_WRITES = 16;            // Number of writes per test

/**
 * Run write test with specified mode.
 */
int run_write_test(const char* filename, bool use_direct) {
    const char* mode_name = use_direct ? "O_DIRECT" : "Buffered";
    std::cout << "\n=== Testing " << mode_name << " writes ===\n";

    try {
        // Create engine
        auraio::Engine engine;

        // Open file with appropriate flags
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (use_direct) {
            flags |= O_DIRECT;
        }

        int fd = open(filename, flags, 0644);
        if (fd < 0) {
            if (use_direct && errno == EINVAL) {
                std::cout << "O_DIRECT not supported on this filesystem, skipping\n";
                return 0;
            }
            throw auraio::Error(errno, filename);
        }

        // Allocate buffer - use aligned for O_DIRECT, regular vector for buffered
        auraio::Buffer aligned_buf;
        std::vector<char> heap_buf;
        void* buf;

        if (use_direct) {
            aligned_buf = engine.allocate_buffer(WRITE_SIZE);
            buf = aligned_buf.data();
            std::cout << "Using aligned buffer from engine.allocate_buffer()\n";
        } else {
            heap_buf.resize(WRITE_SIZE);
            buf = heap_buf.data();
            std::cout << "Using std::vector buffer\n";
        }

        // Fill buffer with pattern
        std::memset(buf, 'A', WRITE_SIZE);

        // Track completion
        int completed = 0;
        auto start_time = Clock::now();
        Clock::time_point end_time;

        // Submit all writes
        std::cout << "Submitting " << NUM_WRITES << " async writes of "
                  << WRITE_SIZE << " bytes each...\n";

        for (int i = 0; i < NUM_WRITES; i++) {
            off_t offset = i * static_cast<off_t>(WRITE_SIZE);
            engine.write(fd, auraio::buf(buf), WRITE_SIZE, offset,
                [&completed, &end_time](auraio::Request&, ssize_t result) {
                    if (result < 0) {
                        std::cerr << "Write failed: " << strerror(static_cast<int>(-result)) << "\n";
                    }
                    completed++;
                    if (completed == NUM_WRITES) {
                        end_time = Clock::now();
                    }
                });
        }

        // Wait for all completions
        while (completed < NUM_WRITES) {
            engine.wait(100);
        }

        // Calculate results
        double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();
        double total_mb = static_cast<double>(NUM_WRITES * WRITE_SIZE) / (1024 * 1024);
        double throughput = total_mb / elapsed_sec;

        std::cout << "Completed " << completed << " writes in "
                  << std::fixed << std::setprecision(3) << elapsed_sec << " seconds\n";
        std::cout << "Total: " << std::setprecision(2) << total_mb
                  << " MB, Throughput: " << throughput << " MB/s\n";

        // Get engine stats
        auto stats = engine.get_stats();
        std::cout << "P99 latency: " << stats.p99_latency_ms() << " ms\n";
        std::cout << "Optimal in-flight: " << stats.optimal_in_flight() << "\n";

        // Fsync to ensure data is on disk
        std::cout << "Fsyncing...\n";
        fsync(fd);

        // Cleanup (buffers cleaned up by RAII)
        close(fd);

        return 0;

    } catch (const auraio::Error& e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        return -1;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file>\n";
        std::cerr << "\nDemonstrates O_DIRECT vs buffered async writes.\n";
        std::cerr << "The file will be created/overwritten.\n";
        return 1;
    }

    const char* filename = argv[1];

    std::cout << "AuraIO Write Modes Example (C++)\n";
    std::cout << "================================\n";
    std::cout << "File: " << filename << "\n";
    std::cout << "Write size: " << WRITE_SIZE << " bytes\n";
    std::cout << "Number of writes: " << NUM_WRITES << "\n";

    // Test buffered I/O first
    if (run_write_test(filename, false) < 0) {
        return 1;
    }

    // Test O_DIRECT
    if (run_write_test(filename, true) < 0) {
        return 1;
    }

    // Cleanup test file
    unlink(filename);

    std::cout << "\n=== Summary ===\n";
    std::cout << "Both modes use the same AuraIO API.\n";
    std::cout << "The library is agnostic - it just submits to io_uring.\n";
    std::cout << "O_DIRECT: Requires aligned buffers, bypasses page cache.\n";
    std::cout << "Buffered: Any buffer works, uses page cache.\n";

    return 0;
}
