#pragma once

#include <memory>

#include "public.sdk/source/common/pluginview.h"

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "user/param_model.h"
#include "user/custom_view.h"

class Vst3_view : public Steinberg::CPluginView {
public:

    using Super = Steinberg::CPluginView;
    Vst3_view(tiny::Pop_export pop_export) : Super{}, _pop_export{pop_export} {}

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
        platform_view = Platform_views::make_owning(_delegate);
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

    using Draw_context = Graphics_delegate::Draw_context;
    auto on_draw(Draw_context& context) -> void
    {
        using namespace tiny;
        // Resolve application state

        // Pop the exports.
        auto event = Export_event{};
        while (_pop_export(event)) {
            auto& ui_export = _exports[event.id];
            if (!ui_export.updated) {
                ui_export.value = 0; // Reset on first update in frame where we receive an event.
            }
            ui_export.value = std::max(ui_export.value, event.value);
            ui_export.updated = true;
        }

        // Adapt to values.
        auto export_arr = std::array<double, User_exports::num_exports>{};
        std::ranges::copy(_exports | std::views::transform(&Ui_export::value), export_arr.begin());

        // Create view context.
        auto view_context = View_context{
            .params_state = {
                .params = {},
                .exports = export_arr 
            },
            .draw_context = context
        };

        // Tell the user view to draw.
        _view->on_draw(view_context);

        // Get ready for next frame.
        for (auto& ui_export : _exports) {
            ui_export.updated = false;
        }
    }

    using User_params = tiny::Params<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    tiny::Pop_export _pop_export{}; // A function to pop exports

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(
        Graphics_delegate::Size{800, 600},
        [this](auto& context) { this->on_draw(context); }
    );
    std::unique_ptr<Platform_view> platform_view{nullptr};

    using User_view = tiny::Custom_view;
    std::unique_ptr<User_view> _view = std::make_unique<User_view>();

    struct Ui_export {
        double value{};
        bool updated{};
    };
    std::array<Ui_export, User_exports::num_exports> _exports{};

};
