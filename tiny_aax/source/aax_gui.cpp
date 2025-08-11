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
                if (const auto param = get_aax_param(params, id)) {
                    return (*param)->GetNormalizedValue();
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
        _uiparams = make_array_by_indices<double, num_params>(
            [this](auto i) { return _receiver.get_knob_value(i); }
        );
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
    view_impl::run_frame<User_exports>(
        _receiver, _uiparams, _uiexports, view_context, _custom_view.get()
    );
}

} // namespace tiny