#pragma once

#include <array>
#include <memory>

#include "AAX_CEffectGUI.h"
#include "AAX_VController.h"

#include <tiny_platform/platform_view.hpp>
#include "editor.hpp"

#include "adapters.hpp"
#include "parameters.hpp"

namespace tiny::aax {

class Gui : public AAX_CEffectGUI {
public:

    static AAX_IEffectGUI* AAX_CALLBACK Create() { return new Gui; }

protected:

    void CreateViewContents() override;
    void CreateViewContainer() override;
    void DeleteViewContainer() override;

    AAX_Result GetViewSize(AAX_Point* view_size) const override;
    AAX_Result ParameterUpdated(AAX_CParamID inParamID) override;

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Dark_mode_changed& notification) -> void;

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    plugin::Editor* _editor{};
    Ui_receiver _receiver{};
    Task_manager* _tasks{};
    Parameters* _params{};
    Action_queue* _actions{}; // Owned by Parameters (survives the Gui).

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    using enum params::Space;
    std::array<double, num_params> _ui_params{tiny::params::make_defaults<double, User_params>(Knob)};
    // Plain values now: the mailbox coalesces on the way in, so the editor no
    // longer reconstructs per-frame peak/latest/one-shot state of its own.
    std::array<double, num_meters> _ui_meters{};

    std::unordered_set<uint32_t> _gestured{};

};

} // namespace tiny::aax