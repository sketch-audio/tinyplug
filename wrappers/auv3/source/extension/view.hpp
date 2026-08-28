#pragma once

#include <memory>

#include "tinyplug/tinyplug.hpp"
#include <tiny_platform/platform_view.hpp>

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

namespace tiny::auv3 {

class View {
public:
    
    struct Deps {
        plugin::Editor* editor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
        Undo_history* undo_history{}; // Owned by the AU (survives the view).
        Action_queue* actions{};      // Owned by the AU (survives the view).
#if TINY_HAS_WORKER
        std::function<void()> drain_worker_to_editor{};
#endif
    };

    View(const Deps& deps) : _deps{deps} {}

    auto create_view() -> void*; // UIView*

    auto on_show() -> void
    {
        _deps.tasks->bind_main(std::this_thread::get_id()); // Can we do it here?
        _platform_view->on_show();
        _deps.editor->on_gui_show();
    }

    auto on_hide() -> void
    {
        _deps.editor->on_gui_hide();
        _platform_view->on_hide();
    }

    auto on_destroy() -> void
    {
        _deps.editor->on_gui_destroy();
        _platform_view->on_destroy();
    }

    auto on_resize(int32_t w, int32_t h) -> void
    {
        _platform_view->resize(w, h);
    }

private:

    auto on_draw(View_context& view_context) -> void;
    auto on_notify(const Dark_mode_changed& notification) -> void;

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    Deps _deps{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    // Plain values now: the mailbox coalesces on the way in, so the editor no
    // longer reconstructs per-frame peak/latest/one-shot state of its own.
    std::array<double, num_meters> _ui_meters{};

    using enum params::Space;
    std::array<double, num_params> _ui_params{tiny::params::make_defaults<double, User_params>(Knob)};

};

} // namespace tiny::auv3
