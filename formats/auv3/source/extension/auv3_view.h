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

    Auv3_view(Ui_receiver receiver, std::shared_ptr<Plug_editor> editor) : _receiver{receiver}, _editor{editor} {}

    auto create_view() -> void*; // UIView*

    auto on_show() -> void
    {
        // Update the ui params
        _ui_params = make_array_by_indices<double, num_params>(
            [this](auto i) { return _receiver.get_knob_value(static_cast<uint32_t>(i)); }
        );

        _platform_view->on_show();
        _editor->on_gui_show({
            .actions = _actions.actor(),
            .undo_redo = _undo_history.actor(),
        });
    }

    auto on_hide() -> void
    {
        _editor->on_gui_hide();
        _platform_view->on_hide();
    }

    auto on_destroy() -> void
    {
        _editor->on_gui_destroy();
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

    User_meters _meter_infos{};
    
    Action_queue _actions{};
    Undo_history _undo_history{};
    Ui_receiver _receiver{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};
    std::shared_ptr<Plug_editor> _editor{nullptr};

    std::array<Tagged_meter, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};

};

} // namespace tiny
