#pragma once

#include <concepts>
#include <functional>
#include <limits>
#include <variant>

namespace tiny {

struct Set_param {
    uint32_t id{};
    double value{};
};

struct Ramp_param {
    uint32_t id{};
    double target{};
    int32_t dur_samples{};
};

using Render_event = std::variant<Set_param, Ramp_param>;

struct Tagged_event {
    Render_event event{};
    int32_t offset{std::numeric_limits<decltype(offset)>::max()}; // Frame offset in current buffer.
};

// MARK: - UI events

struct Set_export {
    uint32_t id{};
    double value{};
};

using Ui_event = std::variant<Set_param, Set_export>;

struct Action_start { uint32_t id{}; };
struct Action_end { uint32_t id{}; };

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