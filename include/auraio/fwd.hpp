/**
 * @file fwd.hpp
 * @brief Forward declarations for AuraIO C++ bindings
 */

#ifndef AURAIO_FWD_HPP
#define AURAIO_FWD_HPP

namespace auraio {

class Engine;
class Buffer;
class BufferRef;
class Request;
class Options;
class Stats;
class RingStats;
class Histogram;
class BufferStats;
class Error;

template<typename T = void>
class Task;

} // namespace auraio

#endif // AURAIO_FWD_HPP
