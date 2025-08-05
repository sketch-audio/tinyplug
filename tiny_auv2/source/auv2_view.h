#pragma once

#include <memory>

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "user/param_model.h"
#include "user/custom_view.h"

namespace tiny {

struct Auv2_view {

    Auv2_view(Ui_receiver receiver) : _receiver{receiver} {}

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
        auto event = Ui_event{};
        while (_receiver.pop_event(event)) {
            std::visit(
                Inline_visitor{
                    [&](const Set_param& p) {
                        _uivalues[p.id] = p.value;
                    },
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

    Ui_receiver _receiver{};

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

} // namespace tiny