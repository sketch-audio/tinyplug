#include <variant>

#include "AAX_IViewContainer.h"

#include "gui.hpp"

namespace tiny::aax {

auto Gui::CreateViewContents() -> void
{
    auto* params = dynamic_cast<Parameters*>(GetEffectParameters());
    _editor = params->get_editor();
    _tasks = params->get_tasks();
    _params = params;
    _actions = params->actions();

    const auto initial_size = _params->get_last_size().value_or(plugin::Editor::preferred_size());

    auto delegate = std::make_shared<View_delegate>(
        initial_size, // Primed from persisted state so the window opens pre-sized.
        [this](auto& context) { this->on_draw(context); },
        [this](const auto& notification) { this->on_notify(notification); }
    );

    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->on_create();
    _editor->on_gui_create();
}

auto Gui::CreateViewContainer() -> void
{
    using namespace params;

    auto* parent = GetViewContainerPtr();
    if (!parent) return;

    _platform_view->receive_parent(parent);

    auto* view = GetViewContainer();
    auto* params = dynamic_cast<Parameters*>(GetEffectParameters());

    // Workaround for now, dump the latest exports into the queue so we get correct values on display.
    if (params) {
        params->dump_meters();
    }

    // We have to make our own UI connection.
    _receiver = Ui_receiver{
        .get_param = [params](auto id) {
            if (const auto aax_id = tiny_id_to_aax(id)) {
                const auto* id_cstr = (*aax_id).c_str();
                AAX_IParameter* param = nullptr;
                if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                    return param->GetNormalizedValue();
                }
            }
            return double{};
        },
        .pop_meter = [params](auto& e) -> bool {
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
                            // Reaches the algorithm as a coefficient packet, automatable
                            // or not: AAX_CParameter::SetValue always routes through the
                            // automation delegate, so UpdateParameterNormalizedValue fires
                            // either way.
                            param->SetNormalizedValue(a.value);
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
                [&](const auto&) {}
            }, action);
        }
    };

    // Now we have the receiver.
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_param(static_cast<uint32_t>(i)); }
    );

    _tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _editor->on_gui_show();
}

void Gui::DeleteViewContainer()
{
    _editor->on_gui_hide();
    _platform_view->on_hide();

    _editor->on_gui_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;
}

AAX_Result Gui::GetViewSize(AAX_Point* view_size) const
{
    const auto fallback = _params ? _params->get_last_size().value_or(plugin::Editor::preferred_size())
                                  : plugin::Editor::preferred_size();
    const auto size = _platform_view ? _platform_view->get_size() : fallback;
    view_size->horz = static_cast<float>(size.w);
    view_size->vert = static_cast<float>(size.h);
    return AAX_SUCCESS;
}

AAX_Result Gui::ParameterUpdated(AAX_CParamID inParamID)
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

auto Gui::on_draw(View_context& view_context) -> void
{
#if TINY_HAS_WORKER
    if (_params) _params->drain_worker_to_editor();
#endif

    view_impl::run_frame(
        User_meters::specs(),
        _receiver,
        _ui_params,
        _ui_meters,
        view_context,
        _editor,
        *_actions,
        *_params->undo_history(),
        *_tasks,
        [this](auto w, auto h) {
            auto* view = GetViewContainer();
            if (!view) return;
            auto size = AAX_Point{static_cast<float>(h), static_cast<float>(w)}; // vert, horz
            const auto result = view->SetViewSize(size);
            if (result != AAX_SUCCESS) return;
            _platform_view->resize(static_cast<int32_t>(w), static_cast<int32_t>(h));
            // No host echo on AAX — update the size cache directly so it persists.
            if (_params) _params->resized({static_cast<int32_t>(w), static_cast<int32_t>(h)});
        }
    );
}

auto Gui::on_notify(const Dark_mode_changed& notification) -> void
{
    _editor->notify(notification);
}

} // namespace tiny::aax