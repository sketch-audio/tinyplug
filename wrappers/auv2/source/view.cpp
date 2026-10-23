#include "view.hpp"

namespace tiny::auv2 {

auto View::create_view() -> void*
{
    auto delegate = std::make_shared<View_delegate>(
        _deps.edit_store->size(),
        [this](auto& context) { this->on_draw(context); }
    );
    _deps.edit_store->set_apply([this](Rect_size size) { resize(size); });

    auto on_autorelease = [this]() {
        _deps.editor->on_gui_hide();
        view_impl::close_session(_deps.receiver, _ui_params, *_deps.actions, *_deps.undo_history);
        _deps.edit_store->set_apply({});
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

// A host can dispose of the unit with its view still on screen, so the view can outlive us. Close the
// editor's GUI lifecycle as the view's dealloc would have; ~Platform_view then leaves the view inert.
View::~View()
{
    _deps.edit_store->set_apply({});
    if (_platform_view && _platform_view->native_handle()) {
        _deps.editor->on_gui_hide();
        _deps.editor->on_gui_destroy();
    }
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
        _deps.receiver,
        _ui_params,
        _ui_meters,
        _ui_blocks,
        view_context,
        _deps.editor,
        *_deps.actions,
        *_deps.undo_history,
        *_deps.tasks
    );
}

auto View::resize(Rect_size size) -> void
{
    if (!_platform_view) return;
    _platform_view->resize(size.w, size.h);
    _deps.edit_store->resized(size); // No host echo on AUv2: this is the size now.
}

} // namespace tiny::auv2