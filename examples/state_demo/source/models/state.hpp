#pragma once

#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

// A 16-step gate pattern, authored in the editor and read by the processor every block.
// Bytes only, so equal patterns are equal bytes and undo never records a phantom step.
struct State {
    static constexpr auto writers = state::Writers::Editor;
    static constexpr auto num_steps = std::size_t{16};

    std::array<std::uint8_t, num_steps> level{ // Percent, per sixteenth note.
        100, 30, 60, 30, 100, 30, 60, 30, 100, 30, 60, 30, 100, 30, 60, 30
    };

    // What a session stores. The version is ours: bump it when the payload changes, and keep
    // reading every version that has shipped.
    static auto save(state::Writer out, const State& value) -> bool
    {
        return out.write(std::uint32_t{1}) && out.write(value.level);
    }

    static auto load(state::Reader in, State& value) -> bool
    {
        auto version = std::uint32_t{};
        if (!in.read(version) || version > 1) return false; // From a newer build: keep the default.
        return in.read(value.level);
    }
};
static_assert(state::Model<State>);
static_assert(state::byte_comparable<State>);

} // namespace tiny::models
