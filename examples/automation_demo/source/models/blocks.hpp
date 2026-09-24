#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// One thing the host said to the processor, at the frame it landed (counted from `configure`).
struct Log_entry {
    enum class Kind : uint8_t { Configure, Set, Ramp, Hard, Soft, Latency, Render };
    uint32_t seq{};     // From 1; 0 is an empty slot.
    Kind kind{};
    uint8_t address{};
    int32_t dur{};      // Ramp length, or accepted latency, in frames.
    int64_t frame{};
    double value{};     // Plain space; for Render, 1 offline.
};

// The last `size` entries, by sequence: slot `seq % size`. The editor takes what it hasn't seen,
// and knows how many it missed.
struct Log_frame {
    static constexpr auto size = size_t{32};
    std::array<Log_entry, size> entries{};
    uint32_t next{1};
};

// The realized Value over the last few seconds, one min / max pair per bucket.
struct Trace_frame {
    static constexpr auto size = size_t{400};
    std::array<float, size> lo{};
    std::array<float, size> hi{};
    uint32_t write{};     // The bucket being filled.
    int64_t head{};       // The frame that bucket started at.
    int32_t bucket{1};    // Frames per bucket.
};

struct Blocks {
    using Types = std::variant<Log_frame, Trace_frame>;

    enum class Address : std::uint32_t {
        Log = 0,
        Trace,
        Num_blocks
    };
    static constexpr auto num_blocks = enum_raw(Address::Num_blocks);

    static constexpr auto make_spec(std::uint32_t address) -> blocks::Spec
    {
        switch (static_cast<Address>(address)) {
            case Address::Log: return {blocks::kind_of<Log_frame, Types>};
            case Address::Trace: return {blocks::kind_of<Trace_frame, Types>};
            case Address::Num_blocks:
            default: return {};
        }
    }
};
static_assert(blocks::Model<Blocks>);

} // namespace tiny::models
