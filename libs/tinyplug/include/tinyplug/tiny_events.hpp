#pragma once

#include <concepts>
#include <functional>
#include <limits>
#include <span>
#include <variant>

#include <tiny_core/meter_mailbox.hpp>
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

struct Ui_receiver {
    using Get_param = std::function<double(uint32_t)>;
    using Action_handler = std::function<void(const User_action&)>;

#if TINY_HAS_METERS
    // Fill one sample per meter address from the mailbox. Replaces the old
    // pop-until-empty drain: a slot array has nothing to run dry, so a reader that
    // skipped a thousand blocks gets the same answer shape as one that skipped none.
    // That is also why there is no resync hook — the mailbox is always current, so a
    // newly attached editor simply reads it.
    using Read_meters = std::function<void(std::span<meters::Sample>)>;
#endif

    Get_param get_param = [](auto) { return 0; };
#if TINY_HAS_METERS
    Read_meters read_meters = [](auto) {};
#endif
    Action_handler action_handler = [](auto&) {};
};

} // namespace tiny
