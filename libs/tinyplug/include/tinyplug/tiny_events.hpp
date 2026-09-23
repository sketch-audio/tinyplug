#pragma once

#include <concepts>
#include <functional>
#include <limits>
#include <span>
#include <variant>

#include <tiny_core/tiny_blocks.hpp>
#include <tiny_core/tiny_meters.hpp>
#include <tiny_core/tiny_notifications.hpp>

#include <tiny_models.hpp>

namespace tiny {

// MARK: - edit events
//
// What the editor emits and what the undo history stores. **Values here are KNOB space**
// (always 0…1). Never handed to a processor without going through `Value_helper`.

struct Action_start { uint32_t address{}; };
struct Action_end { uint32_t address{}; };
struct Request_resize { uint32_t width{}; uint32_t height{}; };

using User_action = std::variant<Action_start, Set_param, Action_end, Request_resize>;

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

struct Ui_receiver {
    using Get_param = std::function<double(uint32_t)>;
    using Action_handler = std::function<void(const User_action&)>;

#if TINY_HAS_METERS
    // Fills one display value per meter address.
    using Read_meters = std::function<void(std::span<float>)>;
#endif

#if TINY_HAS_BLOCKS
    // Refreshes the editor's retained frames, marking this draw's arrivals fresh.
    using Read_blocks = std::function<void(blocks::Frames<models::Resolved::Blocks>&)>;
#endif

    Get_param get_param = [](auto) { return 0; };
#if TINY_HAS_METERS
    Read_meters read_meters = [](auto) {};
#endif
#if TINY_HAS_BLOCKS
    Read_blocks read_blocks = [](auto&) {};
#endif
    Action_handler action_handler = [](auto&) {};
#if TINY_HAS_STATE
    // Once before and once after each draw: take in snapshots, send edits, close a waiting undo step.
    std::function<void()> sync_state = []() {};
#endif
};

} // namespace tiny
