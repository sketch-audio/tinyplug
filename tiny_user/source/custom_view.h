#pragma once

#include "tinyplug/tinyplug.h"
#include "param_model.h"

namespace tiny {

class Custom_view {
public:
    // This is where your view is "created" (maybe presented is a better term).
    // You receive some things that will persist until the next `on_create`.
    // - Action receiver: In `on_draw` you can push user actions like `Set_param` to the host/DSP.
    // - Task receiver: If you need a `Later` executed on the draw thread, you can use it to capture `this` and push a task.
    auto on_create(const Action_queue::Receiver& actions, const Task_queue::Receiver& tasks) -> void;

    // This is where you draw your plug-in's UI and handle the user's interaction.
    // The app state gives you
    // - Read-only access to the param and export values.
    // - A view context with the interaction state and a canvas in which to draw.
    auto on_draw(App_state& app_state) -> void;

private:

    using User_params = Param_infos<Param_model>;

    User_params _params{};
    Action_queue::Receiver _actions{};
    Task_queue::Receiver _task_receiver{};

    bool _down{};
    double _ldrag{}; // norm

    // Input smoothing.
    double _x{};
    Time_point _t{System_clock::now()};

};

} // namespace tiny