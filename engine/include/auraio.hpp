/**
 * @file auraio.hpp
 * @brief Main header for AuraIO C++ bindings
 *
 * This is the single header you need to include to use AuraIO from C++.
 * It provides a modern C++20 interface with RAII, exceptions, and coroutine support.
 *
 * Example (callback-based):
 * @code
 * #include <auraio.hpp>
 *
 * int main() {
 *     auraio::Engine engine;
 *     auto buffer = engine.allocate_buffer(4096);
 *
 *     int fd = open("file.txt", O_RDONLY | O_DIRECT);
 *     engine.read(fd, buffer, 4096, 0, [](auraio::Request&, ssize_t n) {
 *         std::cout << "Read " << n << " bytes\n";
 *     });
 *
 *     engine.wait();
 *     close(fd);
 * }
 * @endcode
 *
 * Example (coroutine-based):
 * @code
 * #include <auraio.hpp>
 *
 * auraio::Task<void> copy_file(auraio::Engine& engine, int src, int dst) {
 *     auto buf = engine.allocate_buffer(4096);
 *     off_t offset = 0;
 *     while (true) {
 *         ssize_t n = co_await engine.async_read(src, buf, 4096, offset);
 *         if (n <= 0) break;
 *         co_await engine.async_write(dst, buf, n, offset);
 *         offset += n;
 *     }
 * }
 * @endcode
 */

#ifndef AURAIO_HPP
#define AURAIO_HPP

// C API
#include <auraio.h>

// C++ bindings (order matters for dependencies)
#include <auraio/fwd.hpp>
#include <auraio/error.hpp>
#include <auraio/log.hpp>
#include <auraio/options.hpp>
#include <auraio/request.hpp>
#include <auraio/stats.hpp>
#include <auraio/buffer.hpp>
#include <auraio/detail/callback_storage.hpp>
#include <auraio/engine.hpp>
#include <auraio/coro.hpp>

#endif // AURAIO_HPP
