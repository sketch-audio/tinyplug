#pragma once

#include "tinyplug/tinyplug.h"
#include "models/meter_model.h"
#include "models/param_model.h"

namespace tiny {

static constexpr auto key_theme_preference = "theme-preference";
static constexpr auto key_preset_name = "preset-name";

enum class Theme_preference : int32_t {
    light = 0, dark, system
};

class Plug_editor {
public:

    static auto preferred_size() -> Rect_size { return {200, 400}; }

    Plug_editor() = default;
    ~Plug_editor() = default;

    auto on_gui_create() -> void {}
    auto on_gui_show(const View_connection&) -> void;
    auto on_gui_notify(const Ui_notification&) -> void;
    auto on_gui_draw(App_state&) -> void;
    auto on_gui_hide() -> void {}
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map {
        return {
            {key_theme_preference, enum_raw(Theme_preference::system)},
            {key_preset_name, std::string{"Hello!"}}
        };
    }
    
    auto load_state(const State_map& state_map) -> void {
        auto it = state_map.end();

        it = state_map.find(key_theme_preference);
        if (it != state_map.end()) {
            const auto& val = it->second;
            const auto pref = static_cast<Theme_preference>(std::get<int32_t>(val));
            std::cout << "Loaded theme preference: " << enum_raw(pref) << "\n";
        }

        it = state_map.find(key_preset_name);
        if (it != state_map.end()) {
            const auto& val = it->second;
            const auto name = std::get<std::string>(val);
            std::cout << "Loaded preset name: " << name << "\n";
        }
    }

private:

    using User_params = Param_infos<Param_model>;
    using Param_address = Param_model::Param_address;

    User_params _params{};
    Action_queue::Receiver _actions{};
    Task_queue::Receiver _task_receiver{};

    bool _down{};
    double _ldrag{}; // norm

    std::optional<uintptr_t> _pointer{};
    bool _dark{};

};

} // namespace tiny