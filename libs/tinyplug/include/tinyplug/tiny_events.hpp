#pragma once

#include <concepts>
#include <functional>
#include <limits>
#include <span>
#include <variant>

#include "meter_mailbox.hpp"

namespace tiny {

// MARK: - edit events
//
// What the editor emits and what the undo history stores. **Values here are KNOB space**
// (always 0…1). Never handed to a processor without going through `Value_helper`.

struct Set_param {
    uint32_t address{};
    double value{}; // Knob space.
};

struct Action_start { uint32_t address{}; };
struct Action_end { uint32_t address{}; };
struct Request_resize { uint32_t width{}; uint32_t height{}; };

using User_action = std::variant<Action_start, Set_param, Action_end, Request_resize>;

// MARK: - Editor notifications

// A change in the host/OS appearance (dark vs. light). Sourced from the platform
// view while the editor window is open.
struct Dark_mode_changed {
    bool new_value{};
};

// MARK: - Host events (surfaced to the editor)

// A host-initiated preset / full-state load, delivered synchronously to the editor's
// notify() the moment the load happens — whether or not the GUI window is open.
struct Host_preset_loaded {
    // Every parameter the load altered, in knob space, as Set_param{address, value}
    // (post-load value). Valid only for the duration of the notify() call.
    std::span<const Set_param> changes{};

    // Full post-load param values in knob space, indexable by address. Valid only
    // for the duration of the notify() call.
    std::span<const double> params{};

    // Apply a param change as part of this load: sets the value through the normal
    // host/processor/UI path AND folds it into the same undo step as the preset's
    // values, so an editor-owned marker (e.g. a preset-name / index param) undoes
    // together with the preset. `knob` is knob space. Valid only during notify().
    std::function<void(uint32_t /*addr*/, double /*knob*/)> add_param{};
};

// The single event type delivered to the editor's notify(). New host-event kinds
// (e.g. host single-param edits) can slot into the variant.
using Host_event = std::variant<Host_preset_loaded, Dark_mode_changed>;

struct Ui_receiver {
    using Get_param = std::function<double(uint32_t)>;
    using Action_handler = std::function<void(const User_action&)>;

    // Fill one sample per meter address from the mailbox. Replaces the old
    // pop-until-empty drain: a slot array has nothing to run dry, so a reader that
    // skipped a thousand blocks gets the same answer shape as one that skipped none.
    // That is also why there is no resync hook — the mailbox is always current, so a
    // newly attached editor simply reads it.
    using Read_meters = std::function<void(std::span<meters::Sample>)>;

    Get_param get_param = [](auto) { return 0; };
    Read_meters read_meters = [](auto) {};
    Action_handler action_handler = [](auto&) {};
};

} // namespace tiny