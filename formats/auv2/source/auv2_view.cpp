#include "auv2_view.h"

namespace tiny {

auto Auv2_view::create_view() -> void*
{
    auto delegate = std::make_shared<View_delegate>(
        Plug_editor::preferred_size(),
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    auto on_autorelease = [this]() {
        _editor->on_gui_hide();
        _platform_view->on_hide();
        _editor->on_gui_destroy();
        _platform_view->on_destroy();
    };
    _platform_view = Platform_views::make_autoreleasing(delegate, on_autorelease);

    _platform_view->on_create();
    _editor->on_gui_create();

    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_knob_value(static_cast<uint32_t>(i)); }
    );

    _platform_view->on_show();
    _editor->on_gui_show({
        .actions = _actions.make_receiver(),
        .tasks = _tasks.make_receiver(),
        .undo_redo = _undo_history.make_receiver(),
    });

    return _platform_view->native_handle();
}

auto Auv2_view::on_draw(View_context& view_context) -> void
{
    _executor.on_main();
    view_impl::run_frame(
        _meter_infos, _receiver, _ui_params, _ui_meters, view_context, _editor.get(), _actions, _tasks, _undo_history
    );
}

auto Auv2_view::on_notify(const Ui_notification& notification) -> void
{
    _editor->on_gui_notify(notification);
}

} // namespace tiny