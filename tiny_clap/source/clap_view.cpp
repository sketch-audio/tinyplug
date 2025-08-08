#include "clap_view.h"

#include <algorithm>
#include <ranges>
#include <string>
#include <utility>
#include <variant>

namespace tiny {

auto Clap_view::create() noexcept -> void
{
    auto delegate = std::make_shared<View_delegate>(
        initial_size,
        [this](auto& context) { this->on_draw(context); }
    );
    _platform_view = Platform_views::make_owning(delegate);
}

auto Clap_view::destroy() noexcept -> void
{
    _platform_view = nullptr;
}

auto Clap_view::get_size(uint32_t* w, uint32_t* h) noexcept -> void
{
    const auto platform_size = _platform_view ? _platform_view->get_size() : initial_size;
    *w = platform_size.w;
    *h = platform_size.h;
}

auto Clap_view::set_size(uint32_t w, uint32_t h) noexcept -> bool
{
    if (!_platform_view) return false;
    _platform_view->resize(w, h);
    return true;
}

auto Clap_view::set_parent(const clap_window* window) noexcept -> bool
{
    if (!window || !_platform_view) return false;

    // Resolve the platform window type.
    const auto is_mac = Platform::resolved == Platform::Type::macos;
    auto* platform_window = is_mac ? window->cocoa : window->win32;

    _platform_view->receive_parent(platform_window);

    for (auto i = uint32_t{}; i < num_params; ++i) {
        _uivalues[i] = _receiver.get_knob_value(i);
    }
    
    return true;
}

auto Clap_view::on_draw(View_context& view_context) -> void
{
    // Pop the exports.
    auto event = Ui_event{};
    while (_receiver.pop_event(event)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                _uivalues[p.id] = p.value;
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

    // Adapt tagged exports to values.
    auto export_arr = std::array<double, num_exports>{};
    const auto value_tx = _uiexports | std::views::transform(&Ui_export::value);
    std::ranges::copy(value_tx, export_arr.begin());

    // Create view context.
    auto app_state = App_state{
        .params_state = {
            .params = _uivalues,
            .exports = export_arr 
        },
        .action_receiver = {},
        .view_context = view_context
    };

    // Tell the user view to draw.
    _custom_view->on_draw(app_state);

    auto& actions = app_state.action_receiver.actions();
    for (auto& action : actions) {
        _receiver.action_handler(action);
        std::visit(Inline_visitor{
            [&](const Set_param& s) { _uivalues[s.id] = s.value; },
            [](const auto&) {}
        }, action);
    }

    // Get ready for next frame.
    for (auto& ui_export : _uiexports) {
        ui_export.updated = false;
    }
}

} // namespace tiny
