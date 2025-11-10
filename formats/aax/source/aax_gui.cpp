#include <variant>

#include "AAX_IViewContainer.h"

#include "aax_gui.h"

namespace tiny {

auto Aax_gui::CreateViewContents() -> void
{
    auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters());
    _editor = params->get_editor();

    auto delegate = std::make_shared<View_delegate>(
        Plug_editor::preferred_size(), // Initial size
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();
    _editor->on_gui_create();
}

auto Aax_gui::CreateViewContainer() -> void
{
    auto* parent = GetViewContainerPtr();
    if (!parent) return;

    _platform_view->receive_parent(parent);

    auto* view = GetViewContainer();
    auto* params = dynamic_cast<Aax_parameters*>(GetEffectParameters());

    // Workaround for now, dump the latest exports into the queue so we get correct values on display.
    if (params) {
        params->dump_meters();
    }

    // We have to make our own UI connection.
    _receiver = {
        .get_knob_value = [params](auto id) {
            if (const auto aax_id = tiny_id_to_aax(id)) {
                const auto* id_cstr = (*aax_id).c_str();
                AAX_IParameter* param = nullptr;
                if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                    return param->GetNormalizedValue();
                }
            }
            return double{};
        },
        .pop_event = [params](auto& e) -> bool {
            return params ? params->pop_meter(e) : false;
        },
        .action_handler = [this, view, params](auto& action) {
            std::visit(Inline_visitor{
                [&](const Action_start& a) {
                    _gestured.insert(a.address);
                    if (const auto aax_id = tiny_id_to_aax(a.address)) {
                        const auto* id_cstr = (*aax_id).c_str();
                        view->HandleParameterMouseDown(id_cstr, 0);
                        AAX_IParameter* param = nullptr;
                        if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS && param->Automatable()) {
                            param->Touch();
                        };
                    }
                },
                [&](const Set_param& a) {
                    if (const auto aax_id = tiny_id_to_aax(a.address)) {
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
                    if (const auto aax_id = tiny_id_to_aax(a.address)) {
                        const auto* id_cstr = (*aax_id).c_str();
                        view->HandleParameterMouseUp(id_cstr, 0);
                        AAX_IParameter* param = nullptr;
                        if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS && param->Automatable()) {
                            param->Release();
                        };
                    }
                    _gestured.erase(a.address);
                },
            }, action);
        }
    };

    // Now we have the receiver.
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_knob_value(static_cast<uint32_t>(i)); }
    );

    _platform_view->on_show();
    _editor->on_gui_show({
        .actions = _actions.make_receiver(),
        .tasks = _tasks.make_receiver(),
        .undo_redo = _undo_history.make_receiver(),
    });
}

void Aax_gui::DeleteViewContainer()
{
    _editor->on_gui_hide();
    _platform_view->on_hide();

    _editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;
}

AAX_Result Aax_gui::GetViewSize(AAX_Point* view_size) const
{
    const auto size = _platform_view ? _platform_view->get_size() : Plug_editor::preferred_size();
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
            _ui_params[*tiny_id] = value;
        }
    }
    return AAX_SUCCESS;
}

// MARK: - private

auto Aax_gui::on_draw(View_context& view_context) -> void
{
    view_impl::run_frame(
        _meter_infos, _receiver, _ui_params, _ui_meters, view_context, _editor.get(), _actions, _tasks, _undo_history
    );
}

auto Aax_gui::on_notify(const Ui_notification& notification) -> void
{
    _editor->on_gui_notify(notification);
}

} // namespace tiny