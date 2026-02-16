/**
 * @file coroutine_copy.cpp
 * @brief C++20 coroutine-based async file copy
 *
 * Demonstrates using co_await with AuraIO for elegant async code.
 * This example copies a file using coroutines for clean, sequential-looking
 * code that is actually asynchronous.
 *
 * Usage: ./coroutine_copy <source> <destination>
 *
 * Requires: C++20 with coroutine support (g++ -std=c++20 -fcoroutines)
 */

#include <aura.hpp>

#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using Clock = std::chrono::high_resolution_clock;

constexpr size_t CHUNK_SIZE = 256 * 1024; // 256KB chunks

/**
 * Async file copy using coroutines.
 *
 * This function looks sequential but is actually asynchronous.
 * Each co_await suspends the coroutine until the I/O completes,
 * allowing other work to proceed.
 */
aura::Task<size_t> async_copy(aura::Engine &engine, int src_fd, int dst_fd, size_t file_size) {
    auto buffer = engine.allocate_buffer(CHUNK_SIZE);
    size_t total_copied = 0;
    off_t offset = 0;

    while (static_cast<size_t>(offset) < file_size) {
        // Calculate chunk size (may be less at end of file)
        size_t chunk = std::min(CHUNK_SIZE, file_size - static_cast<size_t>(offset));

        // Async read - suspends coroutine until complete
        ssize_t bytes_read = co_await engine.async_read(src_fd, buffer, chunk, offset);
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
                throw aura::Error(static_cast<int>(-bytes_read), "read failed");
            }
            break; // EOF
        }

        // Async write - suspends coroutine until complete
        ssize_t bytes_written = co_await engine.async_write(dst_fd, buffer, bytes_read, offset);
        if (bytes_written < 0) {
            throw aura::Error(static_cast<int>(-bytes_written), "write failed");
        }

        total_copied += bytes_written;
        offset += bytes_written;

        // Progress indicator
        double progress = 100.0 * total_copied / file_size;
        std::cout << "\rProgress: " << std::fixed << std::setprecision(1) << progress << "%"
                  << std::flush;
    }

    // Async fsync - ensure data is on disk
    co_await engine.async_fsync(dst_fd);

    std::cout << "\n";
    co_return total_copied;
}

/**
 * Get file size.
 */
size_t get_file_size(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) {
        throw aura::Error(errno, "fstat");
    }
    return static_cast<size_t>(st.st_size);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <source> <destination>\n";
        std::cerr << "\nCopies a file using C++20 coroutines with async I/O.\n";
        return 1;
    }

    const char *src_path = argv[1];
    const char *dst_path = argv[2];

    int src_fd = -1;
    int dst_fd = -1;

    try {
        std::cout << "AuraIO Coroutine File Copy\n";
        std::cout << "==========================\n";
        std::cout << "Source:      " << src_path << "\n";
        std::cout << "Destination: " << dst_path << "\n";

        // Open source file
        src_fd = open(src_path, O_RDONLY);
        if (src_fd < 0) {
            throw aura::Error(errno, src_path);
        }

        // Get source file size
        size_t file_size = get_file_size(src_fd);
        std::cout << "File size:   " << file_size << " bytes (" << std::fixed
                  << std::setprecision(2) << file_size / (1024.0 * 1024.0) << " MB)\n";

        // Create destination file
        dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dst_fd < 0) {
            throw aura::Error(errno, dst_path);
        }

        // Create engine
        aura::Engine engine;

        // Start the copy coroutine
        auto start_time = Clock::now();
        std::cout << "\nCopying...\n";

        auto task = async_copy(engine, src_fd, dst_fd, file_size);

        // Start the coroutine (it will suspend on first I/O)
        task.resume();

        // Wait for completion - I/O callbacks will resume the coroutine
        while (!task.done()) {
            engine.wait(100); // Process completions
        }

        // Get the result (may throw if there was an error)
        size_t bytes_copied = task.get();

        auto end_time = Clock::now();
        double elapsed = std::chrono::duration<double>(end_time - start_time).count();

        // Results
        std::cout << "\n=== Results ===\n";
        std::cout << "Bytes copied: " << bytes_copied << "\n";
        std::cout << "Elapsed time: " << std::fixed << std::setprecision(3) << elapsed
                  << " seconds\n";
        std::cout << "Throughput:   " << std::setprecision(2)
                  << (bytes_copied / (1024.0 * 1024.0)) / elapsed << " MB/s\n";

        // Engine stats
        auto stats = engine.get_stats();
        std::cout << "\nEngine statistics:\n";
        std::cout << "  Ops completed:     " << stats.ops_completed() << "\n";
        std::cout << "  P99 latency:       " << stats.p99_latency_ms() << " ms\n";
        std::cout << "  Optimal in-flight: " << stats.optimal_in_flight() << "\n";

        // Cleanup
        close(src_fd);
        close(dst_fd);

        std::cout << "\nDone! File copied successfully.\n";
        return 0;

    } catch (const aura::Error &e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        if (src_fd >= 0) close(src_fd);
        if (dst_fd >= 0) close(dst_fd);
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        if (src_fd >= 0) close(src_fd);
        if (dst_fd >= 0) close(dst_fd);
        return 1;
    }
}
