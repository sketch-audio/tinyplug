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

            // Grab our exporter.
            _pop_export = [this]() -> Pop_export {
                if (auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters())) {
                    return [params](Export_event& event) -> bool { return params->pop_export(event); };
                }
                else {
                    return [](auto&) { return false; }; // no infinite loop
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

private:

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

    using User_params = tiny::Param_infos<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    tiny::Pop_export _pop_export{}; // A function to pop exports

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
    std::array<Ui_export, User_exports::num_exports> _exports{};
    
};