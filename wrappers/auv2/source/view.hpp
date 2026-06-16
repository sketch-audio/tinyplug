#pragma once

#include <memory>

#include "tinyplug/tinyplug.hpp"
#include <tiny_platform/platform_view.hpp>

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

#include "adapters.hpp"

namespace tiny::auv2 {

class View {
public:

    struct Deps {
        plugin::Editor* editor{};
        Main_executor executor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
        Undo_history* undo_history{}; // Owned by the Effect (survives the view).
        Action_queue* actions{};      // Owned by the Effect (survives the view).
#if TINY_HAS_WORKER
        std::function<void()> drain_worker_to_editor{};
#endif
    };

    View(Deps deps) : _deps{deps} {}

    auto create_view() -> void*;

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Dark_mode_changed& notification) -> void;

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    Deps _deps{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<view::Meter_state, num_meters> _ui_meters{};

    using enum params::Space;
    std::array<double, num_params> _ui_params{tiny::params::make_defaults<double, User_params>(Knob)};

};

} // namespace tiny::auv2