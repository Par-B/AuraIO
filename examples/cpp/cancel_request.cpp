/**
 * @file cancel_request.cpp
 * @brief Demonstrates cancelling an in-flight I/O operation (C++)
 *
 * Shows how to use engine.cancel() to abort a pending read.
 * The cancelled operation's callback receives result = -ECANCELED.
 *
 * Usage: ./cancel_request <file>
 */

#include <auraio.hpp>

#include <iostream>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

constexpr size_t READ_SIZE = 4096;

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file>\n";
        return 1;
    }

    try {
        // Create engine with RAII
        auraio::Engine engine;

        // Open file
        int fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error(std::string("Failed to open file: ") + strerror(errno));
        }

        // Allocate buffer (automatically managed)
        auto buf = engine.allocate_buffer(READ_SIZE);

        // Completion state with atomic for thread-safety
        std::atomic<bool> completed{false};
        std::atomic<ssize_t> read_result{0};

        // Submit async read with lambda callback
        std::cout << "Submitting async read...\n";
        auto req = engine.read(
            fd, buf, READ_SIZE, 0, [&completed, &read_result](auraio::Request &, ssize_t result) {
                read_result.store(result, std::memory_order_release);
                completed.store(true, std::memory_order_release);

                if (result == -ECANCELED) {
                    std::cout << "Read was cancelled (result = -ECANCELED)\n";
                } else if (result < 0) {
                    std::cerr << "Read failed: " << strerror(static_cast<int>(-result)) << "\n";
                } else {
                    std::cout << "Read completed: " << result << " bytes\n";
                }
            });

        /* Attempt to cancel the request.
         * Note: cancellation is best-effort. If the I/O already completed
         * by the time cancel is processed, the original result is returned
         * instead of -ECANCELED. */
        std::cout << "Attempting to cancel...\n";
        bool cancelled = engine.cancel(req);
        if (cancelled) {
            std::cout << "Cancel request submitted successfully\n";
        } else {
            std::cout << "Cancel submission failed (operation may have already completed)\n";
        }

        // Wait for completion callback
        while (!completed.load(std::memory_order_acquire)) {
            engine.wait(100); // 100ms timeout
        }

        ssize_t final_result = read_result.load(std::memory_order_acquire);
        std::cout << "Final result: " << final_result << " ("
                  << (final_result == -ECANCELED ? "cancelled"
                      : final_result < 0         ? strerror(static_cast<int>(-final_result))
                                                 : "success")
                  << ")\n";

        // Cleanup (automatic via RAII)
        close(fd);
        return 0;

    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
