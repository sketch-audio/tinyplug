#pragma once

#include <array>
#include <memory>

#include "AAX_CEffectGUI.h"
#include "AAX_VController.h"

#include "platform/platform_view.h"
#include "plug_editor.h"

#include "aax_adapters.h"
#include "aax_parameters.h"

namespace tiny {

class Aax_gui : public AAX_CEffectGUI {
public:

    static AAX_IEffectGUI* AAX_CALLBACK Create() { return new Aax_gui; }

protected:

    void CreateViewContents() override;
    void CreateViewContainer() override;
    void DeleteViewContainer() override;

    AAX_Result GetViewSize(AAX_Point* view_size) const override;
    AAX_Result ParameterUpdated(AAX_CParamID inParamID) override;

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Ui_notification& notification) -> void;

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    Action_queue _actions{};
    Undo_history _undo_history{};

    Plug_editor* _editor{};
    Ui_receiver _receiver{};
    Task_manager* _tasks{};
    Aax_parameters* _params{};

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
                .editor_state = _editor ? _editor->save_state() : State_map{}
            };
        },
    }};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};
    std::array<Tagged_meter, num_meters> _ui_meters{};

    std::unordered_set<uint32_t> _gestured{};

};

} // namespace tiny