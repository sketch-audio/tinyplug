#include <variant>

#include "AAX_IViewContainer.h"

#include "aax_gui.h"

namespace tiny {

void Aax_gui::CreateViewContents()
{
    // pure virtual
}

void Aax_gui::CreateViewContainer()
{
    if (auto* parent = GetViewContainerPtr()) {
        auto delegate = std::make_shared<View_delegate>(
            initial_size, // Initial size
            [this](auto& context) { this->on_draw(context); }
        );
        _platform_view = Platform_views::make_owning(delegate);
        _platform_view->receive_parent(parent);

        auto* view = GetViewContainer();
        auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters());
        _receiver = {
            .get_knob_value = [params](auto id) {
                if (const auto aax_id = tiny_id_to_aax(id)) {
                    const auto* id_cstr = (*aax_id).c_str();
                    AAX_IParameter* param = nullptr;
                    if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                        return param->GetNormalizedValue();
                    };
                }
                return double{};
            },
            .pop_event = [params](auto& e) -> bool { return params ? params->pop_export(e) : false; },
            .action_handler = [view, params](auto& action) {
                std::visit(Inline_visitor{
                    [&](const Action_start& a) {
                        if (const auto aax_id = tiny_id_to_aax(a.id)) {
                            const auto* id_cstr = (*aax_id).c_str();
                            view->HandleParameterMouseDown(id_cstr, 0);
                            AAX_IParameter* param = nullptr;
                            if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                                param->Touch();
                            };
                        }
                    },
                    [&](const Set_param& a) {
                        if (const auto aax_id = tiny_id_to_aax(a.id)) {
                            const auto* id_cstr = (*aax_id).c_str();
                            view->HandleParameterMouseDrag(id_cstr, 0);
                            AAX_IParameter* param = nullptr;
                            if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                                param->SetNormalizedValue(a.value);
                            };
                        }
                    },
                    [&](const Action_end& a) {
                        if (const auto aax_id = tiny_id_to_aax(a.id)) {
                            const auto* id_cstr = (*aax_id).c_str();
                            view->HandleParameterMouseUp(id_cstr, 0);
                            AAX_IParameter* param = nullptr;
                            if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                                param->Release();
                            };
                        }
                    },
                }, action);
            }
        };

        // Now we have the receiver.
        for (auto i = uint32_t{}; i < num_params; ++i) {
            _uiparams[i] = _receiver.get_knob_value(i);
        }
    }
}

void Aax_gui::DeleteViewContainer()
{
    _platform_view = nullptr;
}

AAX_Result Aax_gui::GetViewSize(AAX_Point* view_size) const
{
    const auto size = _platform_view ? _platform_view->get_size() : initial_size;
    view_size->horz = size.w;
    view_size->vert = size.h;
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

auto Aax_gui::on_draw(View_context& view_context) -> void
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
    auto app_state = App_state{
        .params_state = {
            .params = _uiparams,
            .exports = export_arr
        },
        .action_receiver = {},
        .view_context = view_context
    };

    // Tell the user view to draw.
    _view->on_draw(app_state);

    auto& actions = app_state.action_receiver.actions();
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