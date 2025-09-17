#pragma once

#include <memory>

#include "clap/clap.h"

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "custom_view.h"
#include "param_model.h"

namespace tiny {

class Clap_view {
public:

    Clap_view(Ui_receiver receiver) : _receiver{receiver} {}

    auto create() noexcept -> void;
    auto destroy() noexcept -> void;
    auto get_size(uint32_t* w, uint32_t* h) noexcept -> void;
    auto set_size(uint32_t w, uint32_t h) noexcept -> bool;
    auto set_parent(const clap_window* window) noexcept -> bool;

    auto set_param(uint32_t id, double knob_value) -> void
    {
        _uiparams[id] = knob_value;
    }

private:

    auto on_draw(View_context& view_context) -> void;

    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    User_params _param_infos{};
    Action_queue _actions{};
    Task_queue _tasks{};
    Ui_receiver _receiver{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};
    std::unique_ptr<Custom_view> _custom_view = std::make_unique<Custom_view>();

    std::array<Tagged_export, num_exports> _uiexports{};
    std::array<double, num_params> _uiparams{_param_infos.make_knob_defaults<double>()};

};

} // namespace tiny