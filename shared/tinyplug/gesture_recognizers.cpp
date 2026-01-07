#include "gesture_recognizers.hpp"

#include <chrono>
#include <variant>

namespace tiny {

// MARK: - Over

auto Over_recognizer::set_frame(const Frame& frame) -> void
{
    _frame = frame;

    if (_over) {
        _callbacks.on_cancelled();
        _over = false;
    }
}

auto Over_recognizer::process_events(Event_list& events) -> void
{
    for (const auto& event : events.events) {
        if (event.consumed) continue;
        std::visit(Inline_visitor{
            [&](const Pointer_move& move) {
                const auto was_over = _over;
                const auto now_over = _frame.contains(move.pos);
                resolve_events(move.pos, was_over, now_over);
            },
            [&](const Pointer_enter& enter) {
                const auto was_over = _over;
                const auto now_over = _frame.contains(enter.pos);
                resolve_events(enter.pos, was_over, now_over);
            },
            [&](const Pointer_exit& exit) {
                resolve_events(exit.pos, _over, false);
            },
            [](const auto&) {}
        }, event.event);
    }
}

auto Over_recognizer::resolve_events(Coords pos, bool was_over, bool now_over) -> void
{
    if (!was_over && now_over) {
        _callbacks.on_started({pos, true});
    }
    else if (was_over && !now_over) {
        _callbacks.on_ended({pos, false});
    }
    _over = now_over;
}

// MARK: - Down

auto Down_recognizer::set_frame(const Frame& frame) -> void
{
    _frame = frame;

    if (_down) {
        _callbacks.on_cancelled();
        _down = false;
    }
}

auto Down_recognizer::process_events(Event_list& events) -> void
{
    for (const auto& event : events.events) {
        if (event.consumed) continue;
        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                if (_frame.contains(down.pos)) {
                    _callbacks.on_started({down.pos, true});
                    _down = true;
                }
            },
            [&](const Pointer_up& up) {
                if (_down) { // Event could have left our frame
                    _callbacks.on_ended({up.pos, false});
                    _down = false;
                }
            },
            [](const auto&) {}
        }, event.event);
    }
}

// MARK: - Dwell

auto Dwell_recognizer::set_frame(const Frame& frame) -> void
{
    _frame = frame;

    if (_dwelling) {
        _callbacks.on_cancelled();
        _dwelling = false;
    }
    _down = false;
    _over_t = std::nullopt;
}

auto Dwell_recognizer::process_events(Event_list& events) -> void
{
    for (const auto& event : events.events) {
        // Track down before consumed.
        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                _down = true; // Global down tracking.
            },
            [&](const Pointer_up&) {
                _down = false;
            },
            [&](const auto&) {}
        }, event.event);

        if (event.consumed) continue;

        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                if (_dwelling) {
                    end_dwell(down.pos);
                    _over_t = std::nullopt;
                }
            },
            [&](const Pointer_up&) {
                //
            },
            [&](const Pointer_move& move) {
                if (_down) {
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

auto Dwell_recognizer::end_dwell(Coords pos) -> void
{
    _callbacks.on_ended({pos, false});
    _dwelling = false;
}

// MARK: - Click

auto Click_recognizer::set_frame(const Frame& frame) -> void
{
    _frame = frame;
}

auto Click_recognizer::process_events(Event_list& events) -> void
{
    for (auto& event : events.events) {
        if (event.consumed) continue;
        std::visit(Inline_visitor{
#if PLATFORM_APPLE
            [&](const Pointer_down& down) {
                // Right click executes on pointer down on macOS.
                if (_frame.contains(down.pos) && down.button == Pointer_button::right) {
                    const auto match = (_desc.button == down.button && _desc.count == 1);
                    if (match) _callbacks.on_started({down.pos});
                }
                if (_desc.greedy) {
                    event.consumed = true;
                }
            },
#endif
            [&](const Pointer_click& click) {
                if (_frame.contains(click.pos)) {
                    const auto match = (_desc.button == click.button && _desc.count == click.count);
                    if (match) _callbacks.on_started({click.pos});
                }
                if (_desc.greedy) {
                    event.consumed = true;
                }
            },
            [](const auto&) {}
        }, event.event);
    }
}

// MARK: - Drag

auto Drag_recognizer::set_frame(const Frame& frame) -> void
{
    _frame = frame;

    if (_fpos.has_value() || _tag.has_value() || _initiated) {
        _callbacks.on_cancelled();
        reset();
    }
}

auto Drag_recognizer::process_events(Event_list& events) -> void
{
    for (auto& event : events.events) {
        if (event.consumed) continue;
        if (_tag && *_tag != event.pointer_tag) continue; // Skip events for unbound pointers

        std::visit(Inline_visitor{
            [&](const Pointer_down& down) {
                if (down.button != Pointer_button::left) return; // Only primary button drags.
                if (_frame.contains(down.pos)) {
                    // set
                    _fpos = down.pos;
                    _tag = event.pointer_tag;
                    _initiated = false;

                    event.consumed = _greedy ? true : event.consumed;
                }
            },
            [&](const Pointer_up& up) {
                if (_fpos && _initiated) {
                    _callbacks.on_ended({*_fpos, up.pos});
                    event.consumed = _greedy ? true : event.consumed;
                }
                reset();
            },
            [&](const Pointer_move& move) {
                if (_fpos) {
                    if (!_initiated) {
                        _callbacks.on_started({*_fpos, move.pos});
                        _initiated = true;
                    }
                    else {
                        _callbacks.on_updated({*_fpos, move.pos});
                    }
                    event.consumed = _greedy ? true : event.consumed;
                }
            },
            [](const auto&) {}
        }, event.event);
    }
}

auto Drag_recognizer::reset() -> void
{
    _fpos = std::nullopt;
    _tag = std::nullopt;
    _initiated = false;
}

} // namespace tiny