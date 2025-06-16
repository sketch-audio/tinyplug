#pragma once

#include <memory>

#include "public.sdk/source/common/pluginview.h"

#include "platform_view.h"

namespace tiny {

class Vst3_view : public Steinberg::CPluginView {
public:

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override
    {
        if (strcmp(type, Steinberg::kPlatformTypeNSView) == 0)
            return Steinberg::kResultTrue;

        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override
    {
        platform_view = CreatePlatformView(_delegate.get());
        AttachPlatformView(parent, platform_view);
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API removed() override
    {
        DestroyPlatformView(platform_view);
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onWheel(float distance) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override
    {
        const auto delegate_size = _delegate->getSize();
        *size = {0, 0, delegate_size.width, delegate_size.height};
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override
    {
        _delegate->onResize({newSize->getWidth(), newSize->getHeight()});
        RedrawPlatformView(platform_view, _delegate.get());
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) override
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API canResize() override
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override
    {
        return Steinberg::kResultTrue;
    }

    DELEGATE_REFCOUNT(Steinberg::CPluginView)

protected:

    std::unique_ptr<Graphics_delegate> _delegate = std::make_unique<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    void* platform_view{nullptr};

};

}

