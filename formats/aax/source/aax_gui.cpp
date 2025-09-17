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
            Custom_view::preferred_size(), // Initial size
            [this](auto& context) { this->on_draw(context); }
        );
        _platform_view = Platform_views::make_owning(delegate);
        _platform_view->receive_parent(parent);

        auto* view = GetViewContainer();
        auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters());

        // Workaround for now, dump the latest exports into the queue so we get correct values on display.
        if (params) {
            params->dump_exports();
        }

        _receiver = {
            .get_knob_value = [params](auto id) {
                if (const auto param = get_aax_param(params, id)) {
                    return (*param)->GetNormalizedValue();
                }
                return double{};
            },
            .pop_event = [params](auto& e) -> bool {
                return params ? params->pop_export(e) : false;
            },
            .action_handler = [this, view, params](auto& action) {
                std::visit(Inline_visitor{
                    [&](const Action_start& a) {
                        _gestured.insert(a.id);
                        if (const auto aax_id = tiny_id_to_aax(a.id)) {
                            const auto* id_cstr = (*aax_id).c_str();
                            view->HandleParameterMouseDown(id_cstr, 0);
                            AAX_IParameter* param = nullptr;
                            if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS && param->Automatable()) {
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
                                if (!param->Automatable()) {
                                    params->push_action(a); // Non-automatable params are not synchronized.
                                }
                            }
                        }
                    },
                    [&](const Action_end& a) {
                        if (const auto aax_id = tiny_id_to_aax(a.id)) {
                            const auto* id_cstr = (*aax_id).c_str();
                            view->HandleParameterMouseUp(id_cstr, 0);
                            AAX_IParameter* param = nullptr;
                            if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS && param->Automatable()) {
                                param->Release();
                            };
                        }
                        _gestured.erase(a.id);
                    },
                }, action);
            }
        };

        // Now we have the receiver.
        _uiparams = make_array_by_indices<double, num_params>(
            [this](auto i) { return _receiver.get_knob_value(static_cast<uint32_t>(i)); }
        );

        _custom_view->on_create(_actions.make_receiver(), _tasks.make_receiver());
    }
}

void Aax_gui::DeleteViewContainer()
{
    _platform_view->teardown(); // Force stop display link.
    _platform_view = nullptr;
}

AAX_Result Aax_gui::GetViewSize(AAX_Point* view_size) const
{
    const auto size = _platform_view ? _platform_view->get_size() : Custom_view::preferred_size();
    view_size->horz = static_cast<float>(size.w);
    view_size->vert = static_cast<float>(size.h);
    return AAX_SUCCESS;
}

AAX_Result Aax_gui::ParameterUpdated(AAX_CParamID inParamID)
{
    if (const auto tiny_id = aax_id_to_tiny(inParamID)) {
        // Ignore updates for gestured params.
        if (_gestured.find(*tiny_id) != _gestured.end()) { return AAX_SUCCESS; }

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
        _receiver, _uiparams, _uiexports, view_context, _custom_view.get(), _actions, _tasks
    );
}

} // namespace tiny