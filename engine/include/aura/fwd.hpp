/**
 * @file fwd.hpp
 * @brief Forward declarations for AuraIO C++ bindings
 */

#ifndef AURA_FWD_HPP
#define AURA_FWD_HPP

namespace aura {

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

class OpenatAwaitable;
class MetadataAwaitable;
class StatxAwaitable;

template<typename T = void>
class Task;

} // namespace aura

#endif // AURA_FWD_HPP
