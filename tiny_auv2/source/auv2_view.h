#pragma once

#include <memory>

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "user/param_model.h"
#include "user/custom_view.h"

namespace tiny::auv2 {

struct Auv2_view {

    Auv2_view(Pop_export pop_export) : _pop_export{pop_export} {}

    auto create_view() -> void*
    {
        platform_view = Platform_views::make_autoreleasing(_delegate);
        return platform_view->native_handle();
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

    using User_params = tiny::Params<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    Pop_export _pop_export{}; // A function to pop exports

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

} // namespace tiny