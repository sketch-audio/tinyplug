#pragma once

#include <memory>

#include "tinyplug/tinyplug.h"
#include "platform/platform_view.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"

#include "auv2_adapters.h"

namespace tiny {

class Auv2_view {
public:

    Auv2_view(Ui_receiver receiver, std::shared_ptr<Plug_editor> editor, Main_executor executor)
        : _receiver{receiver}, _editor{editor}, _executor {executor} {}

    auto create_view() -> void*;

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Ui_notification& notification) -> void;

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    User_params _param_infos{};
    User_meters _meter_infos{};
    
    Action_queue _actions{};
    Task_queue _tasks{};

    Ui_receiver _receiver{};
    std::shared_ptr<Plug_editor> _editor{};
    Main_executor _executor{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<Tagged_meter, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{_param_infos.make_knob_defaults<double>()};

};

} // namespace tiny