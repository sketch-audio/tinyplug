#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include <tiny_core/tiny_blocks.hpp>

#include <tiny_models.hpp>

namespace tiny::edit {

// What the parameter handle queues and a frame resolves to. **Values are KNOB space** (always 0…1), never handed
// to a processor without `params::knob_to_plain`: `process::Event::Set` is the plain-space twin, a different type on purpose.
struct Action {
    // A gesture on one parameter, or with no address a transaction: everything until the matching end is one undo
    // step, and each parameter it sets is touched for the host just before its first set. Wrappers only ever see
    // addressed starts and ends (`Action_queue::normalize`).
    struct Start { std::optional<std::uint32_t> address{}; };
    struct Set {
        double value{}; // Knob space.
        std::uint32_t address{};
    };
    struct End { std::optional<std::uint32_t> address{}; };

    using Any = std::variant<Start, Set, End>;
};

} // namespace tiny::edit

namespace tiny {

// What the editor reads from the processor side each draw.
struct Processor_state {
    std::span<const double> params{};
#if TINY_HAS_METERS
    std::span<const double> meters{};
#endif
#if TINY_HAS_BLOCKS
    blocks::View<models::Resolved::Blocks> blocks{};
#endif
};

} // namespace tiny
