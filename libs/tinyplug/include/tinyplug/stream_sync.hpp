#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <variant>

#include "tiny_process.hpp"

namespace tiny::process {

// When a wrapper restarts its processor. Each block the wrapper reports facts; the helper says
// whether to issue a `Reset::Restart` and whether to hold the processor back. One copy of the rule
// for every format: a restart wherever the processor's input stream is discontinuous (a host
// reset, the render-mode edge, a fresh `configure`, resuming after the bypass skip), and nowhere
// else. Render thread only.
//
// The hold makes offline renders reproducible. A render can open with pre-roll at negative
// timeline positions, for which a host sends no automation while its store still holds the last
// locate's values (Logic does, from the origin). After a discontinuity in an offline render the
// processor is held back, silent, until the first block reaching position 0, and restarted there
// with values the host has actually sent. Real content ends a hold at once (a non-zero input
// sample, a note), so a hold never deletes material; so do a missing position, leaving offline,
// and a cap of one second. See plans/bounce-param-authority.md.
class Stream_sync {
public:

    enum class Restart : std::uint8_t {
        None,
        Stream, // A discontinuity: restart the processor, clear notes, clear and snap the bypass.
        Resume, // After the bypass skip: restart the processor only. The bypass is mid-crossfade.
    };

    struct Plan {
        Restart restart{};
        bool hold{}; // Don't run the processor this block: silence out, its events dropped.
    };

    struct Facts {
        bool host_reset{};                      // The host asked for a reset since the last block.
        Mode mode{};
        bool skipping{};                        // `Host_bypass::can_skip_effect()`, before any restart. Ask again after one: it snaps the bypass.
        std::optional<std::int64_t> position{}; // The host's timeline position at the block's first frame, if it gave one.
        std::uint32_t frames{};
    };

    // `hold_offline`: whether this format holds offline renders. Enabled per format, on evidence.
    explicit Stream_sync(bool hold_offline = false) : _hold_offline{hold_offline} {}

    // After every `configure`, before the next block. A fresh processor is a discontinuity too:
    // offline, that's what lets a render that opens a fresh instance hold.
    auto configured(double sr) -> void
    {
        _max_hold = static_cast<std::int64_t>(sr);
        _edge = true;
        _mode.reset();
        _holding = false;
        _was_skipped = false;
    }

    // `has_content()` says whether this block carries real material (any non-zero input or
    // sidechain sample, a note). Called only while holding.
    template<typename Has_content>
    auto next(const Facts& facts, Has_content&& has_content) -> Plan
    {
        const auto mode_changed = _mode.has_value() && *_mode != facts.mode;
        _mode = facts.mode;
        const auto offline = facts.mode == Mode::Offline;

        if (std::exchange(_edge, false) || facts.host_reset || mode_changed) {
            _pending = true;
            if (!_holding) _held = 0; // A discontinuity inside a hold extends it, never its cap.
            _holding = _hold_offline && offline;
        }

        if (_holding) {
            const auto reached = !facts.position || *facts.position + static_cast<std::int64_t>(facts.frames) > 0;
            if (!offline || reached || _held >= _max_hold || has_content()) _holding = false;
            else {
                _held += facts.frames;
                return {.hold = true};
            }
        }

        // A stream restart snaps the bypass, so whether this block skips is only known after it: the
        // next block's facts say. Until then the processor is fresh, not skipped.
        if (std::exchange(_pending, false)) {
            _was_skipped = false;
            return {.restart = Restart::Stream};
        }
        const auto resumed = _was_skipped && !facts.skipping;
        _was_skipped = facts.skipping;
        return {.restart = resumed ? Restart::Resume : Restart::None};
    }

    auto holding() const -> bool { return _holding; }

private:

    bool _hold_offline{};
    bool _edge{true};
    bool _pending{};
    bool _holding{};
    bool _was_skipped{};
    std::optional<Mode> _mode{};
    std::int64_t _held{};
    std::int64_t _max_hold{48000};

};

// The values a `Restart` hands over: the host's store at the top of the block (`plain_at(address)`,
// plain space), with this block's offset-0 `Set`s folded in. A `Ramp` carries only its target, so it
// stays an event. A stack array, so no wrapper needs member state for it.
template<std::size_t N, typename Plain_at>
auto restart_values(Plain_at&& plain_at, std::span<const Event::Ordered> events) -> std::array<double, N>
{
    auto values = std::array<double, N>{};
    for (auto address = std::size_t{}; address < N; ++address) values[address] = plain_at(static_cast<std::uint32_t>(address));
    for (const auto& ordered : events) {
        if (ordered.offset > 0) continue;
        if (const auto* param = std::get_if<Event::Param>(&ordered.event)) {
            if (const auto* set = std::get_if<Event::Set>(param); set && set->address < N) values[set->address] = set->value;
        }
    }
    return values;
}

// Whether any of `channels` holds a non-zero sample in its first `frames`.
inline auto any_signal(std::span<const float* const> channels, std::size_t frames) -> bool
{
    for (const auto* channel : channels) {
        if (!channel) continue;
        for (auto frame = std::size_t{}; frame < frames; ++frame) {
            if (channel[frame] != 0.f) return true;
        }
    }
    return false;
}

// Whether a block's events carry a note.
inline auto any_note(std::span<const Event::Ordered> events) -> bool
{
    for (const auto& ordered : events) {
        if (std::holds_alternative<Note::Any>(ordered.event)) return true;
    }
    return false;
}

} // namespace tiny::process
