#pragma once

#include <memory>

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "custom_view.h"
#include "param_model.h"

#include "auv2_adapters.h"

namespace tiny {

class Auv2_view {
public:

    Auv2_view(Ui_receiver receiver, Main_executor executor) : _receiver{receiver}, _executor{executor} {}

    auto create_view() -> void*;

private:

    auto on_draw(View_context& view_context) -> void;

    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto initial_size = Rect_size{800, 600};
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    User_params _param_infos{};
    Ui_receiver _receiver{};
    Main_executor _executor{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};
    std::unique_ptr<Custom_view> _custom_view = std::make_unique<Custom_view>();

    std::array<Tagged_export, num_exports> _uiexports{};
    std::array<double, num_params> _uiparams{_param_infos.make_knob_defaults<double>()};

};

} // namespace tiny