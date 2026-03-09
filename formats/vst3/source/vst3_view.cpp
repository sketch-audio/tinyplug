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
    if (!parent) return Steinberg::kResultFalse;

    if (!_deps.controller || !_deps.editor) return Steinberg::kResultFalse;

    const auto initial_size = _deps.controller->get_last_size()
        .value_or(Plug_editor::preferred_size());

    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    // Create the platform view and notify the editor.
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();
    _deps.editor->on_gui_create();
    
    _platform_view->receive_parent(parent);

    // Synchronize on display.
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _deps.receiver.get_param(static_cast<uint32_t>(i)); }
    );

    _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _deps.editor->on_gui_show({
        .actions = _actions.actor(),
        .format = Format::Vst3,
        .state_adapter = _state_adapter.actor(),
        .undo_redo = _undo_history.actor(),
    });
    
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::removed()
{
    _deps.editor->on_gui_hide();
    _platform_view->on_hide();

    _deps.editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::getSize(Steinberg::ViewRect* size)
{
    const auto initial_size = _deps.controller->get_last_size()
        .value_or(Plug_editor::preferred_size());
    
    const auto platform_size = _platform_view ? _platform_view->get_size() : initial_size;
    *size = {0, 0, platform_size.w, platform_size.h};

    _deps.controller->resized({platform_size.w, platform_size.h}); // Remember for next time.

    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onSize(Steinberg::ViewRect* newSize)
{
    if (!newSize) return Steinberg::kResultFalse;

    const auto w = newSize->getWidth();
    const auto h = newSize->getHeight();
    _deps.controller->resized({w, h}); // Remember for next time.

    if (!_platform_view) return Steinberg::kResultTrue;
    _platform_view->resize(w, h);
    
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onFocus(Steinberg::TBool /*state*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::setFrame(Steinberg::IPlugFrame* frame)
{
    if (!frame) return Steinberg::kResultFalse;
    Super::setFrame(frame);
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
    if (const auto controller = _deps.controller) {
        controller->consume_changes([this](auto addr, auto value) {
            if (addr < num_params) _ui_params[addr] = value;
        });
    }
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

auto Vst3_view::on_notify(const Ui_notification& notification) -> void
{
    _deps.editor->on_gui_notify(notification);
}

} // namespace tiny