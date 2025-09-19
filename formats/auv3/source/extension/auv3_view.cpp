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
    _editor->on_gui_create();

    return _platform_view->native_handle();
}

// MARK: - private

auto Auv3_view::on_draw(View_context& view_context) -> void
{
    view_impl::run_frame<User_exports>(
        _receiver, _uiparams, _uiexports, view_context, _editor.get(), _actions, _tasks
    );
}

auto Auv3_view::on_notify(const Ui_notification& notification) -> void
{
    _editor->on_gui_notify(notification);
}

} // namespace tiny
