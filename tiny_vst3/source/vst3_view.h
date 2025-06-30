#pragma once

#include <memory>

#include "public.sdk/source/common/pluginview.h"

#include "platform_view.h"

class Vst3_view : public Steinberg::CPluginView {
public:

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override
    {
        // Resolve the platform window type.
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

    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString /*type*/) override
    {
        platform_view = std::make_unique<Platform_view>(_delegate);
        platform_view->receive_parent(parent);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API removed() override
    {
        platform_view = nullptr;
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API onWheel(float /*distance*/) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/, Steinberg::int16 /*modifiers*/) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/, Steinberg::int16 /*modifiers*/) override
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
        if (!platform_view) return Steinberg::kResultFalse;

        const auto w = newSize->getWidth();
        const auto h = newSize->getHeight();
        _delegate->onResize({w, h});
        platform_view->resize(w, h);
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool /*state*/) override
    {
        return Steinberg::kResultFalse;
    }

    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* /*frame*/) override
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API canResize() override
    {
        return Steinberg::kResultTrue;
    }

    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* /*rect*/) override
    {
        return Steinberg::kResultTrue;
    }

    DELEGATE_REFCOUNT(Steinberg::CPluginView)

protected:

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    std::unique_ptr<Platform_view> platform_view{nullptr};

};
