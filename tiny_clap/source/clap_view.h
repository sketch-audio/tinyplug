#pragma once

#include <memory>

#include "clap/clap.h"

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "user/param_model.h"
#include "user/custom_view.h"

#include "clap_adapters.h"

namespace tiny {

struct Clap_view {

    Clap_view(tiny::Ui_receiver receiver) : _receiver{receiver} {}

    auto create() noexcept -> void
    {
        platform_view = Platform_views::make_owning(_delegate);
    }

    auto destroy() noexcept -> void
    {
        platform_view = nullptr;
    }

    auto get_size(uint32_t* w, uint32_t* h) noexcept -> void
    {
        const auto delegate_size = _delegate->getSize();
        *w = delegate_size.width;
        *h = delegate_size.height;
    }

    auto set_size(uint32_t w, uint32_t h) noexcept -> bool
    {
        if (!platform_view) return false;
        _delegate->onResize({static_cast<int>(w), static_cast<int>(h)});
        platform_view->resize(w, h);
        return true;
    }

    auto set_parent(const clap_window* window) noexcept -> bool
    {
        if (!platform_view) return false;
    
        // Resolve the platform window type.
        auto* platform_window = [=]() {
            if (Platform::resolved == Platform::Type::macos) {
                return window->cocoa;
            } else if (Platform::resolved == Platform::Type::windows) {
                return window->win32;
            }
        }();
        
        platform_view->receive_parent(platform_window);
        return true;
    }

private:

    using Draw_context = Graphics_delegate::Draw_context;
    auto on_draw(Draw_context& context) -> void
    {
        using namespace tiny;
        // Resolve application state

        // Pop the exports.
        auto event = Ui_event{};
        while (_receiver.pop_event(event)) {
            std::visit(
                Inline_visitor{
                    [&](const Set_param& p) {
                        _uivalues[p.id] = p.value;
                    },
                    [&](const Set_export& e) {
                        auto& ui_export = _exports[e.id];
                        if (!ui_export.updated) {
                            ui_export.value = 0; // Reset on first update in frame where we receive an event.
                        }
                        ui_export.value = std::max(ui_export.value, e.value);
                        ui_export.updated = true;
                    }
                }
            , event);
        }

        // Adapt to values.
        auto export_arr = std::array<double, User_exports::num_exports>{};
        std::ranges::copy(_exports | std::views::transform(&Ui_export::value), export_arr.begin());

        // Create view context.
        auto view_context = View_context{
            .params_state = {
                .params = _uivalues,
                .exports = export_arr 
            },
            .draw_context = context
        };

        // Tell the user view to draw.
        _view->on_draw(view_context);

        // Get ready for next frame.
        for (auto& ui_export : _exports) {
            ui_export.updated = false;
        }
    }

    using User_params = tiny::Param_infos<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    User_params _params{};

    tiny::Ui_receiver _receiver{};

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(
        Graphics_delegate::Size{800, 600}, // Initial size
        [this](auto& context) { this->on_draw(context); }
    );
    std::unique_ptr<Platform_view> platform_view{nullptr};

    using User_view = tiny::Custom_view;
    std::unique_ptr<User_view> _view = std::make_unique<User_view>();

    struct Ui_export {
        double value{};
        bool updated{};
    };
    std::array<Ui_export, num_exports> _exports{};
    std::array<double, num_params> _uivalues{_params.make_knob_defaults()};

};

}