#include "auv2_view.h"

namespace tiny {

void* Auv2_view::create_view()
{
    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); }
    );
    _platform_view = Platform_views::make_autoreleasing(delegate);

    for (auto i = uint32_t{}; i < num_params; ++i) {
        _uiparams[i] = _receiver.get_knob_value(i);
    }

    return _platform_view->native_handle();
}

void Auv2_view::on_draw(View_context& view_context)
{
    // Pop the exports.
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                _uiparams[p.id] = p.value;
            },
            [&](const Set_export& e) {
                auto& ui_export = _uiexports[e.id];
                if (!ui_export.updated) {
                    ui_export.value = 0; // Reset on first update in frame where we receive an event.
                }
                ui_export.value = std::max(ui_export.value, e.value);
                ui_export.updated = true;
            }
        }, event);
    }

    // Adapt to values.
    auto export_arr = std::array<double, num_exports>{};
    const auto value_tx = _uiexports | std::views::transform(&Ui_export::value);
    std::ranges::copy(value_tx, export_arr.begin());

    // Create view context.
    auto app_state = App_state{
        .params_state = {
            .params = _uiparams,
            .exports = export_arr 
        },
        .action_receiver = {},
        .view_context = view_context
    };

    // Tell the user view to draw.
    _custom_view->on_draw(app_state);

    const auto& actions = app_state.action_receiver.actions();
    for (auto& action : actions) {
        _receiver.action_handler(action);
        std::visit(Inline_visitor{
            [&](const Set_param& s) { _uiparams[s.id] = s.value; },
            [](const auto&) {}
        }, action);
    }

    // Get ready for next frame.
    for (auto& ui_export : _uiexports) {
        ui_export.updated = false;
    }
}

} // namespace tiny