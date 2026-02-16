/**
 * @file aura.hpp
 * @brief Main header for AuraIO C++ bindings
 *
 * This is the single header you need to include to use AuraIO from C++.
 * It provides a modern C++20 interface with RAII, exceptions, and coroutine support.
 *
 * Example (callback-based):
 * @code
 * #include <aura.hpp>
 *
 * int main() {
 *     aura::Engine engine;
 *     auto buffer = engine.allocate_buffer(4096);
 *
 *     int fd = open("file.txt", O_RDONLY | O_DIRECT);
 *     engine.read(fd, buffer, 4096, 0, [](aura::Request&, ssize_t n) {
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
 * #include <aura.hpp>
 *
 * aura::Task<void> copy_file(aura::Engine& engine, int src, int dst) {
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

#ifndef AURA_HPP
#define AURA_HPP

// C API
#include <aura.h>

// C++ bindings (order matters for dependencies)
#include <aura/fwd.hpp>
#include <aura/error.hpp>
#include <aura/log.hpp>
#include <aura/options.hpp>
#include <aura/request.hpp>
#include <aura/stats.hpp>
#include <aura/buffer.hpp>
#include <aura/detail/callback_storage.hpp>
#include <aura/engine.hpp>
#include <aura/coro.hpp>

#endif // AURA_HPP
