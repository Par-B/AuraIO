/**
 * @file simple_read.cpp
 * @brief Simple async file read example (C++ version)
 *
 * Demonstrates basic usage of the AuraIO C++ library.
 *
 * Usage: ./simple_read <file>
 */

#include <auraio.hpp>

#include <iostream>
#include <iomanip>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

constexpr size_t READ_SIZE = 1024 * 1024;  // 1MB

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file>\n";
        std::cerr << "\nReads the first 1MB of a file asynchronously.\n";
        return 1;
    }

    const char* filename = argv[1];

    try {
        // Create the auraio engine
        std::cout << "Creating async I/O engine...\n";
        auraio::Engine engine;

        // Open file with O_DIRECT for best async I/O performance
        int fd = open(filename, O_RDONLY | O_DIRECT);
        if (fd < 0 && errno == EINVAL) {
            // O_DIRECT not supported on this filesystem - fall back
            fd = open(filename, O_RDONLY);
            std::cout << "Note: O_DIRECT not available, using buffered I/O\n";
        }
        if (fd < 0) {
            throw auraio::Error(errno, filename);
        }

        // Allocate aligned buffer from engine's pool (RAII)
        auto buffer = engine.allocate_buffer(READ_SIZE);

        // Track completion with local variables captured by lambda
        bool done = false;
        ssize_t result = 0;

        // Submit async read
        std::cout << "Submitting async read of " << READ_SIZE << " bytes...\n";
        (void)engine.read(fd, buffer, READ_SIZE, 0, [&](auraio::Request&, ssize_t res) {
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
            engine.wait(100);  // 100ms timeout
        }

        // Show first few bytes of data
        if (result > 0) {
            std::cout << "\nFirst 64 bytes of file:\n";
            auto* data = static_cast<unsigned char*>(buffer.data());
            for (int i = 0; i < 64 && i < result; i++) {
                unsigned char c = data[i];
                if (c >= 32 && c < 127) {
                    std::cout << static_cast<char>(c);
                } else {
                    std::cout << "\\x" << std::hex << std::setfill('0')
                              << std::setw(2) << static_cast<int>(c);
                    std::cout << std::dec;  // Reset to decimal
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

        // Cleanup (buffer and engine cleaned up automatically)
        close(fd);

        std::cout << "\nDone!\n";
        return (result > 0) ? 0 : 1;

    } catch (const auraio::Error& e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
