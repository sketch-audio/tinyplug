#include "view.hpp"

namespace tiny::auv2 {

auto View::create_view() -> void*
{
    auto delegate = std::make_shared<View_delegate>(
        _deps.initial_size(),
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
    _deps.editor->on_gui_create(Gui_info{.window = _platform_view->token()});

    _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _deps.editor->on_gui_show();

    return _platform_view->native_handle();
}

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
        [this](auto w, auto h) {
            _platform_view->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
            // No host echo on AUv2 — update the size cache directly so it persists.
            if (_deps.on_resized) _deps.on_resized(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
    );
}

auto View::on_notify(const Dark_mode_changed& notification) -> void
{
    _deps.editor->notify(notification);
}

} // namespace tiny::auv2