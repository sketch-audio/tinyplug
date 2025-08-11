#include "vst3_view.h"

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
        }
    }();

    if (strcmp(type, platform_type) == 0)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::attached(void* parent, Steinberg::FIDString /*type*/)
{
    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); }
    );
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->receive_parent(parent);
    
    _uiparams = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_knob_value(i); }
    );
    
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::removed()
{
    _platform_view = nullptr;
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onWheel(float /*distance*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::onKeyDown(Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/, Steinberg::int16 /*modifiers*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::onKeyUp(Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/, Steinberg::int16 /*modifiers*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::getSize(Steinberg::ViewRect* size)
{
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

// MARK: - on_draw

void Vst3_view::on_draw(View_context& view_context)
{
    view_impl::run_frame<User_exports>(
        _receiver, _uiparams, _uiexports, view_context, _custom_view.get()
    );
}

} // namespace tiny