/**
 * @file auracp.cpp
 * @brief auracp_cpp - async pipelined file copy powered by AuraIO (C++ version)
 *
 * A production-quality cp replacement that uses AuraIO's C++20 bindings with
 * pipelined async I/O and AIMD adaptive tuning for high-throughput file copying.
 * Supports single file, multi-file, and recursive directory copy with
 * cross-file pipelining.
 *
 * Usage: auracp_cpp [OPTIONS] SOURCE... DEST
 */

#ifndef _GNU_SOURCE
#    define _GNU_SOURCE
#endif
#include <aura.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// ============================================================================
// Constants
// ============================================================================

static constexpr size_t DEFAULT_CHUNK_SIZE = 256 * 1024; // 256 KiB
static constexpr int DEFAULT_PIPELINE = 8;
static constexpr int MAX_PIPELINE = 64;
static constexpr uint64_t PROGRESS_INTERVAL_MS = 200;
static constexpr int SECTOR_SIZE = 512;
static constexpr int NFTW_MAX_FDS = 64;

// ============================================================================
// Configuration
// ============================================================================

struct Config {
    std::vector<const char *> sources;
    const char *dest = nullptr;
    size_t chunk_size = DEFAULT_CHUNK_SIZE;
    int pipeline_depth = DEFAULT_PIPELINE;
    bool recursive = false;
    bool use_direct = false;
    bool quiet = false;
    bool no_progress = false;
    bool no_fsync = false;
    bool preserve = false;
    bool verbose = false;
    bool keep_partial = false;
};

// ============================================================================
// File task queue
// ============================================================================

struct FileTask {
    std::string src_path;
    std::string dst_path;
    off_t file_size = 0;
    mode_t mode = 0;
    int src_fd = -1;
    int dst_fd = -1;
    off_t read_offset = 0;
    off_t bytes_written = 0;
    int active_ops = 0;
    bool reads_done = false;
    bool done = false;
};

struct TaskQueue {
    std::vector<FileTask> tasks;
    size_t current = 0; // index of next task needing reads
    int completed_files = 0;
    off_t total_bytes = 0;
    off_t total_written = 0;

    void push(FileTask task) {
        total_bytes += task.file_size;
        tasks.push_back(std::move(task));
    }

    int total_files() const { return static_cast<int>(tasks.size()); }
};

// ============================================================================
// Pipeline buffer slots
// ============================================================================

enum class BufState { Free, Reading, Writing };

struct BufSlot {
    aura::Buffer buf;
    off_t offset = 0;
    size_t bytes = 0;
    BufState state = BufState::Free;
    size_t task_idx = 0;
};

// ============================================================================
// Forward declarations
// ============================================================================

struct CopyContext;

// ============================================================================
// Global state for signal handling
// ============================================================================

static volatile sig_atomic_t g_interrupted = 0;
static struct timespec g_start_time;
static uint64_t g_last_progress_ns = 0;

static void sigint_handler(int /*sig*/) {
    g_interrupted = 1;
}

// ============================================================================
// Size parsing
// ============================================================================

static ssize_t parse_size(const char *str) {
    char *endp;
    double val = strtod(str, &endp);
    if (endp == str || val < 0) return -1;

    switch (*endp) {
    case 'G':
    case 'g':
        val *= 1024.0 * 1024.0 * 1024.0;
        break;
    case 'M':
    case 'm':
        val *= 1024.0 * 1024.0;
        break;
    case 'K':
    case 'k':
        val *= 1024.0;
        break;
    case '\0':
        break;
    default:
        return -1;
    }

    if (val > static_cast<double>(SSIZE_MAX)) return -1;
    return static_cast<ssize_t>(val);
}

// ============================================================================
// Formatting helpers
// ============================================================================

static void format_bytes(char *buf, size_t bufsz, double bytes) {
    if (bytes >= 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f GiB", bytes / (1024.0 * 1024.0 * 1024.0));
    else if (bytes >= 1024.0 * 1024.0) snprintf(buf, bufsz, "%.1f MiB", bytes / (1024.0 * 1024.0));
    else if (bytes >= 1024.0) snprintf(buf, bufsz, "%.1f KiB", bytes / 1024.0);
    else snprintf(buf, bufsz, "%.0f B", bytes);
}

static void format_rate(char *buf, size_t bufsz, double bps) {
    if (bps >= 1024.0 * 1024.0 * 1024.0)
        snprintf(buf, bufsz, "%.1f GiB/s", bps / (1024.0 * 1024.0 * 1024.0));
    else if (bps >= 1024.0 * 1024.0) snprintf(buf, bufsz, "%.1f MiB/s", bps / (1024.0 * 1024.0));
    else if (bps >= 1024.0) snprintf(buf, bufsz, "%.1f KiB/s", bps / 1024.0);
    else snprintf(buf, bufsz, "%.0f B/s", bps);
}

// ============================================================================
// Progress display
// ============================================================================

static void progress_update(const Config &config, const TaskQueue &queue, bool final) {
    if (config.quiet || config.no_progress) return;
    if (!final && !isatty(STDERR_FILENO)) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t now_ns =
        static_cast<uint64_t>(now.tv_sec) * 1000000000ULL + static_cast<uint64_t>(now.tv_nsec);

    if (!final && (now_ns - g_last_progress_ns) < PROGRESS_INTERVAL_MS * 1000000ULL) return;
    g_last_progress_ns = now_ns;

    double elapsed = static_cast<double>(now.tv_sec - g_start_time.tv_sec) +
                     static_cast<double>(now.tv_nsec - g_start_time.tv_nsec) / 1e9;
    double done = static_cast<double>(queue.total_written);
    double total = static_cast<double>(queue.total_bytes);
    double pct = (total > 0) ? done / total * 100.0 : 100.0;
    double rate = (elapsed > 0.01) ? done / elapsed : 0.0;

    char done_str[32], total_str[32], rate_str[32];
    format_bytes(done_str, sizeof(done_str), done);
    format_bytes(total_str, sizeof(total_str), total);
    format_rate(rate_str, sizeof(rate_str), rate);

    // Progress bar
    int bar_width = 30;
    int filled = (total > 0) ? static_cast<int>(pct / 100.0 * bar_width) : bar_width;
    if (filled > bar_width) filled = bar_width;

    char bar[64];
    int i;
    for (i = 0; i < filled && i < bar_width; i++) bar[i] = '=';
    if (filled < bar_width) {
        bar[filled] = '>';
        for (i = filled + 1; i < bar_width; i++) bar[i] = ' ';
    }
    bar[bar_width] = '\0';

    // ETA
    char eta[32] = "";
    if (rate > 0 && total > done) {
        double remaining = (total - done) / rate;
        int mins = static_cast<int>(remaining / 60.0);
        int secs = static_cast<int>(remaining) % 60;
        snprintf(eta, sizeof(eta), "ETA %d:%02d", mins, secs);
    }

    fprintf(stderr, "\r  %s / %s  [%s]  %3.0f%%  %s  %s   ", done_str, total_str, bar, pct,
            rate_str, eta);

    if (final) fprintf(stderr, "\n");
}

// ============================================================================
// Path helpers
// ============================================================================

static std::string path_join(const char *dir, const char *name) {
    std::string d(dir);
    // Trim trailing slashes
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    d += '/';
    d += name;
    return d;
}

static std::string remap_path(const char *src_root, const char *dst_root, const char *fpath) {
    size_t rootlen = strlen(src_root);
    while (rootlen > 1 && src_root[rootlen - 1] == '/') rootlen--;
    const char *suffix = fpath + rootlen;
    if (*suffix == '/') suffix++;

    std::string d(dst_root);
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    d += '/';
    d += suffix;
    return d;
}

// ============================================================================
// Task list building
// ============================================================================

struct WalkContext {
    const char *src_root;
    const char *dst_root;
    TaskQueue *queue;
    const Config *config;
    int errors;
};

static WalkContext *g_walk_ctx; // nftw doesn't support user_data

static int nftw_callback(const char *fpath, const struct stat *sb, int typeflag,
                         struct FTW * /*ftwbuf*/) {
    WalkContext *wc = g_walk_ctx;

    if (typeflag == FTW_D) {
        auto dst = remap_path(wc->src_root, wc->dst_root, fpath);

        if (strcmp(fpath, wc->src_root) == 0) {
            struct stat dst_st;
            if (stat(dst.c_str(), &dst_st) != 0) {
                if (mkdir(dst.c_str(), sb->st_mode & 07777) != 0 && errno != EEXIST) {
                    fprintf(stderr, "auracp: cannot create directory '%s': %s\n", dst.c_str(),
                            strerror(errno));
                    wc->errors++;
                }
            }
        } else {
            if (mkdir(dst.c_str(), sb->st_mode & 07777) != 0 && errno != EEXIST) {
                fprintf(stderr, "auracp: cannot create directory '%s': %s\n", dst.c_str(),
                        strerror(errno));
                wc->errors++;
            }
        }
        return 0;
    }

    if (typeflag == FTW_F) {
        auto dst = remap_path(wc->src_root, wc->dst_root, fpath);

        FileTask task;
        task.src_path = fpath;
        task.dst_path = std::move(dst);
        task.file_size = sb->st_size;
        task.mode = sb->st_mode;
        wc->queue->push(std::move(task));
        return 0;
    }

    if (typeflag != FTW_SL) {
        if (!wc->config->quiet) fprintf(stderr, "auracp: skipping special file: %s\n", fpath);
    }
    return 0;
}

static int build_recursive(const char *src, const char *dst, TaskQueue &queue,
                           const Config &config) {
    WalkContext wc = {src, dst, &queue, &config, 0};
    g_walk_ctx = &wc;

    if (nftw(src, nftw_callback, NFTW_MAX_FDS, 0) != 0) {
        fprintf(stderr, "auracp: cannot walk '%s': %s\n", src, strerror(errno));
        return -1;
    }
    return wc.errors ? -1 : 0;
}

static int build_task_list(const Config &config, TaskQueue &queue) {
    struct stat dst_st;
    bool dst_is_dir = (stat(config.dest, &dst_st) == 0 && S_ISDIR(dst_st.st_mode));

    if (config.sources.size() > 1 && !dst_is_dir) {
        fprintf(stderr, "auracp: target '%s' is not a directory\n", config.dest);
        return -1;
    }

    for (const char *src : config.sources) {
        struct stat src_st;

        if (stat(src, &src_st) != 0) {
            fprintf(stderr, "auracp: cannot stat '%s': %s\n", src, strerror(errno));
            return -1;
        }

        if (S_ISDIR(src_st.st_mode)) {
            if (!config.recursive) {
                fprintf(stderr, "auracp: -r not specified; omitting directory '%s'\n", src);
                return -1;
            }

            std::string dst_dir;
            if (dst_is_dir) {
                // auracp -r src/ existing_dir/ -> existing_dir/src/...
                std::string src_copy(src);
                char *base = basename(src_copy.data());
                dst_dir = path_join(config.dest, base);
            } else {
                dst_dir = config.dest;
            }

            if (mkdir(dst_dir.c_str(), src_st.st_mode & 07777) != 0 && errno != EEXIST) {
                fprintf(stderr, "auracp: cannot create directory '%s': %s\n", dst_dir.c_str(),
                        strerror(errno));
                return -1;
            }

            if (build_recursive(src, dst_dir.c_str(), queue, config) != 0) return -1;
        } else if (S_ISREG(src_st.st_mode)) {
            std::string dst_path;
            if (dst_is_dir) {
                std::string src_copy(src);
                char *base = basename(src_copy.data());
                dst_path = path_join(config.dest, base);
            } else {
                dst_path = config.dest;
            }

            // Check same file
            if (dst_is_dir) {
                struct stat check_st;
                if (stat(dst_path.c_str(), &check_st) == 0 && src_st.st_dev == check_st.st_dev &&
                    src_st.st_ino == check_st.st_ino) {
                    fprintf(stderr, "auracp: '%s' and '%s' are the same file\n", src,
                            dst_path.c_str());
                    return -1;
                }
            }

            FileTask task;
            task.src_path = src;
            task.dst_path = std::move(dst_path);
            task.file_size = src_st.st_size;
            task.mode = src_st.st_mode;
            queue.push(std::move(task));
        } else {
            fprintf(stderr, "auracp: skipping special file: %s\n", src);
        }
    }

    if (queue.tasks.empty()) {
        fprintf(stderr, "auracp: no files to copy\n");
        return -1;
    }
    return 0;
}

// ============================================================================
// Copy context
// ============================================================================

struct CopyContext {
    aura::Engine &engine;
    const Config &config;
    TaskQueue &queue;
    std::vector<BufSlot> &slots;
    int active_ops = 0;
    int error = 0;

    void submit_next_read(size_t slot_idx);
    void on_read_complete(size_t slot_idx, ssize_t result);
    void on_write_complete(size_t slot_idx, ssize_t result);
    void finish_task(FileTask &task);
    int copy_pipeline();

  private:
    int open_task(FileTask &task);
    bool all_tasks_done() const;
};

// ============================================================================
// I/O pipeline implementation
// ============================================================================

int CopyContext::open_task(FileTask &task) {
    int src_flags = O_RDONLY;
    if (config.use_direct) src_flags |= O_DIRECT;

    task.src_fd = open(task.src_path.c_str(), src_flags);
    if (task.src_fd < 0) {
        fprintf(stderr, "auracp: cannot open '%s': %s\n", task.src_path.c_str(), strerror(errno));
        return -1;
    }

    int dst_flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (config.use_direct) dst_flags |= O_DIRECT;

    task.dst_fd = open(task.dst_path.c_str(), dst_flags, task.mode & 07777);
    if (task.dst_fd < 0) {
        fprintf(stderr, "auracp: cannot create '%s': %s\n", task.dst_path.c_str(), strerror(errno));
        close(task.src_fd);
        task.src_fd = -1;
        return -1;
    }

    // Pre-allocate destination (best-effort)
    if (task.file_size > 0) posix_fallocate(task.dst_fd, 0, task.file_size);

    // Copy permissions
    fchmod(task.dst_fd, task.mode & 07777);

    return 0;
}

void CopyContext::finish_task(FileTask &task) {
    queue.completed_files++;

    // Preserve timestamps if requested
    if (config.preserve && task.dst_fd >= 0) {
        struct stat src_st;
        if (fstat(task.src_fd, &src_st) == 0) {
            struct timespec times[2] = {src_st.st_atim, src_st.st_mtim};
            futimens(task.dst_fd, times);
        }
    }

    // Fsync if requested, then close
    if (!config.no_fsync && task.dst_fd >= 0) {
        try {
            int src_fd = task.src_fd;
            int dst_fd = task.dst_fd;
            active_ops++;
            (void)engine.fdatasync(dst_fd,
                                   [this, &task, src_fd, dst_fd](aura::Request &, ssize_t result) {
                                       if (result < 0) {
                                           fprintf(stderr, "auracp: fsync failed for '%s': %s\n",
                                                   task.dst_path.c_str(), strerror(-(int)result));
                                           if (error == 0) error = (int)result;
                                       }
                                       close(src_fd);
                                       close(dst_fd);
                                       active_ops--;
                                   });
            task.src_fd = -1;
            task.dst_fd = -1;
            task.done = true;
        } catch (...) {
            active_ops--;
            // Fallback: sync and close synchronously
            fdatasync(task.dst_fd);
            close(task.src_fd);
            close(task.dst_fd);
            task.src_fd = -1;
            task.dst_fd = -1;
            task.done = true;
        }
    } else {
        close(task.src_fd);
        close(task.dst_fd);
        task.src_fd = -1;
        task.dst_fd = -1;
        task.done = true;
    }
}

void CopyContext::on_read_complete(size_t slot_idx, ssize_t result) {
    auto &slot = slots[slot_idx];
    auto &task = queue.tasks[slot.task_idx];

    if (result <= 0) {
        if (result < 0 && error == 0) {
            fprintf(stderr, "auracp: read error on '%s': %s\n", task.src_path.c_str(),
                    strerror(static_cast<int>(-result)));
            error = static_cast<int>(result);
        }
        slot.state = BufState::Free;
        active_ops--;
        task.active_ops--;
        return;
    }

    // Submit write for the data we just read
    slot.bytes = static_cast<size_t>(result);
    slot.state = BufState::Writing;

    try {
        (void)engine.write(
            task.dst_fd, slot.buf, slot.bytes, slot.offset,
            [this, slot_idx](aura::Request &, ssize_t res) { on_write_complete(slot_idx, res); });
    } catch (...) {
        if (error == 0) {
            fprintf(stderr, "auracp: write submit failed on '%s': %s\n", task.dst_path.c_str(),
                    strerror(errno));
            error = -errno;
        }
        slot.state = BufState::Free;
        active_ops--;
        task.active_ops--;
    }
}

void CopyContext::on_write_complete(size_t slot_idx, ssize_t result) {
    auto &slot = slots[slot_idx];
    auto &task = queue.tasks[slot.task_idx];

    if (result < 0) {
        if (error == 0) {
            fprintf(stderr, "auracp: write error on '%s': %s\n", task.dst_path.c_str(),
                    strerror(static_cast<int>(-result)));
            error = static_cast<int>(result);
        }
    } else if (static_cast<size_t>(result) != slot.bytes) {
        if (error == 0) {
            fprintf(stderr, "auracp: short write on '%s'\n", task.dst_path.c_str());
            error = -EIO;
        }
    } else {
        task.bytes_written += result;
        queue.total_written += result;
    }

    slot.state = BufState::Free;
    active_ops--;
    task.active_ops--;

    // Check if this file is fully done
    if (task.reads_done && task.active_ops == 0 && !task.done) {
        finish_task(task);
    }
}

void CopyContext::submit_next_read(size_t slot_idx) {
    // Find a task that needs reads
    size_t idx = queue.current;
    while (idx < queue.tasks.size()) {
        auto &t = queue.tasks[idx];
        if (!t.reads_done && !t.done) break;
        idx++;
    }
    if (idx >= queue.tasks.size()) return; // all reads submitted

    queue.current = idx;
    auto &task = queue.tasks[idx];

    // Open file if needed
    if (task.src_fd < 0) {
        if (open_task(task) != 0) {
            task.reads_done = true;
            task.done = true;
            queue.completed_files++;
            queue.current = idx + 1;
            submit_next_read(slot_idx);
            return;
        }

        // Handle zero-length file
        if (task.file_size == 0) {
            task.reads_done = true;
            finish_task(task);
            queue.current = idx + 1;
            submit_next_read(slot_idx);
            return;
        }
    }

    size_t chunk = config.chunk_size;
    if (task.read_offset + static_cast<off_t>(chunk) > task.file_size)
        chunk = static_cast<size_t>(task.file_size - task.read_offset);

    auto &slot = slots[slot_idx];
    slot.offset = task.read_offset;
    slot.bytes = chunk;
    slot.state = BufState::Reading;
    slot.task_idx = idx;
    task.read_offset += static_cast<off_t>(chunk);

    if (task.read_offset >= task.file_size) {
        task.reads_done = true;
        queue.current = idx + 1;
    }

    try {
        (void)engine.read(task.src_fd, slot.buf, chunk, slot.offset,
                          [this, slot_idx](aura::Request &, ssize_t result) {
                              on_read_complete(slot_idx, result);
                          });
        active_ops++;
        task.active_ops++;
    } catch (...) {
        if (error == 0) {
            fprintf(stderr, "auracp: read submit failed on '%s': %s\n", task.src_path.c_str(),
                    strerror(errno));
            error = -errno;
        }
        slot.state = BufState::Free;
        task.read_offset -= static_cast<off_t>(chunk);
    }
}

bool CopyContext::all_tasks_done() const {
    for (const auto &t : queue.tasks) {
        if (!t.done) return false;
    }
    return true;
}

int CopyContext::copy_pipeline() {
    int depth = config.pipeline_depth;

    // Submit initial batch
    for (int i = 0; i < depth && error == 0 && !g_interrupted; i++) {
        submit_next_read(static_cast<size_t>(i));
    }

    // Main event loop
    while (!all_tasks_done() && error == 0 && !g_interrupted) {
        try {
            engine.wait(100);
        } catch (const aura::Error &e) {
            if (e.code() != EINTR && e.code() != ETIME && e.code() != ETIMEDOUT) {
                if (error == 0) error = -e.code();
                break;
            }
        }

        // Fill pipeline with new reads for free slots
        for (int i = 0; i < depth; i++) {
            if (slots[static_cast<size_t>(i)].state == BufState::Free && error == 0 &&
                !g_interrupted) {
                submit_next_read(static_cast<size_t>(i));
            }
        }

        progress_update(config, queue, false);
    }

    // Drain remaining ops if exiting due to error/interrupt
    while (active_ops > 0) {
        try {
            engine.wait(100);
        } catch (const aura::Error &) {
            // Ignore timeout/errors during drain
        }
    }

    return error;
}

// ============================================================================
// Cleanup helpers
// ============================================================================

static void cleanup_partial_files(const TaskQueue &queue, bool keep) {
    if (keep) return;
    for (const auto &t : queue.tasks) {
        if (!t.done && !t.dst_path.empty()) {
            unlink(t.dst_path.c_str());
        }
    }
}

static void close_remaining_fds(TaskQueue &queue) {
    for (auto &t : queue.tasks) {
        if (t.src_fd >= 0) {
            close(t.src_fd);
            t.src_fd = -1;
        }
        if (t.dst_fd >= 0) {
            close(t.dst_fd);
            t.dst_fd = -1;
        }
    }
}

// ============================================================================
// CLI parsing
// ============================================================================

static void print_usage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [OPTIONS] SOURCE... DEST\n"
            "\n"
            "Async pipelined file copy powered by AuraIO (C++ version).\n"
            "\n"
            "Options:\n"
            "  -r, --recursive      Copy directories recursively\n"
            "  -b, --block-size N   I/O block size (default: 256K). Suffixes: K, M, G\n"
            "  -d, --direct         Use O_DIRECT (bypass page cache)\n"
            "  -p, --pipeline N     In-flight I/O buffer slots (default: %d, max: %d)\n"
            "  -q, --quiet          Suppress all output\n"
            "  --no-fsync           Skip per-file fsync\n"
            "  --no-progress        Disable progress bar\n"
            "  --preserve           Preserve timestamps (mtime, atime)\n"
            "  --keep-partial       Don't delete partial files on error\n"
            "  -v, --verbose        Show AuraIO stats after copy\n"
            "  -h, --help           Show this help\n",
            argv0, DEFAULT_PIPELINE, MAX_PIPELINE);
}

static int parse_args(int argc, char **argv, Config &config) {
    static struct option long_opts[] = {{"recursive", no_argument, nullptr, 'r'},
                                        {"block-size", required_argument, nullptr, 'b'},
                                        {"direct", no_argument, nullptr, 'd'},
                                        {"pipeline", required_argument, nullptr, 'p'},
                                        {"quiet", no_argument, nullptr, 'q'},
                                        {"no-fsync", no_argument, nullptr, 'F'},
                                        {"no-progress", no_argument, nullptr, 'P'},
                                        {"preserve", no_argument, nullptr, 'T'},
                                        {"keep-partial", no_argument, nullptr, 'K'},
                                        {"verbose", no_argument, nullptr, 'v'},
                                        {"help", no_argument, nullptr, 'h'},
                                        {nullptr, 0, nullptr, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "rb:dp:qvh", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'r':
            config.recursive = true;
            break;
        case 'b': {
            ssize_t sz = parse_size(optarg);
            if (sz <= 0) {
                fprintf(stderr, "auracp: invalid block size: %s\n", optarg);
                return -1;
            }
            config.chunk_size = static_cast<size_t>(sz);
            break;
        }
        case 'd':
            config.use_direct = true;
            break;
        case 'p': {
            char *end;
            long val = strtol(optarg, &end, 10);
            if (*end != '\0' || val < 1 || val > MAX_PIPELINE) {
                fprintf(stderr, "auracp: pipeline must be 1-%d\n", MAX_PIPELINE);
                return -1;
            }
            config.pipeline_depth = static_cast<int>(val);
        } break;
        case 'q':
            config.quiet = true;
            config.no_progress = true;
            break;
        case 'v':
            config.verbose = true;
            break;
        case 'F':
            config.no_fsync = true;
            break;
        case 'P':
            config.no_progress = true;
            break;
        case 'T':
            config.preserve = true;
            break;
        case 'K':
            config.keep_partial = true;
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    int remaining = argc - optind;
    if (remaining < 2) {
        fprintf(stderr, "auracp: expected SOURCE... DEST arguments\n");
        print_usage(argv[0]);
        return -1;
    }

    for (int i = optind; i < argc - 1; i++) config.sources.push_back(argv[i]);
    config.dest = argv[argc - 1];

    if (config.use_direct && (config.chunk_size % SECTOR_SIZE) != 0) {
        fprintf(stderr, "auracp: block size must be sector-aligned (%d) with --direct\n",
                SECTOR_SIZE);
        return -1;
    }

    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    Config config;
    if (parse_args(argc, argv, config) != 0) return 1;

    // Install SIGINT handler
    struct sigaction sa = {};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);

    // Build task list
    TaskQueue queue;
    if (build_task_list(config, queue) != 0) return 1;

    if (!config.quiet) {
        char total_str[32];
        format_bytes(total_str, sizeof(total_str), static_cast<double>(queue.total_bytes));
        fprintf(stderr, "auracp: %d file%s (%s)\n", queue.total_files(),
                queue.total_files() == 1 ? "" : "s", total_str);
    }

    try {
        // Create AuraIO engine with options
        aura::Options opts;
        int qd = config.pipeline_depth * 4;
        if (qd < 64) qd = 64;
        opts.queue_depth(qd).single_thread(true).ring_count(1).ring_select(
            aura::RingSelect::ThreadLocal);

        aura::Engine engine(opts);

        // Allocate pipeline buffer slots
        std::vector<BufSlot> slots(static_cast<size_t>(config.pipeline_depth));
        for (auto &slot : slots) {
            slot.buf = engine.allocate_buffer(config.chunk_size);
        }

        // Set up copy context
        CopyContext ctx{engine, config, queue, slots};

        // Run copy
        clock_gettime(CLOCK_MONOTONIC, &g_start_time);
        int err = ctx.copy_pipeline();

        // Final progress
        progress_update(config, queue, true);

        // Verbose stats
        if (config.verbose && err == 0 && !g_interrupted) {
            struct timespec end;
            clock_gettime(CLOCK_MONOTONIC, &end);
            double elapsed = static_cast<double>(end.tv_sec - g_start_time.tv_sec) +
                             static_cast<double>(end.tv_nsec - g_start_time.tv_nsec) / 1e9;

            char size_str[32], rate_str[32], chunk_str[32], pipe_str[32];
            format_bytes(size_str, sizeof(size_str), static_cast<double>(queue.total_written));
            format_rate(rate_str, sizeof(rate_str),
                        elapsed > 0 ? static_cast<double>(queue.total_written) / elapsed : 0);
            format_bytes(chunk_str, sizeof(chunk_str), static_cast<double>(config.chunk_size));
            format_bytes(pipe_str, sizeof(pipe_str),
                         static_cast<double>(config.chunk_size) * config.pipeline_depth);

            fprintf(stderr, "Copied %d file%s (%s) in %.2fs\n", queue.completed_files,
                    queue.completed_files == 1 ? "" : "s", size_str, elapsed);
            fprintf(stderr, "Throughput: %s\n", rate_str);
            fprintf(stderr, "Pipeline: %d buffers x %s = %s\n", config.pipeline_depth, chunk_str,
                    pipe_str);

            try {
                auto rstats = engine.get_ring_stats(0);
                fprintf(stderr, "AuraIO: depth=%d, phase=%s, p99=%.2fms\n",
                        rstats.in_flight_limit(), rstats.aimd_phase_name(),
                        rstats.p99_latency_ms());
            } catch (...) {
                // Stats failure is non-fatal
            }
        }

        // Close any remaining fds
        close_remaining_fds(queue);

        // Handle errors / interrupts
        if (err != 0 || g_interrupted) {
            cleanup_partial_files(queue, config.keep_partial);
            if (g_interrupted) {
                fprintf(stderr, "\nauracp: interrupted\n");
                return 130;
            }
            return 1;
        }

        return 0;

    } catch (const aura::Error &e) {
        fprintf(stderr, "auracp: engine error: %s\n", e.what());
        close_remaining_fds(queue);
        return 1;
    } catch (const std::exception &e) {
        fprintf(stderr, "auracp: error: %s\n", e.what());
        close_remaining_fds(queue);
        return 1;
    }
}
