#include "tiny_core/gesture_recognizers.hpp"

#include <chrono>
#include <variant>

#include "tiny_core/platform_defs.hpp" // TINY_PLATFORM_* (used below)

namespace tiny::gesture {

// MARK: - Over

auto Over::set_frame(const Frame& frame) -> void
{
    const auto moved = (frame != _frame); // Make sure we really changed frame.
    _frame = frame;

    if (_over && moved) {
        _callbacks.on_cancelled();
        _over = false;
        _tag = std::nullopt;
    }
}

auto Over::process_events(Event_list& events) -> void
{
    const auto has_touches = !events.pointer_origins.empty();
    auto touch_is_ours = [&](auto tag) {
        // We consider a touch to be "ours" if it originated within our frame, even if it has since left.
        for (const auto& origin : events.pointer_origins) {
            if (origin.tag == tag) {
                return _frame.contains(origin.pos);
            }
        }
        return false;
    };

    for (const auto& event : events.events) {
        if (event.consumed) continue;
        std::visit(Inline_visitor{
            [&](const Pointer::Move& move) {
                if (has_touches && !touch_is_ours(event.pointer_tag)) return;
                const auto was_over = _over;
                const auto now_over = _frame.contains(move.pos);
                resolve_events(move.pos, was_over, now_over, event.pointer_tag);
            },
            [&](const Pointer::Enter& enter) {
                if (has_touches && !touch_is_ours(event.pointer_tag)) return;
                const auto was_over = _over;
                const auto now_over = _frame.contains(enter.pos);
                resolve_events(enter.pos, was_over, now_over, event.pointer_tag);
            },
            [&](const Pointer::Exit& exit) {
                if (has_touches && !touch_is_ours(event.pointer_tag)) return;
                resolve_events(exit.pos, _over, false, event.pointer_tag);
            },
#if TINY_PLATFORM_IOS
            [&](const Pointer::Up& up) {
                if (_tag && *_tag != event.pointer_tag) return; // Some other finger let go.
                resolve_events(up.pos, _over, false, event.pointer_tag);
            },
#endif
            [&](const Pointer::Cancel& cancel) {
                if (_tag && *_tag != event.pointer_tag) return;
                resolve_events(cancel.pos, _over, false, event.pointer_tag);
            },
            [](const auto&) {}
        }, event.event);
    }
}

auto Over::resolve_events(Coords pos, bool was_over, bool now_over, uintptr_t tag) -> void
{
    if (!was_over && now_over) {
        _callbacks.on_started({pos, true});
        _tag = tag; // Bind the pointer that brought us over.
    }
    else if (was_over && !now_over) {
        _callbacks.on_ended({pos, false});
        _tag = std::nullopt;
    }
    _over = now_over;
}

// MARK: - Down

auto Down::set_frame(const Frame& frame) -> void
{
    const auto moved = (frame != _frame); // Make sure we really changed frame.
    _frame = frame;

    if (_down && moved) {
        _callbacks.on_cancelled();
        _down = false;
        _tag = std::nullopt;
    }
}

auto Down::process_events(Event_list& events) -> void
{
    for (const auto& event : events.events) {
        if (event.consumed) continue;
        std::visit(Inline_visitor{
            [&](const Pointer::Down& down) {
                if (down.button != Pointer::Button::left) return;
                if (_down) return; // Already bound; a second finger must not re-press us.
                if (_frame.contains(down.pos)) {
                    _callbacks.on_started({down.pos, true});
                    _down = true;
                    _tag = event.pointer_tag;
                }
            },
            [&](const Pointer::Up& up) {
                if (!_down) return;
                if (_tag && *_tag != event.pointer_tag) return; // Some other finger let go.
                _callbacks.on_ended({up.pos, false}); // Pointer could have left our frame.
                _down = false;
                _tag = std::nullopt;
            },
            [&](const Pointer::Cancel&) {
                if (!_down) return;
                if (_tag && *_tag != event.pointer_tag) return;
                _callbacks.on_cancelled();
                _down = false;
                _tag = std::nullopt;
            },
            [](const auto&) {}
        }, event.event);
    }
}

// MARK: - Dwell

auto Dwell::set_frame(const Frame& frame) -> void
{
    const auto moved = (frame != _frame); // Make sure we really changed frame.
    _frame = frame;
    if (!moved) return;

    if (_dwelling) {
        _callbacks.on_cancelled();
        _dwelling = false;
    }
    _over_t = std::nullopt;
}

auto Dwell::process_events(Event_list& events) -> void
{
    // Replaces global down.
    auto pointer_is_down = [&](uintptr_t tag) {
        for (const auto& origin : events.pointer_origins) {
            if (origin.tag == tag) return true;
        }
        return false;
    };

    for (const auto& event : events.events) {
        if (event.consumed) continue;

        std::visit(Inline_visitor{
            [&](const Pointer::Down& down) {
                if (_dwelling) {
                    end_dwell(down.pos);
                    _over_t = std::nullopt;
                }
            },
            [&](const Pointer::Up&) {
#if TINY_PLATFORM_IOS
                // On touch there is no pointer once the finger lifts, so nothing can be
                // hovering. `pointer_origins` is erased when the up is pushed but the list
                // is snapshotted at consume, so any move sharing a frame with the up reads
                // as "nothing down" and arms the timer — the tooltip then lands seconds
                // after the gesture ended. Same reason `Over` ends hover here.
                _over_t = std::nullopt;
#endif
            },
            [&](const Pointer::Move& move) {
                if (pointer_is_down(event.pointer_tag)) { // Held pointer: dragging, not hovering.
                    _over_t = std::nullopt;
                    return;
                }

                if (_dwelling) {
                    end_dwell(move.pos);
                    _over_t = std::nullopt;
                    return;
                }

                if (!_frame.contains(move.pos)) {
                    _over_t = std::nullopt;
                    return;
                }

                _over_t = events.timestamp;
                _pos = move.pos;
            },
            [&](const Pointer::Cancel&) {
                if (_dwelling) {
                    _callbacks.on_cancelled();
                    _dwelling = false;
                }
                _over_t = std::nullopt;
            },
            [&](const auto&) {
                if (_dwelling) {
                    end_dwell(_pos);
                }
                _over_t = std::nullopt;
            }
        }, event.event);
    }

    if (_over_t) {
        const auto now = Steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - *_over_t);
        if (!_dwelling && elapsed.count() > dwell_ms) {
            _callbacks.on_started({_pos, true});
            _dwelling = true;
        }
    }
    else {
        if (_dwelling) {
            end_dwell(_pos);
        }
    }
}

auto Dwell::end_dwell(Coords pos) -> void
{
    _callbacks.on_ended({pos, false});
    _dwelling = false;
}

// MARK: - Click

auto Click::set_frame(const Frame& frame) -> void
{
    _frame = frame;
}

auto Click::process_events(Event_list& events) -> void
{
    for (auto& event : events.events) {
        if (event.consumed) continue;
        std::visit(Inline_visitor{
#if TINY_PLATFORM_APPLE
            [&](const Pointer::Down& down) {
                // Right click executes on pointer down on macOS.
                if (_frame.contains(down.pos) && down.button == Pointer::Button::right) {
                    const auto match = (_desc.button == down.button && _desc.count == 1);
                    if (match) _callbacks.on_started({down.pos});

                    if (_desc.greedy) {
                        event.consumed = true;
                    }
                }
            },
#endif
            [&](const Pointer::Click& click) {
                if (_frame.contains(click.pos)) {
                    const auto single = (_desc.button == click.button && _desc.count == 1 && click.count >= 1); // Allow multiple clicks to trigger single-click handlers.
                    const auto dbl = (_desc.button == click.button && _desc.count == 2 && click.count == 2);
                    const auto match = single || dbl;
                    if (match) _callbacks.on_started({click.pos});

                    if (_desc.greedy) {
                        event.consumed = true;
                    }
                }
            },
            [](const auto&) {}
        }, event.event);
    }
}

// MARK: - Drag

auto Drag::set_frame(const Frame& frame) -> void
{
    _frame = frame;
    if (!_cancels_on_frame_change) return;

    if (_fpos.has_value() || _tag.has_value() || _initiated) {
        _callbacks.on_cancelled();
        reset();
    }
}

auto Drag::process_events(Event_list& events) -> void
{
    for (auto& event : events.events) {
        if (event.consumed) continue;
        if (_tag && *_tag != event.pointer_tag) continue; // Skip events for unbound pointers

        std::visit(Inline_visitor{
            [&](const Pointer::Down& down) {
                if (down.button != Pointer::Button::left) return; // Only primary button drags.
                if (_frame.contains(down.pos)) {
                    // set
                    _fpos = down.pos;
                    _tag = event.pointer_tag;
                    _initiated = false;

                    event.consumed = _greedy ? true : event.consumed;
                }
            },
            [&](const Pointer::Up& up) {
                if (_fpos && _initiated) {
                    _callbacks.on_ended({*_fpos, up.pos});
                    event.consumed = _greedy ? true : event.consumed;
                }
                reset();
            },
            [&](const Pointer::Move& move) {
                if (_fpos) {
                    if (!_initiated) {
                        // Below the slop this is still a click's wobble, not a drag —
                        // don't start, and don't consume.
                        if (!past_slop(*_fpos, move.pos)) return;
                        _callbacks.on_started({*_fpos, move.pos});
                        _initiated = true;
                    }
                    else {
                        _callbacks.on_updated({*_fpos, move.pos});
                    }
                    event.consumed = _greedy ? true : event.consumed;
                }
            },
            [&](const Pointer::Cancel&) {
                if (_initiated) {
                    _callbacks.on_cancelled();
                }
                reset();
            },
            [](const auto&) {}
        }, event.event);
    }
}

auto Drag::past_slop(Coords fpos, Coords tpos) const -> bool
{
    if (_start_slop <= 0.) return true;
    const auto dx = tpos.x - fpos.x;
    const auto dy = tpos.y - fpos.y;
    return (dx * dx + dy * dy) >= (_start_slop * _start_slop);
}

auto Drag::reset() -> void
{
    _fpos = std::nullopt;
    _tag = std::nullopt;
    _initiated = false;
}

} // namespace tiny::gesture