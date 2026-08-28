#include "view.hpp"

namespace tiny::auv3 {

auto View::create_view() -> void*
{
    auto delegate = std::make_shared<View_delegate>(
        plugin::Editor::preferred_size(),
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); } 
    );
    _platform_view = Platform_views::make_owning(delegate); // TODO: - revisit

    _platform_view->on_create();
    _deps.editor->on_gui_create(Gui_info{.window = _platform_view->token()});

    return _platform_view->native_handle();
}

// MARK: - private

auto View::on_draw(View_context& view_context) -> void
{
    using namespace params;
    
#if TINY_HAS_WORKER
    if (_deps.drain_worker_to_editor) _deps.drain_worker_to_editor();
#endif

    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_param(static_cast<uint32_t>(i)); }
    );
    view_impl::run_frame(
        User_meters::specs(),
        _deps.receiver,
        _ui_params,
        _ui_meters,
        view_context,
        _deps.editor,
        *_deps.actions,
        *_deps.undo_history,
        *_deps.tasks,
        [](auto, auto) {}
    );
}

auto View::on_notify(const Dark_mode_changed& notification) -> void
{
    _deps.editor->notify(notification);
}

} // namespace tiny::auv3
