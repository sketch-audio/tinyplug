#include "view.hpp"

#include "controller.hpp"

namespace tiny::vst3 {

Steinberg::tresult PLUGIN_API View::isPlatformTypeSupported(Steinberg::FIDString type)
{
#if TINY_PLATFORM_MACOS
    const auto platform_type = Steinberg::kPlatformTypeNSView;
#elif TINY_PLATFORM_IOS
    const auto platform_type = Steinberg::kPlatformTypeUIView;
#elif TINY_PLATFORM_WINDOWS
    const auto platform_type = Steinberg::kPlatformTypeHWND;
#endif

    if (strcmp(type, platform_type) == 0)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API View::attached(void* parent, Steinberg::FIDString /*type*/)
{
    if (!parent) return Steinberg::kResultFalse;

    if (!_deps.controller || !_deps.editor) return Steinberg::kResultFalse;

    auto& store = _deps.controller->edit_store();
    auto delegate = std::make_shared<View_delegate>(
        store.size(),
        [this](auto& context) { this->on_draw(context); }
    );
    store.set_apply([this](Rect_size size) { _request_size(size.w, size.h); });

    // Create the platform view and notify the editor.
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();
    _deps.editor->on_gui_create(Gui_info{.window = _platform_view->token()});

    _platform_view->receive_parent(parent);

    // Synchronize on display.
    _ui_params = params::make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_param(static_cast<uint32_t>(i)); }
    );

    _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _deps.editor->on_gui_show();

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::removed()
{
    _deps.controller->edit_store().set_apply({});
    _deps.editor->on_gui_hide();
    view_impl::close_session(_deps.receiver, _ui_params, *_deps.actions, *_deps.undo_history);
    _platform_view->on_hide();

    _deps.editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::getSize(Steinberg::ViewRect* size)
{
    const auto platform_size = _platform_view ? _platform_view->get_size() : _deps.controller->edit_store().size();
    *size = {0, 0, platform_size.w, platform_size.h};
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::onSize(Steinberg::ViewRect* newSize)
{
    if (!newSize) return Steinberg::kResultFalse;

    auto& store = _deps.controller->edit_store();
    store.resized({newSize->getWidth(), newSize->getHeight()}); // The size the session keeps.
    const auto size = store.size();

    if (!_platform_view) return Steinberg::kResultTrue;
    _platform_view->resize(size.w, size.h);

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::onFocus(Steinberg::TBool /*state*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API View::setFrame(Steinberg::IPlugFrame* frame)
{
    if (!frame) return Steinberg::kResultFalse;
    Super::setFrame(frame);
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::canResize()
{
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::checkSizeConstraint(Steinberg::ViewRect* rect)
{
    if (!rect) return Steinberg::kInvalidArgument;
    const auto size = _deps.controller->edit_store().sizing().clamp({rect->getWidth(), rect->getHeight()});
    rect->right = rect->left + size.w;
    rect->bottom = rect->top + size.h;
    return Steinberg::kResultTrue;
}

// MARK: - private

void View::on_draw(View_context& view_context)
{
#if TINY_HAS_WORKER
    if (_deps.drain_worker_to_editor) _deps.drain_worker_to_editor();
#endif

    if (const auto controller = _deps.controller) {
        controller->consume_changes([this](auto addr, auto value) {
            if (addr < num_params) _ui_params[addr] = value;
        });
    }
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

auto View::_request_size(int32_t w, int32_t h) -> void
{
    if (!plugFrame) return;
    auto rect = Steinberg::ViewRect{0, 0, w, h};
    plugFrame->resizeView(this, &rect);
}

} // namespace tiny::vst3
