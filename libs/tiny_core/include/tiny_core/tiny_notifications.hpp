#pragma once

#include <cstdint>

namespace tiny {

// MARK: - Appearance

// The host's or system's appearance changed, from the platform view to its delegate, which carries it
// into each frame's `View_context::dark`.
struct Dark_mode_changed {
    bool new_value{};
};

} // namespace tiny
