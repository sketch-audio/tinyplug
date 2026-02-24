#include "auv3_view.h"

namespace tiny {

auto Auv3_view::create_view() -> void*
{
    auto delegate = std::make_shared<View_delegate>(
        Plug_editor::preferred_size(),
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); } 
    );
    _platform_view = Platform_views::make_owning(delegate); // TODO: - revisit

    _platform_view->on_create();
    _deps.editor->on_gui_create();

    return _platform_view->native_handle();
}

// MARK: - private

auto Auv3_view::on_draw(View_context& view_context) -> void
{
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_param(static_cast<uint32_t>(i)); }
    );
    view_impl::run_frame(
        User_meters::meter_specs(),
        _deps.receiver,
        _ui_params,
        _ui_meters,
        view_context,
        _deps.editor,
        _actions,
        _undo_history,
        *_deps.tasks,
        [](auto, auto) {}
    );
}

auto Auv3_view::on_notify(const Ui_notification& notification) -> void
{
    _editor->on_gui_notify(notification);
}

} // namespace tiny
