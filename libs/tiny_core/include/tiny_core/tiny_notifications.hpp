#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <variant>

namespace tiny {

// MARK: - edit values

// A parameter value as the editor and undo history see it. **KNOB space** (always 0…1).
// Never handed to a processor without going through `Value_helper`.
struct Set_param {
    uint32_t address{};
    double value{}; // Knob space.
};

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

} // namespace tiny
