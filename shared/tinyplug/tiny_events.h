#pragma once

#include <concepts>
#include <functional>
#include <limits>
#include <variant>

namespace tiny {

struct Set_param {
    uint32_t address{};
    double value{};
};

struct Ramp_param {
    uint32_t address{};
    double target{};
    int32_t dur_samples{};
};

// The host has accepted your latency proposal.
// You should immediately apply the new latency.
struct Accepted_latency {
    uint32_t samples{};
};

using Render_event = std::variant<Set_param, Ramp_param, Accepted_latency>;

struct Tagged_event {
    Render_event event{};
    int32_t offset{std::numeric_limits<decltype(offset)>::max()}; // Frame offset in current buffer.
};

// MARK: - UI events

struct Set_meter {
    uint32_t address{};
    double value{};
};

using Ui_event = std::variant<Set_param, Set_meter>;

struct Action_start { uint32_t address{}; };
struct Action_end { uint32_t address{}; };

using User_action = std::variant<Action_start, Set_param, Action_end>;

struct Ui_receiver {
    using Get_value = std::function<double(uint32_t)>;
    using Pop_event = std::function<bool(Ui_event&)>;
    using Action_handler = std::function<void(const User_action&)>;
    Get_value get_knob_value = [](auto) { return 0; };
    Pop_event pop_event = [](auto&) { return false; };
    Action_handler action_handler = [](auto&) {};
};

} // namespace tiny