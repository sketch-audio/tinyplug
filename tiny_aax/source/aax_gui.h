#pragma once

#include <memory>

#include "AAX_CEffectGUI.h"
#include "AAX_VController.h"

#include "platform/platform_view.h"
#include "user/custom_view.h"

#include "aax_adapters.h"
#include "aax_parameters.h"

class Aax_gui : public AAX_CEffectGUI {
public:

    static AAX_IEffectGUI* AAX_CALLBACK Create() { return new Aax_gui; }

protected:

    void CreateViewContents() override {}

    void CreateViewContainer() override
    {
        using namespace tiny;

        if (auto* parent = GetViewContainerPtr()) {
            platform_view = Platform_views::make_owning(_delegate);
            platform_view->receive_parent(parent);

            // Create our event receiver.
            _receiver = [this]() -> Ui_receiver {
                if (auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters())) {
                    return {[params](auto& e) -> bool { return params ? params->pop_export(e) : false; }};
                }
                else {
                    return {};
                }
            }();
        }
    }

    void DeleteViewContainer() override
    {
        platform_view = nullptr;
    }

    AAX_Result GetViewSize(AAX_Point* view_size) const override
    {
        auto size = _delegate->getSize();
        view_size->horz = size.width;
        view_size->vert = size.height;
        return AAX_SUCCESS;
    }

    AAX_Result ParameterUpdated(AAX_CParamID inParamID) override
    {
        if (const auto tiny_id = tiny::aax_id_to_tiny(inParamID)) {
            auto* aax_parameters = GetEffectParameters();
            AAX_IParameter* param = nullptr;
            if (aax_parameters->GetParameter(inParamID, &param) == AAX_SUCCESS) {
                const auto value = param->GetNormalizedValue();
                _uivalues[*tiny_id] = value;
            }
        }
        return AAX_SUCCESS;
    }

private:

    using Draw_context = Graphics_delegate::Draw_context;
    auto on_draw(Draw_context& context) -> void
    {
        using namespace tiny;
        // Resolve application state

        // Pop the exports.
        auto event = Ui_event{};
        while (_receiver.pop_event(event)) {
            std::visit(
                Inline_visitor{
                    [&](const Set_param&) { /* use _values directly. */},
                    [&](const Set_export& e) {
                        auto& ui_export = _exports[e.id];
                        if (!ui_export.updated) {
                            ui_export.value = 0; // Reset on first update in frame where we receive an event.
                        }
                        ui_export.value = std::max(ui_export.value, e.value);
                        ui_export.updated = true;
                    }
                }
            , event);
        }

        // Adapt to values.
        auto export_arr = std::array<double, User_exports::num_exports>{};
        std::ranges::copy(_exports | std::views::transform(&Ui_export::value), export_arr.begin());

        // Create view context.
        auto view_context = View_context{
            .params_state = {
                .params = _uivalues,
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

    using User_params = tiny::Param_infos<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    User_params _params{};

    tiny::Ui_receiver _receiver{}; // A function to pop exports

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(
        Graphics_delegate::Size{800, 600}, // Initial size
        [this](auto& context) { this->on_draw(context); }
    );
    std::unique_ptr<Platform_view> platform_view{nullptr};

    using User_view = tiny::Custom_view;
    std::unique_ptr<User_view> _view = std::make_unique<User_view>();

    struct Ui_export {
        double value{};
        bool updated{};
    };
    std::array<Ui_export, num_exports> _exports{};
    std::array<double, num_params> _uivalues{_params.make_knob_defaults()};
    
};