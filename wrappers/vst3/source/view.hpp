#pragma once

#include <array>
#include <memory>

#include "public.sdk/source/common/pluginview.h"

#include "tinyplug/tinyplug.hpp"
#include <tiny_platform/platform_view.hpp>

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

namespace tiny::vst3 {

class Controller;

class View : public Steinberg::CPluginView {
public:

    struct Deps {
        Controller* controller{}; // So we can cache the resized size.
        plugin::Editor* editor{};
        Ui_receiver receiver{};
        Task_manager* tasks{};
        Undo_history* undo_history{}; // Owned by the controller (survives the view).
        Action_queue* actions{};      // Owned by the controller (survives the view).
#if TINY_HAS_WORKER
        std::function<void()> drain_worker_to_editor{};
#endif
    };

    using Super = Steinberg::CPluginView;
    View(const Deps& deps) : Super{}, _deps{deps} {}
    ~View() = default;

    Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API attached(void* parent, Steinberg::FIDString type) override;
    Steinberg::tresult PLUGIN_API removed() override;
    Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect* size) override;
    Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect* newSize) override;
    Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame* frame) override;
    Steinberg::tresult PLUGIN_API canResize() override;
    Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect* rect) override;

    DELEGATE_REFCOUNT(Super)

protected:

    // This is where we resolve the app state and tell the user's custom view to draw.
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

} // namespace tiny::vst3