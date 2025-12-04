#include "vst3_view.h"

#include "vst3_controller.h"

namespace tiny {

Steinberg::tresult PLUGIN_API Vst3_view::isPlatformTypeSupported(Steinberg::FIDString type)
{
    const auto platform_type = []() {
        switch (Platform::resolved) {
            case Platform::Type::macos:
                return Steinberg::kPlatformTypeNSView;
            case Platform::Type::ios:
                return Steinberg::kPlatformTypeUIView;
            case Platform::Type::windows:
                return Steinberg::kPlatformTypeHWND;
            default:
                return Steinberg::kPlatformTypeX11EmbedWindowID; // Not yet supported.
        }
    }();

    if (strcmp(type, platform_type) == 0)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::attached(void* parent, Steinberg::FIDString /*type*/)
{
    const auto initial_size = _controller->get_last_size()
        .value_or(Plug_editor::preferred_size());

    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    // Create the platform view and notify the editor.
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();
    _editor->on_gui_create();
    
    _platform_view->receive_parent(parent);

    // Update the ui param values with the current state.
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_knob_value(static_cast<uint32_t>(i)); }
    );

    _platform_view->on_show();
    _editor->on_gui_show({
        .actions = _actions.actor(),
        .undo_redo = _undo_history.actor(),
    });
    
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::removed()
{
    _editor->on_gui_hide();
    _platform_view->on_hide();

    _editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::getSize(Steinberg::ViewRect* size)
{
    const auto initial_size = _controller->get_last_size()
        .value_or(Plug_editor::preferred_size());
    
    const auto platform_size = _platform_view ? _platform_view->get_size() : initial_size;
    *size = {0, 0, platform_size.w, platform_size.h};
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onSize(Steinberg::ViewRect* newSize)
{
    if (!_platform_view || !newSize) return Steinberg::kResultFalse;
    const auto w = newSize->getWidth();
    const auto h = newSize->getHeight();
    _platform_view->resize(w, h);
    _controller->resized({w, h}); // Remember for next time.
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onFocus(Steinberg::TBool /*state*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::setFrame(Steinberg::IPlugFrame* /*frame*/)
{
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::canResize()
{
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::checkSizeConstraint(Steinberg::ViewRect* /*rect*/)
{
    return Steinberg::kResultTrue;
}

// MARK: - private

void Vst3_view::on_draw(View_context& view_context)
{
    view_impl::run_frame(
        _meter_infos, _receiver, _ui_params, _ui_meters, view_context, _editor.get(), _actions, _undo_history
    );
}

auto Vst3_view::on_notify(const Ui_notification& notification) -> void
{
    _editor->on_gui_notify(notification);
}

} // namespace tiny