#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

#include <tinyplug/tinyplug.hpp>

// Parameter controls for simple editors, free of Skia so an editor header can hold them.
// Drawing lives in draw.hpp.
namespace tiny::ui {

// MARK: - controls

// A parameter on screen: a horizontal slider, a vertical fader, a toggle that flips on click, or
// one choice of a list (click sets the knob to `choice`).
struct Control {
    enum class Kind { Slider, Fader, Toggle, Choice };
    Frame frame{};
    uint32_t address{};
    Kind kind{Kind::Slider};
    std::string_view label{};
    uint32_t color{0xff5a96eb}; // An SkColor.
    double choice{};            // Knob value, for `Choice`.
};

// Turns pointer events over controls into gestures: one open at a time, closed on release or
// cancel, so the host sees begin / edit / end exactly as it would from a knob. Marks the events
// it takes as consumed.
class Controls {
public:

    auto interact(User_interaction& interaction, Action_queue::Actor& actions, std::span<const Control> controls,
                  std::span<const double> knobs) -> void
    {
        for (auto& event : interaction.events.events) {
            std::visit(Inline_visitor{
                [&](const Pointer_down& down) {
                    if (down.button != Pointer_button::left || _active) return;
                    for (const auto& c : controls) {
                        if (!c.frame.contains(down.pos)) continue;
                        event.consumed = true;
                        actions.push(Action_start{c.address});
                        if (c.kind == Control::Kind::Toggle || c.kind == Control::Kind::Choice) {
                            const auto toggled = knobs[c.address] >= 0.5 ? 0. : 1.;
                            actions.push(Set_param{c.address, c.kind == Control::Kind::Choice ? c.choice : toggled});
                            actions.push(Action_end{c.address});
                            return;
                        }
                        _active = Drag{c.address, c.frame, event.pointer_tag, c.kind == Control::Kind::Fader};
                        actions.push(Set_param{c.address, _active->knob_at(down.pos)});
                        return;
                    }
                },
                [&](const Pointer_move& move) {
                    if (!_active || event.pointer_tag != _active->tag) return;
                    event.consumed = true;
                    actions.push(Set_param{_active->address, _active->knob_at(move.pos)});
                },
                [&](const auto& e) {
                    if constexpr (std::is_same_v<std::decay_t<decltype(e)>, Pointer_up> || std::is_same_v<std::decay_t<decltype(e)>, Pointer_cancel>) {
                        if (!_active || event.pointer_tag != _active->tag) return;
                        event.consumed = true;
                        actions.push(Action_end{_active->address});
                        _active.reset();
                    }
                },
            }, event.event);
        }
    }

    // Close an open gesture, e.g. when the window hides mid-drag.
    auto cancel(Action_queue::Actor& actions) -> void
    {
        if (_active) actions.push(Action_end{_active->address});
        _active.reset();
    }

private:

    struct Drag {
        uint32_t address{};
        Frame frame{};
        uintptr_t tag{};
        bool vertical{};

        auto knob_at(Coords pos) const -> double
        {
            return std::clamp(vertical ? (frame.y + frame.h - pos.y) / frame.h : (pos.x - frame.x) / frame.w, 0., 1.);
        }
    };

    std::optional<Drag> _active{};

};

} // namespace tiny::ui
