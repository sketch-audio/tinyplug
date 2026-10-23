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

    auto delegate = std::make_shared<View_delegate>(
        _params->edit_store().size(), // The session's size, or the preferred one.
        [this](auto& context) { this->on_draw(context); }
    );
    _params->edit_store().set_apply([this](Rect_size size) { _resize(size.w, size.h); });

    _platform_view = platform::View::make_owning(delegate);
    _platform_view->on_create();
    _editor->on_create(Gui_info{.window = _platform_view->token()});
}

auto Gui::CreateViewContainer() -> void
{
    using namespace params;

    auto* parent = GetViewContainerPtr();
    if (!parent) return;

    _platform_view->receive_parent(parent);

    auto* view = GetViewContainer();
    auto* params = dynamic_cast<Parameters*>(GetEffectParameters());

    // We have to make our own UI connection.
    _receiver = edit::Connection{
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
#if TINY_HAS_METERS
        .read_meters = [params](std::span<float> out) {
            if (params) params->read_meters(out);
        },
#endif
#if TINY_HAS_BLOCKS
        .read_blocks = [params](blocks::Frames<models::Resolved::Blocks>& out) {
            if (params) params->read_blocks(out);
        },
#endif
        .begin = [this, view, params](std::uint32_t address) {
            _gestured.insert(address);
            if (const auto aax_id = tiny_id_to_aax(address)) {
                const auto* id_cstr = (*aax_id).c_str();
                view->HandleParameterMouseDown(id_cstr, 0);
                AAX_IParameter* param = nullptr;
                if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS && param->Automatable()) param->Touch();
            }
        },
        .set = [view, params](std::uint32_t address, double knob) {
            if (const auto aax_id = tiny_id_to_aax(address)) {
                const auto* id_cstr = (*aax_id).c_str();
                view->HandleParameterMouseDrag(id_cstr, 0);
                AAX_IParameter* param = nullptr;
                if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS) {
                    // Reaches the algorithm as a coefficient packet, automatable or not: AAX_CParameter::SetValue
                    // always routes through the automation delegate, so UpdateParameterNormalizedValue fires either way.
                    param->SetNormalizedValue(knob);
                }
            }
        },
        .end = [this, view, params](std::uint32_t address) {
            if (const auto aax_id = tiny_id_to_aax(address)) {
                const auto* id_cstr = (*aax_id).c_str();
                view->HandleParameterMouseUp(id_cstr, 0);
                AAX_IParameter* param = nullptr;
                if (params->GetParameter(id_cstr, &param) == AAX_SUCCESS && param->Automatable()) param->Release();
            }
            _gestured.erase(address);
        },
#if TINY_HAS_STATE
        .sync_state = [params]() {
            if (params) params->sync_state();
        },
#endif
        .end_frame = [params]() {
            if (params) params->edit_store().end_frame();
        },
    };

    // Now we have the receiver.
    _ui_params = make_array_by_indices<double, num_params>(
        [this](auto i) { return _receiver.get_param(static_cast<uint32_t>(i)); }
    );

    _tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
    _platform_view->on_show();
    _editor->on_show();
}

void Gui::DeleteViewContainer()
{
    _editor->on_hide();
    if (_params) {
        view_impl::close_session(_receiver, _ui_params, *_actions, *_params->undo_history());
        _params->edit_store().set_apply({});
    }
    _platform_view->on_hide();

    _editor->on_destroy();
    _platform_view->on_destroy();
    _platform_view = nullptr;
}

AAX_Result Gui::GetViewSize(AAX_Point* view_size) const
{
    const auto fallback = _params ? _params->edit_store().size() : sizing_of<User_editor>().preferred;
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
        _receiver,
        _ui_params,
        _ui_meters,
        _ui_blocks,
        view_context,
        _editor,
        *_actions,
        *_params->undo_history(),
        *_tasks
    );
}

auto Gui::_resize(int32_t w, int32_t h) -> void
{
    auto* view = GetViewContainer();
    if (!view || !_platform_view) return;
    auto size = AAX_Point{static_cast<float>(h), static_cast<float>(w)}; // vert, horz
    if (view->SetViewSize(size) != AAX_SUCCESS) return;
    _platform_view->resize(w, h);
    if (_params) _params->edit_store().resized({w, h}); // No host echo on AAX: this is the size now.
}

} // namespace tiny::aax