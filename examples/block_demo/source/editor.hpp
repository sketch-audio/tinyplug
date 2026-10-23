#pragma once

#include <array>
#include <memory>

#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Spectrum and scope side by side, plus a gain strip. The spectrum reads `latest` every
// draw; the scope takes only `fresh` frames. Each panel counts its arrivals per second.
class Editor {
public:

    static constexpr auto sizing() -> Sizing { return {.preferred = {760, 340}}; }

    Editor(const edit::Context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_create(Gui_info) -> void {}
    auto on_show() -> void;
    auto on_draw(Plugin_state&) -> void;
    auto on_hide() -> void {}
    auto on_destroy() -> void {}

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

    edit::Context _edit{};
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
    std::unique_ptr<gesture::Recognizer> _drag{};

};
static_assert(Interface<Editor>);

} // namespace tiny::edit
