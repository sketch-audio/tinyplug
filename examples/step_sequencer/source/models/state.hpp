#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// Sixteen steps over one octave, edited in the UI only. A step holds a key or a rest.
struct State {
    static constexpr auto writers = state::Writers::Editor;
    static constexpr auto num_steps = std::size_t{16};
    static constexpr auto rest = std::uint8_t{0xff};

    std::array<std::uint8_t, num_steps> key{
        60, rest, 63, rest, 67, rest, 63, 70, 60, rest, 63, rest, 67, 68, 67, 63
    };

    static auto save(state::Writer out, const State& value) -> bool
    {
        return out.write(std::uint32_t{1}) && out.write(value.key);
    }

    static auto load(state::Reader in, State& value) -> bool
    {
        auto version = std::uint32_t{};
        if (!in.read(version) || version > 1) return false;
        return in.read(value.key);
    }
};
static_assert(state::Model<State>);
static_assert(state::byte_comparable<State>);

} // namespace tiny::models
