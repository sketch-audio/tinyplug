#pragma once

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

class Custom_view {
public:
    // This is where you draw your plug-in's UI and handle
    // The app state gives you
    // - Read-only access to the param and export values.
    // - A view context with the interaction state and a canvas in which to draw.
    // - A receiver for your control's actions.
    auto on_draw(App_state& app_state) -> void;

private:

    using User_params = Param_infos<Param_model>;

    User_params _params{};
    double _ldrag{}; // norm

    // Input smoothing.
    double _x{};
    Time_point _t{System_clock::now()};

};

} // namespace tiny