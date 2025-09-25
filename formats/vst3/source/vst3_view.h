#pragma once

#include <array>
#include <memory>

#include "public.sdk/source/common/pluginview.h"

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"

namespace tiny {

class Vst3_controller; // So we can cache the resized size. Would like to do this in a neater way.

class Vst3_view : public Steinberg::CPluginView {
public:

    using Super = Steinberg::CPluginView;
    Vst3_view(Ui_receiver receiver, std::shared_ptr<Plug_editor> editor, Vst3_controller* controller)
        : Super{}, _receiver{receiver}, _editor{editor}, _controller{controller} {}

    ~Vst3_view() = default;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
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
    auto on_notify(const Ui_notification& notification) -> void;

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    User_params _param_infos{};
    User_meters _meter_infos{};
    
    Action_queue _actions{};
    Task_queue _tasks{};

    Ui_receiver _receiver{};
    std::shared_ptr<Plug_editor> _editor{};
    Vst3_controller* _controller{nullptr};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<Tagged_meter, num_meters> _uiexports{};
    std::array<double, num_params> _uiparams{_param_infos.make_knob_defaults<double>()};

};

} // namespace tiny