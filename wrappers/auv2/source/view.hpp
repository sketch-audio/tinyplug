#pragma once

#include <memory>

#include <tinyplug/tinyplug.hpp>
#include <tiny_platform/platform_view.hpp>

#include <tiny_plugin.hpp>

#include "adapters.hpp"

namespace tiny::auv2 {

class View {
public:

    struct Deps {
        User_editor* editor{};
        edit::Connection receiver{};
        Task_manager* tasks{};
        Undo_history* undo_history{}; // Owned by the Effect (survives the view).
        Action_queue* actions{};      // Owned by the Effect (survives the view).
        Edit_store* edit_store{};     // The size to open at, read at create_view (after any restore).
#if TINY_HAS_WORKER
        std::function<void()> drain_worker_to_editor{};
#endif
    };

    View(Deps deps) : _deps{deps} {}
    ~View();
    View(const View&) = delete;
    auto operator=(const View&) -> View& = delete;

    auto create_view() -> void*;

private:

    auto on_draw(View_context& view_context) -> void;
    auto resize(Rect_size size) -> void; // AUv2 has no host echo: the view resizes itself.


    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    Deps _deps{};

    std::unique_ptr<Platform_view> _platform_view{nullptr};

    // Plain values now: the mailbox coalesces on the way in, so the editor no
    // longer reconstructs per-frame peak/latest/one-shot state of its own.
    std::array<double, num_meters> _ui_meters{};
    blocks::Frames<models::Resolved::Blocks> _ui_blocks{}; // Retained between draws.

    using enum params::Space;
    std::array<double, num_params> _ui_params{tiny::params::make_defaults<double, User_params>(Knob)};

};

} // namespace tiny::auv2