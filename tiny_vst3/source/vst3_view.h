#pragma once

#include <array>
#include <memory>

#include "public.sdk/source/common/pluginview.h"

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "custom_view.h"
#include "param_model.h"

namespace tiny {

class Vst3_view : public Steinberg::CPluginView {
public:

    using Super = Steinberg::CPluginView;
    Vst3_view(Ui_receiver receiver) : Super{}, _receiver{receiver} {}

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API onWheel(float distance) override;
    Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 keyCode, Steinberg::int16 modifiers) override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    DELEGATE_REFCOUNT(Super)

protected:

    // This is where we resolve the app state and tell the user's custom view to draw.
    auto on_draw(View_context& view_context) -> void;
    
    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto initial_size = Rect_size{800, 600};
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    User_params _param_infos{};
    Ui_receiver _receiver{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};
    std::unique_ptr<Custom_view> _custom_view = std::make_unique<Custom_view>();

    struct Ui_export {
        double value{};
        bool updated{};
    };
    std::array<Ui_export, num_exports> _uiexports{};
    std::array<double, num_params> _uivalues{_param_infos.make_knob_defaults<double>()};

};

} // namespace tiny