#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::plugin {

class Editor {
public:

    static auto preferred_size() -> Rect_size { return {560, 380}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void {}
    auto on_gui_destroy() -> void {}

    auto notify(const Host_event&) -> void;

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using User_meters = meters::Infos<models::Meters>;
    using Meter = models::Meters::Address;
    static constexpr auto num_meters = User_meters::num_meters;

    Edit_context _edit{};
    bool _dark{true};

    // The magnitudes of the last eight triggers, in arrival order. Falsifies both
    // failure modes at a glance: a phantom trigger repeats a magnitude, a
    // swallowed one leaves a gap in the processor's 1..8 cycle.
    std::array<uint32_t, 8> _trig_sequence{};
    size_t _trig_write{};
    double _trig_flash{}; // Seconds of highlight remaining.

    Time_point _last_time{};
    bool _has_time{};

    auto _draw_row(Plugin_state& state, int index, Meter address, const char* label) -> void;

};

} // namespace tiny::plugin
