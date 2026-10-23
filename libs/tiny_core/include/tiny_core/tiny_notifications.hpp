#pragma once

#include <cstdint>

namespace tiny {

// MARK: - edit values

// A parameter value as the editor and undo history see it. **KNOB space** (always 0…1).
// Never handed to a processor without going through `Value_helper`.
struct Set_param {
    uint32_t address{};
    double value{}; // Knob space.
};

// MARK: - Appearance

// The host's or system's appearance changed, from the platform view to its delegate, which carries it
// into each frame's `View_context::dark`.
struct Dark_mode_changed {
    bool new_value{};
};

} // namespace tiny
