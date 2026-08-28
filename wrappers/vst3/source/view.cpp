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

    const auto initial_size = _deps.controller->get_last_size()
        .value_or(plugin::Editor::preferred_size());

    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

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
    _deps.editor->on_gui_hide();
    _platform_view->on_hide();

    _deps.editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::getSize(Steinberg::ViewRect* size)
{
    const auto initial_size = _deps.controller->get_last_size()
        .value_or(plugin::Editor::preferred_size());

    const auto platform_size = _platform_view ? _platform_view->get_size() : initial_size;
    *size = {0, 0, platform_size.w, platform_size.h};

    _deps.controller->resized({platform_size.w, platform_size.h}); // Remember for next time.

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API View::onSize(Steinberg::ViewRect* newSize)
{
    if (!newSize) return Steinberg::kResultFalse;

    const auto w = newSize->getWidth();
    const auto h = newSize->getHeight();
    _deps.controller->resized({w, h}); // Remember for next time.

    if (!_platform_view) return Steinberg::kResultTrue;
    _platform_view->resize(w, h);

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

Steinberg::tresult PLUGIN_API View::checkSizeConstraint(Steinberg::ViewRect* /*rect*/)
{
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
            // Editor-initiated resize (corner-drag handle → Request_resize). Ask the
            // host to resize us; it echoes back via onSize, which resizes the platform
            // view and refreshes the controller's size cache.
            if (plugFrame) {
                auto rect = Steinberg::ViewRect{0, 0, static_cast<Steinberg::int32>(w), static_cast<Steinberg::int32>(h)};
                plugFrame->resizeView(this, &rect);
            }
        }
    );
}

auto View::on_notify(const Dark_mode_changed& notification) -> void
{
    _deps.editor->notify(notification);
}

} // namespace tiny::vst3
