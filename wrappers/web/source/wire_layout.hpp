#pragma once

#include <cstddef>
#include <cstdint>

#include <tiny_core/wire.hpp>

// The web format's channels between the processor module (AudioWorklet) and the controller (main thread).
// Every payload is trivially copyable and laid out for wire.js, which reads the same offsets.
namespace tiny::web {

enum class Kind : std::uint32_t {
    // Controller -> processor.
    Param_set = 0,  // Param_set
    Param_ramp,     // Param_ramp
    Bypass,         // Flag
    Reset_hard,     // (none)
    Transport,      // Transport

    // Processor -> controller.
    Meter = 64,     // Meter
    Latency,        // Samples: the latency the processor now has, applied.
};

struct Param_set {
    double value{}; // Plain space.
    std::uint32_t address{};
    std::int32_t offset{}; // Frames into the next quantum.
};
static_assert(sizeof(Param_set) == 16);

struct Param_ramp {
    double value{}; // Plain space, the ramp's target.
    std::uint32_t address{};
    std::int32_t samples{};
    std::int32_t offset{};
    std::uint32_t pad{};
};
static_assert(sizeof(Param_ramp) == 24);

struct Flag {
    std::uint32_t on{};
    std::uint32_t pad{};
};
static_assert(sizeof(Flag) == 8);

// The page's transport: a position anchored to a processor frame, so it advances between messages.
struct Transport {
    double tempo{};             // BPM; 0 when the page has none.
    std::int64_t position{};    // Samples, at `anchor`.
    std::int64_t anchor{};      // The processor's frame count the position refers to.
    std::int32_t numer{};       // 0 when the page has no signature.
    std::int32_t denom{};
    std::uint32_t playing{};
    std::uint32_t pad{};
};
static_assert(sizeof(Transport) == 40);

struct Meter {
    std::uint32_t address{};
    float value{};
};
static_assert(sizeof(Meter) == 8);

struct Samples {
    std::uint32_t samples{};
    std::uint32_t pad{};
};
static_assert(sizeof(Samples) == 8);

inline constexpr auto inbound_bytes = std::size_t{16 * 1024};
inline constexpr auto outbound_bytes = std::size_t{16 * 1024};

using Inbound_ring = wire::Byte_ring<inbound_bytes, Kind>;
using Outbound_ring = wire::Byte_ring<outbound_bytes, Kind>;

// What the processor module publishes about its memory, read once by the JS side. Byte offsets into the
// module's memory; capacities in bytes.
struct Wire_table {
    std::uint32_t version{1};
    std::uint32_t inbound{};
    std::uint32_t inbound_capacity{};
    std::uint32_t outbound{};
    std::uint32_t outbound_capacity{};
    std::uint32_t inputs{};     // max_channels pointers' worth of float buffers, each max_frames long.
    std::uint32_t sidechain{};
    std::uint32_t outputs{};
    std::uint32_t max_channels{};
    std::uint32_t max_frames{};
    std::uint32_t params{};     // num_params doubles, plain: the values `configure` comes up holding.
    std::uint32_t num_params{};
};

} // namespace tiny::web
