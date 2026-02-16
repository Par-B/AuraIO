/**
 * @file bulk_reader.cpp
 * @brief High-throughput bulk file reader example (C++ version)
 *
 * Demonstrates reading many files concurrently with adaptive tuning.
 *
 * Usage: ./bulk_reader <directory>
 */

#include <aura.hpp>

#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Clock = std::chrono::high_resolution_clock;

constexpr size_t MAX_FILES = 10000;
constexpr size_t READ_SIZE = 256 * 1024;  // 256KB per file
constexpr size_t STATS_INTERVAL = 1000;   // Print stats every N completions

// Context for tracking bulk read progress
struct BulkContext {
    aura::Engine& engine;
    std::atomic<int> files_pending{0};
    std::atomic<int> files_completed{0};
    std::atomic<long long> bytes_read{0};
    std::atomic<long long> errors{0};
    Clock::time_point start_time;

    explicit BulkContext(aura::Engine& eng)
        : engine(eng), start_time(Clock::now()) {}
};

// Per-file context for cleanup in callback
struct FileContext {
    BulkContext& bulk;
    int fd;
    aura::Buffer buffer;

    FileContext(BulkContext& b, int f, aura::Buffer buf)
        : bulk(b), fd(f), buffer(std::move(buf)) {}

    ~FileContext() {
        if (fd >= 0) {
            close(fd);
        }
        // buffer automatically returned to pool by RAII
    }
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <directory>\n";
        std::cerr << "\nReads all regular files in a directory using async I/O.\n";
        std::cerr << "The engine self-tunes for optimal throughput.\n";
        return 1;
    }

    const std::string dirname = argv[1];

    try {
        // Create the aura engine
        std::cout << "Creating async I/O engine...\n";
        aura::Engine engine;

        // Initialize context
        BulkContext ctx(engine);

        // Check directory exists
        if (!fs::is_directory(dirname)) {
            std::cerr << "Not a directory: " << dirname << "\n";
            return 1;
        }

        std::cout << "Scanning directory '" << dirname << "'...\n";

        // Collect files to read
        std::vector<std::unique_ptr<FileContext>> file_contexts;
        int submitted = 0;

        for (const auto& entry : fs::directory_iterator(dirname)) {
            if (submitted >= static_cast<int>(MAX_FILES)) break;
            if (!entry.is_regular_file()) continue;

            const auto& path = entry.path();

            // Open file with O_DIRECT for best performance
            int fd = open(path.c_str(), O_RDONLY | O_DIRECT);
            if (fd < 0 && errno == EINVAL) {
                // O_DIRECT not supported on this filesystem - fall back
                fd = open(path.c_str(), O_RDONLY);
            }
            if (fd < 0) continue;  // Skip files we can't open

            // Allocate buffer (RAII)
            aura::Buffer buffer;
            try {
                buffer = engine.allocate_buffer(READ_SIZE);
            } catch (const aura::Error&) {
                close(fd);
                continue;
            }

            // Create per-file context
            auto fctx = std::make_unique<FileContext>(ctx, fd, std::move(buffer));

            // Get raw pointer for lambda capture (context outlives the lambda)
            FileContext* fctx_ptr = fctx.get();

            // Submit async read with lambda callback
            try {
                (void)engine.read(fctx_ptr->fd, fctx_ptr->buffer, READ_SIZE, 0,
                    [fctx_ptr, &ctx, &engine](aura::Request&, ssize_t result) {
                        if (result > 0) {
                            ctx.bytes_read += result;
                        } else if (result < 0) {
                            ctx.errors++;
                        }

                        // Close fd (buffer cleanup happens when FileContext is destroyed)
                        close(fctx_ptr->fd);
                        fctx_ptr->fd = -1;  // Mark as closed

                        ctx.files_completed++;
                        ctx.files_pending--;

                        // Print periodic stats
                        int completed = ctx.files_completed.load();
                        if (completed % STATS_INTERVAL == 0) {
                            auto now = Clock::now();
                            auto elapsed = std::chrono::duration<double>(now - ctx.start_time).count();
                            double mb_read = ctx.bytes_read.load() / (1024.0 * 1024.0);
                            double mb_per_sec = mb_read / elapsed;

                            std::cout << "\rProgress: " << completed << " files, "
                                      << std::fixed << std::setprecision(2)
                                      << mb_read << " MB, " << mb_per_sec << " MB/s" << std::flush;
                        }

                        // Stop when all files done
                        if (ctx.files_pending.load() == 0) {
                            engine.stop();
                        }
                    });

                ctx.files_pending++;
                submitted++;
                file_contexts.push_back(std::move(fctx));

            } catch (const aura::Error&) {
                // Submission failed, fctx will be cleaned up
                continue;
            }
        }

        if (submitted == 0) {
            std::cout << "No files found to read.\n";
            return 1;
        }

        std::cout << "Submitted " << submitted << " file reads\n";
        std::cout << "Running event loop (self-tuning in progress)...\n\n";

        // Process until all complete
        engine.run();

        // Final stats
        std::cout << "\n\n";
        std::cout << "=== Final Results ===\n";

        auto end_time = Clock::now();
        double total_elapsed = std::chrono::duration<double>(end_time - ctx.start_time).count();
        double mb_total = ctx.bytes_read.load() / (1024.0 * 1024.0);

        std::cout << "Files read:       " << ctx.files_completed.load() << "\n";
        std::cout << "Total bytes:      " << std::fixed << std::setprecision(2)
                  << mb_total << " MB\n";
        std::cout << "Errors:           " << ctx.errors.load() << "\n";
        std::cout << "Elapsed time:     " << total_elapsed << " seconds\n";
        std::cout << "Average speed:    " << mb_total / total_elapsed << " MB/s\n";

        // Engine tuning results
        auto stats = engine.get_stats();
        std::cout << "\nAdaptive tuning results:\n";
        std::cout << "  Optimal in-flight: " << stats.optimal_in_flight() << "\n";
        std::cout << "  P99 latency:       " << stats.p99_latency_ms() << " ms\n";

        std::cout << "\nDone!\n";
        return 0;

    } catch (const aura::Error& e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
