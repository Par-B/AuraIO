/**
 * @file quickstart.cpp
 * @brief Minimal working example of AuraIO C++ async read
 *
 * Build: make -C examples cpp-examples
 * Run:   ./examples/cpp/quickstart
 */

#include <auraio.hpp>

#include <iostream>
#include <fstream>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

constexpr size_t BUF_SIZE = 4096;

int main() {
    const char* test_file = "/tmp/auraio_quickstart_cpp.tmp";
    const char* test_data = "Hello from AuraIO C++! This is async I/O.\n";

    try {
        // Create a test file with known content
        {
            std::ofstream out(test_file, std::ios::binary);
            if (!out) {
                std::cerr << "Failed to create test file\n";
                return 1;
            }
            out.write(test_data, static_cast<std::streamsize>(strlen(test_data)));
        }

        // Create AuraIO engine (RAII - automatically cleaned up)
        auraio::Engine engine;

        // Allocate aligned buffer (RAII - automatically returned to pool)
        auto buffer = engine.allocate_buffer(BUF_SIZE);
        std::memset(buffer.data(), 0, BUF_SIZE);

        // Open file for reading
        int fd = open(test_file, O_RDONLY);
        if (fd < 0) {
            throw auraio::Error(errno, "open");
        }

        // Track completion with lambda capture
        bool done = false;
        ssize_t bytes_read = 0;

        // Submit async read with lambda callback
        engine.read(fd, buffer, BUF_SIZE, 0, [&](auraio::Request&, ssize_t result) {
            std::cout << "Read completed: " << result << " bytes\n";
            bytes_read = result;
            done = true;
        });

        // Wait for completion
        while (!done) {
            engine.wait(100);
        }

        // Verify result
        if (bytes_read > 0) {
            std::cout << "Data read: " << static_cast<char*>(buffer.data());
        }

        // Cleanup fd (buffer and engine cleaned up automatically)
        close(fd);
        unlink(test_file);

        std::cout << "Success!\n";
        return 0;

    } catch (const auraio::Error& e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        unlink(test_file);
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        unlink(test_file);
        return 1;
    }
}
