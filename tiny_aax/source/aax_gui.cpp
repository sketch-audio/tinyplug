#include "aax_gui.h"

namespace tiny {

void Aax_gui::CreateViewContents()
{
    // pure virtual
}

void Aax_gui::CreateViewContainer()
{
    if (auto* parent = GetViewContainerPtr()) {
        platform_view = Platform_views::make_owning(_delegate);
        platform_view->receive_parent(parent);

        auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters());
        _receiver = {[params](auto& e) -> bool { return params ? params->pop_export(e) : false; }};
    }
}

void Aax_gui::DeleteViewContainer()
{
    platform_view = nullptr;
}

AAX_Result Aax_gui::GetViewSize(AAX_Point* view_size) const
{
    const auto size = _delegate->getSize();
    view_size->horz = size.width;
    view_size->vert = size.height;
    return AAX_SUCCESS;
}

AAX_Result Aax_gui::ParameterUpdated(AAX_CParamID inParamID)
{
    if (const auto tiny_id = aax_id_to_tiny(inParamID)) {
        auto* aax_params = GetEffectParameters();
        AAX_IParameter* param = nullptr;
        if (aax_params->GetParameter(inParamID, &param) == AAX_SUCCESS) {
            const auto value = param->GetNormalizedValue();
            _uiparams[*tiny_id] = value;
        }
    }
    return AAX_SUCCESS;
}

// MARK: - private

auto Aax_gui::on_draw(Draw_context& context) -> void
{
    // Resolve application state

    // Pop UI events.
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param&) {/* use _uiparams directly. */},
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

    // Adapt UI exports to values.
    auto export_arr = std::array<double, num_exports>{};
    const auto value_tx = _uiexports | std::views::transform(&Ui_export::value);
    std::ranges::copy(value_tx, export_arr.begin());

    // Create view context.
    auto view_context = View_context{
        .params_state = {
            .params = _uiparams,
            .exports = export_arr
        },
        .draw_context = context
    };

    // Tell the user view to draw.
    _view->on_draw(view_context);

    // Get ready for next frame.
    for (auto& ui_export : _uiexports) {
        ui_export.updated = false;
    }
}

} // namespace tiny