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
        _deps.editor->on_gui_hide();
        _platform_view->on_hide();
        _deps.editor->on_gui_destroy();
        _platform_view->on_destroy();
    };
    _platform_view = Platform_views::make_autoreleasing(delegate, on_autorelease);

    _platform_view->on_create();
    _deps.editor->on_gui_create();

    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_knob_value(static_cast<uint32_t>(i)); }
    );

    _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _deps.editor->on_gui_show({
        .actions = _actions.actor(),
        .format = Format::Auv2,
        .state_adapter = _state_adapter.actor(),
        .undo_redo = _undo_history.actor(),
    });

    return _platform_view->native_handle();
}

auto Auv2_view::on_draw(View_context& view_context) -> void
{
    _deps.executor.on_main();
    view_impl::run_frame(
        User_meters::meter_specs(), _deps.receiver, _ui_params, _ui_meters, view_context, _deps.editor, _actions, _undo_history, *_deps.tasks
    );
}

auto Auv2_view::on_notify(const Ui_notification& notification) -> void
{
    _deps.editor->on_gui_notify(notification);
}

} // namespace tiny