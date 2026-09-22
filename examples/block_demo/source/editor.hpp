#pragma once

#include <array>
#include <memory>

#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Spectrum and scope side by side, plus a gain strip. The spectrum reads `latest` every
// draw; the scope takes only `fresh` frames. Each panel counts its arrivals per second.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {760, 340}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void;
    auto notify(const Host_event&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void {}
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;

    // Arrivals per second for one block, refreshed once a second.
    struct Rate {
        int count{};
        int shown{};
        double elapsed{};

        auto tick(bool arrived, double dt) -> void;
    };

    Edit_context _edit{};
    bool _dark{true};

    Time_point _last_time{};
    bool _has_time{};

    std::array<float, models::Spectrum_frame::max_bins> _display{}; // Falling peaks, dBFS.
    models::Scope_frame _scope{};                                     // The last fresh sweep.
    Rate _spectrum_rate{};
    Rate _scope_rate{};

    Frame _gain_frame{};
    Coords _translation{};
    double _gain{};
    std::unique_ptr<Gesture_recognizer> _drag{};

};

} // namespace tiny::edit
