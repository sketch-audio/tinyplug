#pragma once

#include <memory>

#include "clap/clap.h"

#include "tinyplug/tinyplug.hpp"
#include "platform/platform_view.hpp"

#include "editor.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::clap {

// The CLAP view adapts the view lifecycle to the user's `plugin::Editor`.
class View {
public:

    struct Deps {
        plugin::Editor* editor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
#if TINY_HAS_WORKER
        std::function<void()> drain_worker_to_editor{};
#endif
    };

    View(Deps deps) : _deps{deps} {}
    ~View() = default;

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

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    Action_queue _actions{};
    Undo_history _undo_history{};

    Deps _deps{};

    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        },
        .save_model = [this]() {
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(_ui_params.begin(), _ui_params.end()),
                .editor_state = _deps.editor ? _deps.editor->save_state() : State_map{}
            };
        },
    }};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    std::array<view::Meter_state, num_meters> _ui_meters{};
    std::array<double, num_params> _ui_params{User_params::make_defaults<double>(Value_space::Knob)};

};

} // namespace tiny::clap