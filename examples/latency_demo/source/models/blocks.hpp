#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// The handshake as the processor sees it: what the parameter asks for, what it proposed and is
// still waiting on, what it renders with, and the steps that got it there.
struct Handshake_frame {
    struct Step {
        enum class Kind : uint8_t { Configure, Propose, Accept, Hard };
        uint32_t seq{};     // From 1; 0 is an empty slot.
        Kind kind{};
        uint32_t samples{};
        int64_t frame{};    // Counted from `configure`.
        int64_t waited{};   // Accept: frames since the proposal it answered.
    };
    static constexpr auto size = size_t{16};

    std::array<Step, size> steps{}; // Slot `seq % size`.
    uint32_t next{1};
    uint32_t wanted{};
    uint32_t current{};
    int64_t pending{-1};  // The outstanding proposal, or -1.
    int64_t waiting{};    // Frames it has been outstanding.
    double sr{48000};
};

struct Blocks {
    using Types = std::variant<Handshake_frame>;

    enum class Address : std::uint32_t {
        Handshake = 0,
        Num_blocks
    };
    static constexpr auto num_blocks = enum_raw(Address::Num_blocks);

    static constexpr auto make_spec(std::uint32_t address) -> blocks::Spec
    {
        switch (static_cast<Address>(address)) {
            case Address::Handshake: return {blocks::kind_of<Handshake_frame, Types>};
            case Address::Num_blocks:
            default: return {};
        }
    }
};
static_assert(blocks::Model<Blocks>);

} // namespace tiny::models
