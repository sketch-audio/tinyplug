#pragma once

#include "tinyplug/tinyplug.h"
#include "models/param_model.h"

namespace tiny {

class Plug_editor {
public:

    static auto preferred_size() -> Rect_size { return {200, 200}; }

    Plug_editor(Task_manager::Actor) {}
    ~Plug_editor() = default;

    auto on_gui_create() -> void;
    auto on_gui_show(const Edit_context&) -> void;
    auto on_gui_notify(const Ui_notification&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

    // Optional opt-in.
    auto bind_worker(Worker_editor_actor a) -> void { _worker = a; }

    auto on_worker_reply(const Plug_worker::To_editor& r) -> void
    {
        std::visit([this](const auto& a) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(a)>, Session_path>) {
                _last_path = a.path;
            }
        }, r);
    }

private:

    using User_params = Param_infos<Param_model>;
    using Param_address = Param_model::Param_address;

    Edit_context _edit{};
    bool _dark{};

    Worker_editor_actor _worker{};
    std::array<char, 128> _last_path{};

};

} // namespace tiny
