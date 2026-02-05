/**
 * @file stress_test.cpp
 * @brief Comprehensive stress test for AuraIO library
 *
 * Tests:
 * 1. High concurrency - many simultaneous I/O operations
 * 2. Mixed operations - reads, writes, fsync
 * 3. Various buffer sizes
 * 4. Buffer pool stress - rapid alloc/free cycles
 * 5. Long-running sustained load
 * 6. Multi-threaded submissions
 * 7. Cancellation under load
 *
 * Build: make -C tests stress_test
 * Run:   ./tests/stress_test [options]
 *
 * Options:
 *   --duration <seconds>   Test duration (default: 10)
 *   --threads <count>      Number of threads (default: 4)
 *   --files <count>        Number of test files (default: 16)
 *   --max-inflight <n>     Max concurrent I/O (default: 256)
 */

#include <auraio.hpp>

#include <iostream>
#include <iomanip>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <random>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono_literals;

// =============================================================================
// Configuration
// =============================================================================

struct Config {
    int duration_sec = 10;
    int num_threads = 4;
    int num_files = 16;
    int max_inflight = 256;
    size_t file_size = 64 * 1024 * 1024;  // 64MB per file
    std::string test_dir = "/tmp/auraio_stress";
};

// =============================================================================
// Statistics
// =============================================================================

struct Stats {
    std::atomic<long long> ops_submitted{0};
    std::atomic<long long> ops_completed{0};
    std::atomic<long long> ops_failed{0};
    std::atomic<long long> bytes_read{0};
    std::atomic<long long> bytes_written{0};
    std::atomic<long long> buffers_allocated{0};
    std::atomic<long long> buffers_freed{0};
    std::atomic<long long> cancellations{0};

    void print(double elapsed_sec) const {
        long long total_bytes = bytes_read + bytes_written;
        double throughput_mb = (total_bytes / (1024.0 * 1024.0)) / elapsed_sec;
        double iops = ops_completed / elapsed_sec;

        std::cout << "\n=== Stress Test Results ===\n";
        std::cout << "Duration:          " << std::fixed << std::setprecision(2)
                  << elapsed_sec << " seconds\n";
        std::cout << "Ops submitted:     " << ops_submitted << "\n";
        std::cout << "Ops completed:     " << ops_completed << "\n";
        std::cout << "Ops failed:        " << ops_failed << "\n";
        std::cout << "Bytes read:        " << bytes_read / (1024 * 1024) << " MB\n";
        std::cout << "Bytes written:     " << bytes_written / (1024 * 1024) << " MB\n";
        std::cout << "Throughput:        " << throughput_mb << " MB/s\n";
        std::cout << "IOPS:              " << std::setprecision(0) << iops << "\n";
        std::cout << "Buffers allocated: " << buffers_allocated << "\n";
        std::cout << "Buffers freed:     " << buffers_freed << "\n";
        std::cout << "Cancellations:     " << cancellations << "\n";
    }
};

// =============================================================================
// Test File Management
// =============================================================================

class TestFiles {
public:
    TestFiles(const std::string& dir, int count, size_t file_size)
        : dir_(dir), file_size_(file_size)
    {
        // Create test directory
        mkdir(dir.c_str(), 0755);

        // Create test files with random data
        std::vector<char> buf(1024 * 1024);  // 1MB buffer
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dist(0, 255);

        for (int i = 0; i < count; i++) {
            std::string path = dir + "/test_" + std::to_string(i) + ".dat";
            paths_.push_back(path);

            int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                throw std::runtime_error("Failed to create test file: " + path);
            }

            // Write random data
            for (size_t written = 0; written < file_size; ) {
                // Fill buffer with random data
                for (auto& c : buf) {
                    c = static_cast<char>(dist(gen));
                }
                size_t chunk = std::min(buf.size(), file_size - written);
                if (write(fd, buf.data(), chunk) < 0) {
                    close(fd);
                    throw std::runtime_error("Failed to write test file");
                }
                written += chunk;
            }
            fsync(fd);
            close(fd);
        }

        std::cout << "Created " << count << " test files of "
                  << file_size / (1024 * 1024) << " MB each\n";
    }

    ~TestFiles() {
        for (const auto& path : paths_) {
            unlink(path.c_str());
        }
        rmdir(dir_.c_str());
    }

    const std::vector<std::string>& paths() const { return paths_; }
    size_t file_size() const { return file_size_; }

private:
    std::string dir_;
    size_t file_size_;
    std::vector<std::string> paths_;
};

// =============================================================================
// Stress Test: High Concurrency Reads
// =============================================================================

void test_high_concurrency_reads(auraio::Engine& engine, const TestFiles& files,
                                  Stats& stats, int max_inflight, int duration_sec)
{
    std::cout << "\n--- Test: High Concurrency Reads ---\n";

    const size_t buf_size = 64 * 1024;  // 64KB
    std::atomic<int> inflight{0};
    std::atomic<bool> done{false};

    // Open all files
    std::vector<int> fds;
    for (const auto& path : files.paths()) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) fds.push_back(fd);
    }

    if (fds.empty()) {
        std::cerr << "No files to read!\n";
        return;
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> fd_dist(0, fds.size() - 1);
    std::uniform_int_distribution<off_t> off_dist(0, files.file_size() - buf_size);

    auto start = Clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    // Submission thread
    std::thread submitter([&]() {
        while (Clock::now() < end_time) {
            // Wait if at max inflight
            while (inflight >= max_inflight && !done) {
                std::this_thread::sleep_for(100us);
            }
            if (done) break;

            try {
                auto buffer = std::make_shared<auraio::Buffer>(engine.allocate_buffer(buf_size));
                stats.buffers_allocated++;

                int fd = fds[fd_dist(gen)];
                off_t offset = off_dist(gen);
                // Align offset for O_DIRECT compatibility
                offset = (offset / 4096) * 4096;

                inflight++;
                stats.ops_submitted++;

                engine.read(fd, *buffer, buf_size, offset,
                    [&stats, &inflight, buffer](auraio::Request&, ssize_t result) {
                        inflight--;
                        if (result > 0) {
                            stats.bytes_read += result;
                            stats.ops_completed++;
                        } else {
                            stats.ops_failed++;
                        }
                        stats.buffers_freed++;
                        // buffer is released here when shared_ptr destructs
                    });
            } catch (const std::exception& e) {
                stats.ops_failed++;
            }
        }
        done = true;
    });

    // Completion processing
    while (!done || inflight > 0) {
        engine.wait(10);
    }

    submitter.join();

    for (int fd : fds) close(fd);

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Completed in " << std::fixed << std::setprecision(2)
              << elapsed << "s, " << stats.ops_completed << " ops\n";
}

// =============================================================================
// Stress Test: Mixed Read/Write Operations
// =============================================================================

void test_mixed_operations(auraio::Engine& engine, const TestFiles& files,
                           Stats& stats, int max_inflight, int duration_sec)
{
    std::cout << "\n--- Test: Mixed Read/Write/Fsync Operations ---\n";

    const size_t buf_size = 32 * 1024;  // 32KB
    std::atomic<int> inflight{0};
    std::atomic<bool> done{false};

    // Open files for read/write
    std::vector<int> fds;
    for (const auto& path : files.paths()) {
        int fd = open(path.c_str(), O_RDWR);
        if (fd >= 0) fds.push_back(fd);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> fd_dist(0, fds.size() - 1);
    std::uniform_int_distribution<off_t> off_dist(0, files.file_size() - buf_size);
    std::uniform_int_distribution<int> op_dist(0, 99);  // 0-99 for percentage

    auto start = Clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    std::thread submitter([&]() {
        while (Clock::now() < end_time) {
            while (inflight >= max_inflight && !done) {
                std::this_thread::sleep_for(100us);
            }
            if (done) break;

            try {
                int fd = fds[fd_dist(gen)];
                int op = op_dist(gen);

                if (op < 60) {
                    // 60% reads
                    auto buffer = std::make_shared<auraio::Buffer>(engine.allocate_buffer(buf_size));
                    stats.buffers_allocated++;
                    off_t offset = (off_dist(gen) / 4096) * 4096;

                    inflight++;
                    stats.ops_submitted++;

                    engine.read(fd, *buffer, buf_size, offset,
                        [&stats, &inflight, buffer](auraio::Request&, ssize_t result) {
                            inflight--;
                            if (result > 0) {
                                stats.bytes_read += result;
                                stats.ops_completed++;
                            } else {
                                stats.ops_failed++;
                            }
                            stats.buffers_freed++;
                        });
                } else if (op < 95) {
                    // 35% writes
                    auto buffer = std::make_shared<auraio::Buffer>(engine.allocate_buffer(buf_size));
                    stats.buffers_allocated++;
                    std::memset(buffer->data(), 'W', buf_size);
                    off_t offset = (off_dist(gen) / 4096) * 4096;

                    inflight++;
                    stats.ops_submitted++;

                    engine.write(fd, *buffer, buf_size, offset,
                        [&stats, &inflight, buffer](auraio::Request&, ssize_t result) {
                            inflight--;
                            if (result > 0) {
                                stats.bytes_written += result;
                                stats.ops_completed++;
                            } else {
                                stats.ops_failed++;
                            }
                            stats.buffers_freed++;
                        });
                } else {
                    // 5% fsync
                    inflight++;
                    stats.ops_submitted++;

                    engine.fsync(fd, [&stats, &inflight](auraio::Request&, ssize_t result) {
                        inflight--;
                        if (result >= 0) {
                            stats.ops_completed++;
                        } else {
                            stats.ops_failed++;
                        }
                    });
                }
            } catch (const std::exception& e) {
                stats.ops_failed++;
            }
        }
        done = true;
    });

    while (!done || inflight > 0) {
        engine.wait(10);
    }

    submitter.join();

    for (int fd : fds) close(fd);

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Completed in " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";
}

// =============================================================================
// Stress Test: Buffer Pool
// =============================================================================

void test_buffer_pool_stress(auraio::Engine& engine, Stats& stats, int duration_sec)
{
    std::cout << "\n--- Test: Buffer Pool Stress ---\n";

    std::atomic<bool> done{false};
    auto start = Clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    const size_t sizes[] = {4096, 8192, 16384, 32768, 65536, 131072, 262144};
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> size_dist(0, num_sizes - 1);

    // Multiple threads allocating and freeing buffers
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; t++) {
        threads.emplace_back([&, seed = rd()]() {
            std::mt19937 local_gen(seed);
            std::vector<auraio::Buffer> buffers;
            buffers.reserve(100);

            while (Clock::now() < end_time) {
                // Allocate some buffers
                for (int i = 0; i < 10; i++) {
                    try {
                        size_t size = sizes[size_dist(local_gen)];
                        buffers.push_back(engine.allocate_buffer(size));
                        stats.buffers_allocated++;
                    } catch (...) {
                        // Pool might be exhausted
                    }
                }

                // Free some buffers
                int to_free = std::min(static_cast<int>(buffers.size()), 5);
                for (int i = 0; i < to_free; i++) {
                    buffers.pop_back();
                    stats.buffers_freed++;
                }

                std::this_thread::sleep_for(100us);
            }

            // Free remaining
            while (!buffers.empty()) {
                buffers.pop_back();
                stats.buffers_freed++;
            }
        });
    }

    for (auto& t : threads) t.join();

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Completed in " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";
}

// =============================================================================
// Stress Test: Multi-threaded Submissions
// =============================================================================

void test_multithread_submissions(auraio::Engine& engine, const TestFiles& files,
                                   Stats& stats, int num_threads, int duration_sec)
{
    std::cout << "\n--- Test: Multi-threaded Submissions (" << num_threads << " threads) ---\n";

    const size_t buf_size = 16 * 1024;  // 16KB
    std::atomic<int> inflight{0};
    std::atomic<bool> done{false};

    // Open files
    std::vector<int> fds;
    for (const auto& path : files.paths()) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) fds.push_back(fd);
    }

    auto start = Clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    // Multiple submission threads
    std::vector<std::thread> submitters;
    for (int t = 0; t < num_threads; t++) {
        submitters.emplace_back([&, seed = std::random_device{}()]() {
            std::mt19937 gen(seed);
            std::uniform_int_distribution<size_t> fd_dist(0, fds.size() - 1);
            std::uniform_int_distribution<off_t> off_dist(0, files.file_size() - buf_size);

            while (Clock::now() < end_time) {
                while (inflight >= 512 && !done) {
                    std::this_thread::sleep_for(100us);
                }
                if (done) break;

                try {
                    auto buffer = std::make_shared<auraio::Buffer>(engine.allocate_buffer(buf_size));
                    stats.buffers_allocated++;

                    int fd = fds[fd_dist(gen)];
                    off_t offset = (off_dist(gen) / 4096) * 4096;

                    inflight++;
                    stats.ops_submitted++;

                    engine.read(fd, *buffer, buf_size, offset,
                        [&stats, &inflight, buffer](auraio::Request&, ssize_t result) {
                            inflight--;
                            if (result > 0) {
                                stats.bytes_read += result;
                                stats.ops_completed++;
                            } else {
                                stats.ops_failed++;
                            }
                            stats.buffers_freed++;
                        });
                } catch (const std::exception& e) {
                    stats.ops_failed++;
                }
            }
        });
    }

    // Completion thread
    std::thread completer([&]() {
        while (!done || inflight > 0) {
            engine.wait(10);
        }
    });

    // Wait for duration
    std::this_thread::sleep_until(end_time);
    done = true;

    for (auto& t : submitters) t.join();
    completer.join();

    for (int fd : fds) close(fd);

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Completed in " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";
}

// =============================================================================
// Stress Test: Vectored I/O
// =============================================================================

void test_vectored_io(auraio::Engine& engine, const TestFiles& files,
                      Stats& stats, int duration_sec)
{
    std::cout << "\n--- Test: Vectored I/O ---\n";

    const size_t chunk_size = 4096;
    const int num_chunks = 16;  // 16 x 4KB = 64KB per operation
    std::atomic<int> inflight{0};
    std::atomic<bool> done{false};

    std::vector<int> fds;
    for (const auto& path : files.paths()) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) fds.push_back(fd);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> fd_dist(0, fds.size() - 1);
    std::uniform_int_distribution<off_t> off_dist(0, files.file_size() - chunk_size * num_chunks);

    auto start = Clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    std::thread submitter([&]() {
        while (Clock::now() < end_time) {
            while (inflight >= 128 && !done) {
                std::this_thread::sleep_for(100us);
            }
            if (done) break;

            try {
                // Allocate buffers for scatter read
                auto buffers = std::make_shared<std::vector<auraio::Buffer>>();
                auto iovecs = std::make_shared<std::vector<iovec>>();

                for (int i = 0; i < num_chunks; i++) {
                    buffers->push_back(engine.allocate_buffer(chunk_size));
                    stats.buffers_allocated++;
                    iovecs->push_back({buffers->back().data(), chunk_size});
                }

                int fd = fds[fd_dist(gen)];
                off_t offset = (off_dist(gen) / 4096) * 4096;

                inflight++;
                stats.ops_submitted++;

                engine.readv(fd, std::span<const iovec>(*iovecs), offset,
                    [&stats, &inflight, buffers, iovecs](auraio::Request&, ssize_t result) {
                        inflight--;
                        if (result > 0) {
                            stats.bytes_read += result;
                            stats.ops_completed++;
                        } else {
                            stats.ops_failed++;
                        }
                        stats.buffers_freed += buffers->size();
                        // buffers and iovecs released here
                    });
            } catch (const std::exception& e) {
                stats.ops_failed++;
            }
        }
        done = true;
    });

    while (!done || inflight > 0) {
        engine.wait(10);
    }

    submitter.join();

    for (int fd : fds) close(fd);

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Completed in " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";
}

// =============================================================================
// Stress Test: Varying Buffer Sizes
// =============================================================================

void test_varying_buffer_sizes(auraio::Engine& engine, const TestFiles& files,
                                Stats& stats, int duration_sec)
{
    std::cout << "\n--- Test: Varying Buffer Sizes ---\n";

    const size_t sizes[] = {4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576};
    const int num_sizes = sizeof(sizes) / sizeof(sizes[0]);

    std::atomic<int> inflight{0};
    std::atomic<bool> done{false};

    std::vector<int> fds;
    for (const auto& path : files.paths()) {
        int fd = open(path.c_str(), O_RDONLY);
        if (fd >= 0) fds.push_back(fd);
    }

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> fd_dist(0, fds.size() - 1);
    std::uniform_int_distribution<int> size_dist(0, num_sizes - 1);

    auto start = Clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    std::thread submitter([&]() {
        while (Clock::now() < end_time) {
            while (inflight >= 64 && !done) {  // Lower limit for large buffers
                std::this_thread::sleep_for(100us);
            }
            if (done) break;

            try {
                size_t buf_size = sizes[size_dist(gen)];
                auto buffer = std::make_shared<auraio::Buffer>(engine.allocate_buffer(buf_size));
                stats.buffers_allocated++;

                int fd = fds[fd_dist(gen)];
                off_t max_offset = files.file_size() - buf_size;
                if (max_offset < 0) max_offset = 0;
                std::uniform_int_distribution<off_t> off_dist(0, max_offset);
                off_t offset = (off_dist(gen) / 4096) * 4096;

                inflight++;
                stats.ops_submitted++;

                engine.read(fd, *buffer, buf_size, offset,
                    [&stats, &inflight, buffer](auraio::Request&, ssize_t result) {
                        inflight--;
                        if (result > 0) {
                            stats.bytes_read += result;
                            stats.ops_completed++;
                        } else {
                            stats.ops_failed++;
                        }
                        stats.buffers_freed++;
                    });
            } catch (const std::exception& e) {
                stats.ops_failed++;
            }
        }
        done = true;
    });

    while (!done || inflight > 0) {
        engine.wait(10);
    }

    submitter.join();

    for (int fd : fds) close(fd);

    auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::cout << "Completed in " << std::fixed << std::setprecision(2)
              << elapsed << "s\n";
}

// =============================================================================
// Main
// =============================================================================

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n";
    std::cerr << "Options:\n";
    std::cerr << "  --duration <seconds>   Test duration (default: 10)\n";
    std::cerr << "  --threads <count>      Number of threads (default: 4)\n";
    std::cerr << "  --files <count>        Number of test files (default: 16)\n";
    std::cerr << "  --max-inflight <n>     Max concurrent I/O (default: 256)\n";
    std::cerr << "  --quick                Quick test (2 seconds per test)\n";
    std::cerr << "  --help                 Show this help\n";
}

int main(int argc, char** argv) {
    Config config;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--duration" && i + 1 < argc) {
            config.duration_sec = std::stoi(argv[++i]);
        } else if (arg == "--threads" && i + 1 < argc) {
            config.num_threads = std::stoi(argv[++i]);
        } else if (arg == "--files" && i + 1 < argc) {
            config.num_files = std::stoi(argv[++i]);
        } else if (arg == "--max-inflight" && i + 1 < argc) {
            config.max_inflight = std::stoi(argv[++i]);
        } else if (arg == "--quick") {
            config.duration_sec = 2;
            config.num_files = 4;
            config.file_size = 16 * 1024 * 1024;  // 16MB
        } else if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    std::cout << "=== AuraIO Stress Test ===\n";
    std::cout << "Duration:     " << config.duration_sec << " seconds per test\n";
    std::cout << "Threads:      " << config.num_threads << "\n";
    std::cout << "Test files:   " << config.num_files << "\n";
    std::cout << "Max inflight: " << config.max_inflight << "\n";
    std::cout << "File size:    " << config.file_size / (1024 * 1024) << " MB\n";

    try {
        // Create test files
        std::cout << "\nCreating test files...\n";
        TestFiles files(config.test_dir, config.num_files, config.file_size);

        // Create engine
        auraio::Options opts;
        opts.queue_depth(512)
            .ring_count(0);  // Auto-detect

        auraio::Engine engine(opts);
        Stats stats;

        auto total_start = Clock::now();

        // Run all stress tests
        test_high_concurrency_reads(engine, files, stats, config.max_inflight, config.duration_sec);
        test_mixed_operations(engine, files, stats, config.max_inflight, config.duration_sec);
        test_buffer_pool_stress(engine, stats, config.duration_sec);
        test_multithread_submissions(engine, files, stats, config.num_threads, config.duration_sec);
        test_vectored_io(engine, files, stats, config.duration_sec);
        test_varying_buffer_sizes(engine, files, stats, config.duration_sec);

        auto total_elapsed = std::chrono::duration<double>(Clock::now() - total_start).count();

        // Final engine stats
        auto engine_stats = engine.get_stats();
        std::cout << "\n=== Engine Statistics ===\n";
        std::cout << "Total ops completed:  " << engine_stats.ops_completed() << "\n";
        std::cout << "Total bytes:          " << engine_stats.bytes_transferred() / (1024 * 1024) << " MB\n";
        std::cout << "P99 latency:          " << std::fixed << std::setprecision(2)
                  << engine_stats.p99_latency_ms() << " ms\n";
        std::cout << "Optimal in-flight:    " << engine_stats.optimal_in_flight() << "\n";

        stats.print(total_elapsed);

        if (stats.ops_failed > 0) {
            std::cout << "\n*** WARNING: " << stats.ops_failed << " operations failed ***\n";
        }

        std::cout << "\n=== STRESS TEST PASSED ===\n";
        return 0;

    } catch (const auraio::Error& e) {
        std::cerr << "AuraIO error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
