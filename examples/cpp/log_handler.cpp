/**
 * @file log_handler.cpp
 * @brief Demonstrate AuraIO custom log handler (C++)
 *
 * Shows how to install a custom log callback that formats library
 * messages with timestamps and severity levels, and how to emit
 * application-level messages through the same pipeline using
 * aura::log_emit().
 *
 * Build: make examples
 * Run:   ./examples/cpp/log_handler
 */

#include <aura.hpp>

#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <unistd.h>

constexpr auto TEST_FILE = "/tmp/aura_log_test_cpp.dat";
constexpr size_t FILE_SIZE = 64 * 1024; // 64 KB
constexpr size_t BUF_SIZE = 4096;

int main() {
    std::cout << "AuraIO Log Handler Example (C++)\n";
    std::cout << "================================\n\n";

    // --- Step 1: Install log handler with lambda -------------------------
    //
    // The C++ bindings let you pass any callable — here a lambda that
    // formats each message with a millisecond timestamp and severity tag.

    aura::set_log_handler([](aura::LogLevel level, std::string_view msg) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
        localtime_r(&time_t_now, &tm);

        std::cerr << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0')
                  << std::setw(3) << ms.count() << " [myapp] " << aura::log_level_name(level)
                  << ": " << msg << '\n';
    });

    // --- Step 2: Emit application-level messages -------------------------
    aura::log_emit(aura::LogLevel::Info, "log handler installed, creating engine");

    int fd = -1;

    try {
        // --- Step 3: Create engine and do I/O ----------------------------
        auto opts = aura::Options().queue_depth(64).ring_count(1);
        aura::Engine engine(opts);

        aura::log_emit(aura::LogLevel::Notice, "engine created (1 ring, depth 64)");

        // Create test file
        {
            std::ofstream out(TEST_FILE, std::ios::binary);
            if (!out) {
                aura::log_emit(aura::LogLevel::Error, "failed to create test file");
                aura::clear_log_handler();
                return 1;
            }
            std::string data(FILE_SIZE, 'A');
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
        }

        fd = open(TEST_FILE, O_RDONLY);
        if (fd < 0) {
            aura::log_emit(aura::LogLevel::Error, "failed to open test file");
            unlink(TEST_FILE);
            aura::clear_log_handler();
            return 1;
        }

        auto buffer = engine.allocate_buffer(BUF_SIZE);

        aura::log_emit(aura::LogLevel::Debug, "submitting read");

        bool done = false;
        (void)engine.read(fd, buffer, BUF_SIZE, 0, [&](aura::Request &, ssize_t result) {
            if (result < 0)
                aura::log_emit(aura::LogLevel::Error, "I/O error: " + std::to_string(result));
            done = true;
        });

        while (!done) engine.wait(100);

        aura::log_emit(aura::LogLevel::Info, "read completed successfully");

        // --- Step 4: Show stats ------------------------------------------
        auto stats = engine.get_stats();
        std::cout << "\nEngine stats:\n";
        std::cout << "  Operations completed: " << stats.ops_completed() << '\n';
        std::cout << "  P99 latency: " << stats.p99_latency_ms() << " ms\n";

        // --- Step 5: Clean up --------------------------------------------
        close(fd);
        unlink(TEST_FILE);

        aura::log_emit(aura::LogLevel::Notice, "shutting down");

        // Engine destroyed here (RAII) while handler is still installed,
        // so we capture any shutdown diagnostics.

    } catch (const aura::Error &e) {
        aura::log_emit(aura::LogLevel::Error, std::string("AuraIO error: ") + e.what());
        if (fd >= 0) close(fd);
        unlink(TEST_FILE);
        aura::clear_log_handler();
        return 1;
    } catch (const std::exception &e) {
        aura::log_emit(aura::LogLevel::Error, std::string("unexpected error: ") + e.what());
        if (fd >= 0) close(fd);
        unlink(TEST_FILE);
        aura::clear_log_handler();
        return 1;
    }

    // Handler no longer needed after engine is gone.
    aura::clear_log_handler();

    std::cout << "\n--- Summary ---\n";
    std::cout << "The log handler captured all library and application messages\n";
    std::cout << "on stderr with timestamps, severity levels, and an app prefix.\n";
    std::cout << "In production, replace the lambda with your framework's\n";
    std::cout << "logging function (spdlog, syslog, journald, etc.).\n";

    return 0;
}
