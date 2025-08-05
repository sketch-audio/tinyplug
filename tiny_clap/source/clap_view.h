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

    Clap_view(Pop_export pop_export) : _pop_export{pop_export} {}

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
        auto event = Export_event{};
        while (_pop_export(event)) {
            auto& ui_export = _exports[event.id];
            if (!ui_export.updated) {
                ui_export.value = 0; // Reset on first update in frame where we receive an event.
            }
            ui_export.value = std::max(ui_export.value, event.value);
            ui_export.updated = true;
        }

        // Adapt to values.
        auto export_arr = std::array<double, User_exports::num_exports>{};
        std::ranges::copy(_exports | std::views::transform(&Ui_export::value), export_arr.begin());

        // Create view context.
        auto view_context = View_context{
            .params_state = {
                .params = {},
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

    Pop_export _pop_export{}; // A function to pop exports

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
    std::array<Ui_export, User_exports::num_exports> _exports{};

};

}