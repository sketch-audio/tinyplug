#pragma once

#include <array>
#include <memory>

#include "AAX_CEffectGUI.h"
#include "AAX_VController.h"

#include "platform/platform_view.h"
#include "custom_view.h"

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

    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    User_params _param_infos{}; // infos
    Action_queue _actions{};
    Task_queue _tasks{};
    Ui_receiver _receiver{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};
    std::unique_ptr<Custom_view> _custom_view = std::make_unique<Custom_view>();

    std::array<double, num_params> _uiparams{_param_infos.make_knob_defaults<double>()};
    std::array<Tagged_export, num_exports> _uiexports{};

    std::unordered_set<uint32_t> _gestured{};

};

} // namespace tiny