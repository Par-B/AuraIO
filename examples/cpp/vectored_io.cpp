/**
 * @file vectored_io.cpp
 * @brief Demonstrate vectored (scatter-gather) I/O operations (C++ version)
 *
 * Shows how to use engine.readv() and engine.writev() for efficient
 * I/O with multiple buffers in a single syscall. Useful for:
 * - Database page operations (header + data in separate buffers)
 * - Structured file formats (metadata + content sections)
 * - Log files (timestamp/metadata + message in separate buffers)
 * - Reducing syscall overhead for non-contiguous data regions
 *
 * Build: make cpp-examples
 * Run:   ./examples/cpp/vectored_io
 */

#include <auraio.hpp>

#include <iostream>
#include <array>
#include <vector>
#include <cstring>
#include <span>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

constexpr const char *TEST_FILE = "/tmp/auraio_vectored_test.dat";
constexpr size_t HEADER_SIZE = 64;
constexpr size_t PAYLOAD_SIZE = 1024;

struct IoContext {
    bool done = false;
    ssize_t result = 0;
};

void write_completion(auraio::Request &, ssize_t result, IoContext *ctx) {
    ctx->result = result;
    ctx->done = true;
    if (result < 0) {
        std::cerr << "Write error: " << result << "\n";
    } else {
        std::cout << "Vectored write completed: " << result << " bytes\n";
    }
}

void read_completion(auraio::Request &, ssize_t result, IoContext *ctx) {
    ctx->result = result;
    ctx->done = true;
    if (result < 0) {
        std::cerr << "Read error: " << result << "\n";
    } else {
        std::cout << "Vectored read completed: " << result << " bytes\n";
    }
}

int main() {
    std::cout << "AuraIO Vectored I/O Example (C++)\n";
    std::cout << "==================================\n\n";

    try {
        auraio::Engine engine;

        // ===================================================================
        // Example 1: Vectored Write (Gather)
        // Write metadata and data in a single operation (like database page writes)
        // ===================================================================
        std::cout << "Example 1: Vectored Write (Gather)\n";
        std::cout << "Writing structured data with separate metadata and payload buffers...\n";

        // Prepare metadata buffer (like a database page header)
        std::vector<char> header(HEADER_SIZE, 0);
        std::snprintf(header.data(), HEADER_SIZE, "METADATA: size=%zu, checksum=0xABCD, version=1",
                      PAYLOAD_SIZE);

        // Prepare payload buffer
        std::vector<char> payload(PAYLOAD_SIZE, 'X');
        payload.back() = '\0'; // Null-terminate for printing

        // Create iovec array for gather write
        std::array<iovec, 2> write_iov = {
            {{header.data(), HEADER_SIZE}, {payload.data(), PAYLOAD_SIZE}}};

        // Open file for writing
        int wfd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (wfd < 0) {
            throw auraio::Error(errno, "open for write");
        }

        // Submit vectored write
        IoContext write_ctx;
        (void)engine.writev(wfd, write_iov, 0, [&write_ctx](auraio::Request &req, ssize_t result) {
            write_completion(req, result, &write_ctx);
        });

        // Wait for write completion
        while (!write_ctx.done) {
            engine.wait(10);
        }

        std::cout << "  Header (" << write_iov[0].iov_len << " bytes): " << header.data() << "\n";
        std::cout << "  Payload (" << write_iov[1].iov_len << " bytes): [" << PAYLOAD_SIZE
                  << " bytes of 'X']\n";
        std::cout << "  Total written: " << write_ctx.result << " bytes\n";

        close(wfd);

        // ===================================================================
        // Example 2: Vectored Read (Scatter)
        // Read metadata and data into separate buffers (like database page reads)
        // ===================================================================
        std::cout << "\nExample 2: Vectored Read (Scatter)\n";
        std::cout << "Reading structured data into separate metadata and payload buffers...\n";

        // Allocate separate buffers for header and payload
        std::vector<char> read_header(HEADER_SIZE, 0);
        std::vector<char> read_payload(PAYLOAD_SIZE, 0);

        // Create iovec array for scatter read
        std::array<iovec, 2> read_iov = {
            {{read_header.data(), HEADER_SIZE}, {read_payload.data(), PAYLOAD_SIZE}}};

        // Open file for reading
        int rfd = open(TEST_FILE, O_RDONLY);
        if (rfd < 0) {
            unlink(TEST_FILE);
            throw auraio::Error(errno, "open for read");
        }

        // Submit vectored read
        IoContext read_ctx;
        (void)engine.readv(rfd, read_iov, 0, [&read_ctx](auraio::Request &req, ssize_t result) {
            read_completion(req, result, &read_ctx);
        });

        // Wait for read completion
        while (!read_ctx.done) {
            engine.wait(10);
        }

        std::cout << "  Header (" << read_iov[0].iov_len << " bytes): " << read_header.data()
                  << "\n";
        std::cout << "  Payload (" << read_iov[1].iov_len << " bytes): [First 20 bytes: ";
        for (size_t i = 0; i < 20 && i < PAYLOAD_SIZE; i++) {
            std::cout << read_payload[i];
        }
        std::cout << "...]\n";
        std::cout << "  Total read: " << read_ctx.result << " bytes\n";

        // Verify data integrity
        bool payload_ok = true;
        for (size_t i = 0; i < PAYLOAD_SIZE - 1; i++) {
            if (read_payload[i] != 'X') {
                payload_ok = false;
                break;
            }
        }

        std::cout << "\nData integrity check: " << (payload_ok ? "PASSED" : "FAILED") << "\n";

        // Cleanup
        close(rfd);
        unlink(TEST_FILE);

        std::cout << "\n======================================\n";
        std::cout << "Vectored I/O Benefits:\n";
        std::cout << "- Single syscall for multiple buffers\n";
        std::cout << "- Natural separation of metadata/data (e.g., database pages)\n";
        std::cout << "- Efficient for structured file formats\n";
        std::cout << "- Reduces syscall overhead vs multiple read/write calls\n";
        std::cout << "- Useful for log files with separate metadata and content sections\n";

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
