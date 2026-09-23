#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// What each key is doing, for the on-screen keyboard: 0 silent, else 1 + pressure × 254.
// Covers notes from the host as well as the keyboard's own.
struct Keys_frame {
    std::array<std::uint8_t, 128> level{};
};

struct Blocks {
    using Types = std::variant<Keys_frame>;

    enum class Address : std::uint32_t {
        Keys = 0,
        Num_blocks
    };
    static constexpr auto num_blocks = enum_raw(Address::Num_blocks);

    static constexpr auto make_spec(std::uint32_t address) -> blocks::Spec
    {
        switch (static_cast<Address>(address)) {
            case Address::Keys: return {blocks::kind_of<Keys_frame, Types>};
            case Address::Num_blocks:
            default:            return {};
        }
    }
};
static_assert(blocks::Model<Blocks>);

} // namespace tiny::models
