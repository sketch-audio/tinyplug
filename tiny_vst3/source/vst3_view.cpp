#include "vst3_view.h"

namespace tiny {

Steinberg::tresult PLUGIN_API Vst3_view::isPlatformTypeSupported(Steinberg::FIDString type)
{
    const auto platform_type = []() {
        switch (Platform::resolved) {
            case Platform::Type::macos:
                return Steinberg::kPlatformTypeNSView;
            case Platform::Type::ios:
                return Steinberg::kPlatformTypeUIView;
            case Platform::Type::windows:
                return Steinberg::kPlatformTypeHWND;
        }
    }();

    if (strcmp(type, platform_type) == 0)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::attached(void* parent, Steinberg::FIDString /*type*/)
{
    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); }
    );
    _platform_view = Platform_views::make_owning(delegate);
    _platform_view->receive_parent(parent);

    for (auto i = uint32_t{}; i < num_params; ++i) {
        _uivalues[i] = _receiver.get_knob_value(i);
    }
    
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::removed()
{
    _platform_view = nullptr;
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onWheel(float /*distance*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::onKeyDown(Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/, Steinberg::int16 /*modifiers*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::onKeyUp(Steinberg::char16 /*key*/, Steinberg::int16 /*keyCode*/, Steinberg::int16 /*modifiers*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::getSize(Steinberg::ViewRect* size)
{
    const auto platform_size = _platform_view ? _platform_view->get_size() : initial_size;
    *size = {0, 0, platform_size.w, platform_size.h};
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onSize(Steinberg::ViewRect* newSize)
{
    if (!_platform_view || !newSize) return Steinberg::kResultFalse;
    const auto w = newSize->getWidth();
    const auto h = newSize->getHeight();
    _platform_view->resize(w, h);
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::onFocus(Steinberg::TBool /*state*/)
{
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Vst3_view::setFrame(Steinberg::IPlugFrame* /*frame*/)
{
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::canResize()
{
    return Steinberg::kResultTrue;
}

Steinberg::tresult PLUGIN_API Vst3_view::checkSizeConstraint(Steinberg::ViewRect* /*rect*/)
{
    return Steinberg::kResultTrue;
}

// MARK: - on_draw

void Vst3_view::on_draw(View_context& view_context)
{
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) { _uivalues[p.id] = p.value; },
            [&](const Set_export& e) {
                auto& ui_export = _uiexports[e.id];
                if (!ui_export.updated) {
                    ui_export.value = 0;
                }
                ui_export.value = std::max(ui_export.value, e.value);
                ui_export.updated = true;
            }
        }, event);
    }

    auto export_arr = std::array<double, User_exports::num_exports>{};
    const auto value_tx = _uiexports | std::views::transform(&Ui_export::value);
    std::ranges::copy(value_tx, export_arr.begin());

    auto app_state = App_state{
        .params_state = {
            .params = _uivalues,
            .exports = export_arr
        },
        .action_receiver = Action_receiver{},
        .view_context = view_context
    };

    _custom_view->on_draw(app_state);

    auto& actions = app_state.action_receiver.actions();
    for (auto& action : actions) {
        _receiver.action_handler(action);
        std::visit(Inline_visitor{
            [&](const Set_param& s) { _uivalues[s.id] = s.value; },
            [](const auto&) {}
        }, action);
    }

    for (auto& ui_export : _uiexports) {
        ui_export.updated = false;
    }
}

} // namespace tiny