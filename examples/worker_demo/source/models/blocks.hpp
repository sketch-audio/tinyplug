#pragma once

#include <tiny_core/tiny_core.hpp>

#include "work.hpp"

namespace tiny::models {

// The processor's side of the channel, for the editor: what it sent and got back, and the curve
// it is shaping with.
struct Channel_frame {
    uint32_t heartbeats{};    // Processor -> worker.
    uint32_t echoes{};        // Worker -> processor.
    int64_t round_trip{};     // Frames, the last heartbeat's.
    uint32_t designs{};       // Processor -> worker.
    uint32_t curves{};        // Worker -> processor.
    double drive{};           // The curve's drive.
    Curve_table table{};
    double sr{48000};
};

struct Blocks {
    using Types = std::variant<Channel_frame>;

    enum class Address : std::uint32_t {
        Channel = 0,
        Num_blocks
    };
    static constexpr auto num_blocks = enum_raw(Address::Num_blocks);

    static constexpr auto make_spec(std::uint32_t address) -> blocks::Spec
    {
        switch (static_cast<Address>(address)) {
            case Address::Channel: return {blocks::kind_of<Channel_frame, Types>};
            case Address::Num_blocks:
            default: return {};
        }
    }
};
static_assert(blocks::Model<Blocks>);

} // namespace tiny::models
