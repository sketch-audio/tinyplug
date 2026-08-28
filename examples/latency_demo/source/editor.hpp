#pragma once

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

class Editor {
public:

    static auto preferred_size() -> Rect_size { return {400, 400}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void;
    auto on_gui_show() -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

    auto notify(const Host_event&) -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_params = params::Infos<models::Params>;
    using Address = models::Params::Address;

    Frame _frame{};
    double _value{};
    std::unique_ptr<Gesture_recognizer> _click{std::make_unique<Click_recognizer>(Gesture_callbacks<Click_info>{
        .on_started = [=, this](const Click_info&) {
            const auto addr = enum_raw(Address::Latency_mode);
            const auto next = (_value == 0) ? 1. : 0.;
            _edit.actions.push(Action_start{addr});
            _edit.actions.push(Set_param{addr, next});
            _edit.actions.push(Action_end{addr});
        },
        .on_updated = [](const Click_info&) {},
        .on_ended = [](const Click_info&) {},
        .on_cancelled = []() {}
    }, Click_recognizer::Desc{/* single, left click */})};

    Edit_context _edit{};

};

} // namespace tiny::plugin