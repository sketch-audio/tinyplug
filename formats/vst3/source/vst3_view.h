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

class Vst3_controller;

class Vst3_view : public Steinberg::CPluginView {
public:

    struct Deps {
        Vst3_controller* controller{}; // So we can cache the resized size.
        Plug_editor* editor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
    };

    using Super = Steinberg::CPluginView;
    Vst3_view(const Deps& deps) : Super{}, _deps{deps} {}
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

    Action_queue _actions{};
    Undo_history _undo_history{};

    Deps _deps{};

    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        },
        .save_model = [this]() {
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(_ui_params.begin(), _ui_params.end()),
                .editor_state = _deps.editor ? _deps.editor->save_state() : State_map{}
            };
        },
    }};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<Tagged_meter, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};

};

} // namespace tiny