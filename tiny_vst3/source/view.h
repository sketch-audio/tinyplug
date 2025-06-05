#pragma once

#include "public.sdk/source/common/pluginview.h"

#include "platform_view.h"

namespace tiny {

class Vst3_view : public Steinberg::CPluginView {
public:

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type)
    {
        if (strcmp(type, Steinberg::kPlatformTypeNSView) == 0)
            return Steinberg::kResultTrue;

        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type)
    {
        platform_view = CreatePlatformView(800, 600);
        AttachPlatformView(parent, platform_view);
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API removed()
    {
        DestroyPlatformView(platform_view);
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onWheel(float distance)
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers)
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers)
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size)
    {
        *size = {0, 0, 800, 600};
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize)
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state)
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame)
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API canResize()
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect)
    {
        return Steinberg::kResultFalse;
    }

    DELEGATE_REFCOUNT(Steinberg::CPluginView)

protected:

    void* platform_view{nullptr};

};

}

