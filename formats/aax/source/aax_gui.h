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

    User_params _param_infos{}; // infos
    User_meters _meter_infos{};
    
    Action_queue _actions{};
    Undo_history _undo_history{};

    Ui_receiver _receiver{};
    std::shared_ptr<Plug_editor> _editor{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<double, num_params> _ui_params{_param_infos.make_knob_defaults<double>()};
    std::array<Tagged_meter, num_meters> _ui_meters{};

    std::unordered_set<uint32_t> _gestured{};

};

} // namespace tiny