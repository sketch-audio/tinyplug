#pragma once

#include <memory>

#include "tinyplug/tinyplug.hpp"
#include "platform/platform_view.hpp"

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

namespace tiny::auv3 {

class View {
public:
    
    struct Deps {
        plugin::Editor* editor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
#if TINY_HAS_WORKER
        std::function<void()> drain_worker_to_editor{};
#endif
    };

    View(const Deps& deps) : _deps{deps} {}

    auto create_view() -> void*; // UIView*

    auto on_show() -> void
    {
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

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;

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

    std::array<view::Meter_state, num_meters> _ui_meters{};

    using enum params::Space;
    std::array<double, num_params> _ui_params{tiny::params::make_defaults<double, User_params>(Knob)};

};

} // namespace tiny::auv3
