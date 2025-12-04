#pragma once

#include <memory>

#include "clap/clap.h"

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "plug_editor.h"
#include "models/meter_model.h"
#include "models/param_model.h"

namespace tiny {

// The CLAP view adapts the view lifecycle to the user's `Plug_editor`.
class Clap_view {
public:

    Clap_view(Ui_receiver receiver, std::shared_ptr<Plug_editor> editor)
        : _receiver{receiver}, _editor{editor} {}

    ~Clap_view() = default;

    auto on_create() noexcept -> void;
    auto on_show() noexcept -> void;
    auto on_hide() noexcept -> void;
    auto on_destroy() noexcept -> void;

    auto get_size(uint32_t* w, uint32_t* h) noexcept -> void;
    auto set_size(uint32_t w, uint32_t h) noexcept -> bool;
    auto set_parent(const clap_window* window) noexcept -> bool;

    // When we pull the CLAP kernel back into the plug-in, we should be able to get rid of this.
    auto set_param(uint32_t id, double knob_value) -> void
    {
        _ui_params[id] = knob_value;
    }

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Ui_notification& notification) -> void;

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    User_meters _meter_infos{};
    
    Action_queue _actions{};
    Undo_history _undo_history{};

    Ui_receiver _receiver{};
    std::shared_ptr<Plug_editor> _editor{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<Tagged_meter, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};

};

} // namespace tiny