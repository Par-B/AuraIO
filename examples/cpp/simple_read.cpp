/**
 * @file simple_read.cpp
 * @brief Simple async file read example (C++ version)
 *
 * Demonstrates basic usage of the AuraIO C++ library.
 *
 * Usage: ./simple_read <file>
 */

#include <aura.hpp>

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

constexpr size_t READ_SIZE = 1024 * 1024; // 1MB

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file>\n";
        std::cerr << "\nReads the first 1MB of a file asynchronously.\n";
        return 1;
    }

    const char *filename = argv[1];

    try {
        // Create the aura engine
        std::cout << "Creating async I/O engine...\n";
        aura::Engine engine;

        // Open file with O_DIRECT for best async I/O performance
        int fd = open(filename, O_RDONLY | O_DIRECT);
        if (fd < 0 && errno == EINVAL) {
            // O_DIRECT not supported on this filesystem - fall back
            fd = open(filename, O_RDONLY);
            std::cout << "Note: O_DIRECT not available, using buffered I/O\n";
        }
        if (fd < 0) {
            throw aura::Error(errno, filename);
        }

        // Allocate aligned buffer from engine's pool (RAII)
        auto buffer = engine.allocate_buffer(READ_SIZE);

        // Track completion with local variables captured by lambda
        bool done = false;
        ssize_t result = 0;

        // Submit async read
        std::cout << "Submitting async read of " << READ_SIZE << " bytes...\n";
        (void)engine.read(fd, buffer, READ_SIZE, 0, [&](aura::Request &, ssize_t res) {
            if (res < 0) {
                std::cerr << "Read failed: " << strerror(static_cast<int>(-res)) << "\n";
            } else {
                std::cout << "Read " << res << " bytes successfully\n";
            }
            result = res;
            done = true;
        });

        // Wait for completion
        std::cout << "Waiting for completion...\n";
        while (!done) {
            engine.wait(100); // 100ms timeout
        }

        // Show first few bytes of data
        if (result > 0) {
            std::cout << "\nFirst 64 bytes of file:\n";
            auto *data = static_cast<unsigned char *>(buffer.data());
            for (int i = 0; i < 64 && i < result; i++) {
                unsigned char c = data[i];
                if (c >= 32 && c < 127) {
                    std::cout << static_cast<char>(c);
                } else {
                    std::cout << "\\x" << std::hex << std::setfill('0') << std::setw(2)
                              << static_cast<int>(c);
                    std::cout << std::dec; // Reset to decimal
                }
            }
            std::cout << "\n";
        }

        // Get statistics
        auto stats = engine.get_stats();
        std::cout << "\nEngine statistics:\n";
        std::cout << "  Ops completed:     " << stats.ops_completed() << "\n";
        std::cout << "  Bytes transferred: " << stats.bytes_transferred() << "\n";
        std::cout << "  Throughput:        " << std::fixed << std::setprecision(2)
                  << stats.throughput_bps() / (1024 * 1024) << " MB/s\n";
        std::cout << "  P99 latency:       " << stats.p99_latency_ms() << " ms\n";
        std::cout << "  Optimal in-flight: " << stats.optimal_in_flight() << "\n";
        std::cout << "  Optimal batch:     " << stats.optimal_batch_size() << "\n";

        // Per-ring statistics (demonstrates ring_count() and get_ring_stats())
        int rings = engine.ring_count();
        for (int i = 0; i < rings; i++) {
            auto ring_stats = engine.get_ring_stats(i);
            std::cout << "  Ring " << i << ": phase=" << ring_stats.aimd_phase_name()
                      << " depth=" << ring_stats.pending_count() << "/"
                      << ring_stats.in_flight_limit() << "\n";
        }

        // Latency histogram (demonstrates get_histogram())
        if (rings > 0) {
            auto hist = engine.get_histogram(0);
            if (hist.total_count() > 0) {
                std::cout << "\nLatency Histogram (Ring 0):\n";
                std::cout << "  Total samples: " << hist.total_count() << "\n";
                std::cout << "  Max tracked:   " << hist.max_tracked_us() << " μs\n";

                // Compute percentiles using the public API
                double p50 = hist.percentile(50.0);
                double p90 = hist.percentile(90.0);
                double p99 = hist.percentile(99.0);
                double p999 = hist.percentile(99.9);

                std::cout << std::fixed << std::setprecision(2);
                if (p50 >= 0) std::cout << "  P50 latency:   " << p50 << " ms\n";
                if (p90 >= 0) std::cout << "  P90 latency:   " << p90 << " ms\n";
                if (p99 >= 0) std::cout << "  P99 latency:   " << p99 << " ms\n";
                if (p999 >= 0) std::cout << "  P99.9 latency: " << p999 << " ms\n";
                if (hist.overflow() > 0) {
                    std::cout << "  Overflow:      " << hist.overflow() << " samples (> "
                              << hist.max_tracked_us() << " μs)\n";
                }
            }
        }

        // Buffer pool statistics (demonstrates get_buffer_stats())
        auto buf_stats = engine.get_buffer_stats();
        std::cout << "\nBuffer Pool Statistics:\n";
        std::cout << "  Total allocated:  " << buf_stats.total_allocated_bytes() << " bytes\n";
        std::cout << "  Buffer count:     " << buf_stats.total_buffers() << "\n";
        std::cout << "  Shard count:      " << buf_stats.shard_count() << "\n";

        // Cleanup (buffer and engine cleaned up automatically)
        close(fd);

        std::cout << "\nDone!\n";
        return (result > 0) ? 0 : 1;

    } catch (const aura::Error &e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
