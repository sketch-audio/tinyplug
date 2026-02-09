#pragma once

#include <memory>

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"

namespace tiny {

class Auv3_view {
public:
    
    struct Deps {
        Plug_editor* editor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
    };

    Auv3_view(const Deps& deps) : _deps{deps} {}

    auto create_view() -> void*; // UIView*

    auto on_show() -> void
    {
        // Update the ui params
        _ui_params = make_array_by_indices<double, num_params>(
            [this](auto i) { return _deps.receiver.get_knob_value(static_cast<uint32_t>(i)); }
        );

        _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
        _platform_view->on_show();
        _deps.editor->on_gui_show({
            .actions = _actions.actor(),
            .format = Format::Auv3,
            .state_adapter = _state_adapter.actor(),
            .undo_redo = _undo_history.actor(),
        });
    }

    auto on_hide() -> void
    {
        _deps.editor->on_gui_hide();
        _platform_view->on_hide();
    }

    auto on_destroy() -> void
    {
        _deps.editor->on_gui_destroy();
        _platform_view->on_destroy();
    }

    auto on_resize(int32_t w, int32_t h) -> void
    {
        _platform_view->resize(w, h);
    }

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
        .load_model = [this]() {
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
    std::shared_ptr<Plug_editor> _editor{nullptr};

    std::array<Tagged_meter, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};

};

} // namespace tiny
