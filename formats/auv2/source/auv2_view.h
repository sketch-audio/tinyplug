#pragma once

#include <memory>

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"

#include "auv2_adapters.h"

namespace tiny {

class Auv2_view {
public:

    struct Deps {
        Plug_editor* editor{};
        Main_executor executor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
    };

    Auv2_view(Deps deps) : _deps{deps} {}

    auto create_view() -> void*;

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Ui_notification& notification) -> void;

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    Action_queue _actions{};
    Undo_history _undo_history{};

    Deps _deps{};

    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        },
        .save_model = [this]() {
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(_ui_params.begin(), _ui_params.end()),
                .editor_state = _deps.editor ? _deps.editor->save_state() : State_map{}
            };
        },
    }};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<Tagged_meter, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};

};

} // namespace tiny