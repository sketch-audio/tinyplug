#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>

#include "midi_codec.hpp"
#include "tiny_midi.hpp"

namespace tiny::midi {

// MPE over MIDI 1.0: which channels carry one note each (members), and what their bend, channel
// pressure and CC 74 mean for that note. Tracks the zone layout (the configuration message,
// RPN 6), each zone's member bend range (RPN 0) and every channel's last values. Without a
// configuration message it assumes a lower zone of 15 members, since most hosts send none.
// Fixed memory, audio thread only.
class Mpe {
public:

    using Kind = Note::Expression::Kind;

    struct Expression {
        uint8_t channel{};
        Kind kind{};
        double value{};
    };

    static constexpr auto member_range = 48.; // Semitones, the MPE default.

    // Update from one channel message. Always, so the layout is right whenever MPE turns on.
    auto observe(uint8_t status, uint8_t d1, uint8_t d2) -> void
    {
        const auto ch = static_cast<uint8_t>(status & 0x0f);
        auto& c = _channels[ch];
        switch (status & 0xf0) {
            case 0xd0: c.pressure = detail::unit(d1); break;
            case 0xe0: c.bend = detail::bend(d1, d2); break;
            case 0xb0:
                switch (d1) {
                    case 101: c.rpn_msb = d2; break;
                    case 100: c.rpn_lsb = d2; break;
                    case 99: case 98: c.rpn_msb = c.rpn_lsb = 127; break; // An NRPN deselects the RPN.
                    case 6: _data_entry(ch, d2 & 0x7f, true); break;
                    case 38: _data_entry(ch, d2 & 0x7f, false); break;
                    case 74: c.timbre = detail::unit(d2); break;
                    case 121: c.bend = 0.; c.pressure = 0.; break; // Reset all controllers.
                    default: break;
                }
                break;
            default: break;
        }
    }

    auto is_member(uint8_t channel) const -> bool { return _zone_of(channel).has_value(); }

    // A member channel's message read per note, after `observe`: bend as Tuning (semitones),
    // channel pressure as Pressure, CC 74 as Brightness.
    auto expression(uint8_t status, uint8_t d1) const -> std::optional<Expression>
    {
        const auto ch = static_cast<uint8_t>(status & 0x0f);
        const auto zone = _zone_of(ch);
        if (!zone) return std::nullopt;
        const auto& c = _channels[ch];
        switch (status & 0xf0) {
            case 0xe0: return Expression{ch, Kind::Tuning, c.bend * _range[*zone]};
            case 0xd0: return Expression{ch, Kind::Pressure, c.pressure};
            case 0xb0: if (d1 == 74) return Expression{ch, Kind::Brightness, c.timbre}; break;
            default: break;
        }
        return std::nullopt;
    }

    // What a note starting on a member channel inherits, where it differs from neutral: MPE
    // senders set a channel's values before its note-on.
    template<typename F>
    auto initial(uint8_t channel, F&& emit) const -> void
    {
        const auto zone = _zone_of(channel);
        if (!zone) return;
        const auto& c = _channels[channel];
        const auto each = [&](Kind kind, double value) { if (value != Note::Expression::neutral(kind)) emit(kind, value); };
        each(Kind::Tuning, c.bend * _range[*zone]);
        each(Kind::Pressure, c.pressure);
        each(Kind::Brightness, c.timbre);
    }

    // Forget channel values; the layout and ranges are configuration and stay.
    auto clear() -> void
    {
        for (auto& c : _channels) c = Channel{.rpn_msb = c.rpn_msb, .rpn_lsb = c.rpn_lsb};
    }

private:

    enum Zone : size_t { Lower, Upper };

    struct Channel {
        uint8_t rpn_msb{127}; // 127/127: none selected.
        uint8_t rpn_lsb{127};
        double bend{};        // −1…1
        double pressure{};
        double timbre{0.5};
    };

    std::array<Channel, 16> _channels{};
    std::array<uint8_t, 2> _members{15, 0}; // Lower zone: channels 1…n. Upper: 14 down to 15 - n.
    std::array<double, 2> _range{member_range, member_range};

    auto _zone_of(uint8_t channel) const -> std::optional<Zone>
    {
        if (channel >= 1 && channel <= _members[Lower]) return Lower;
        if (_members[Upper] > 0 && channel <= 14 && channel >= 15 - _members[Upper]) return Upper;
        return std::nullopt;
    }

    auto _data_entry(uint8_t channel, uint8_t value, bool msb) -> void
    {
        const auto& c = _channels[channel];
        if (c.rpn_msb != 0) return;
        if (c.rpn_lsb == 6 && msb && (channel == 0 || channel == 15)) { // Configuration, on a manager channel.
            const auto zone = channel == 0 ? Lower : Upper;
            const auto other = channel == 0 ? Upper : Lower;
            _members[zone] = std::min<uint8_t>(value, 15);
            _members[other] = static_cast<uint8_t>(std::clamp(14 - _members[zone], 0, static_cast<int>(_members[other])));
            _range[zone] = member_range;
        }
        else if (c.rpn_lsb == 0) { // Bend sensitivity: on a member channel, it sets its zone's.
            const auto zone = _zone_of(channel);
            if (!zone) return;
            _range[*zone] = msb ? static_cast<double>(value) : std::floor(_range[*zone]) + static_cast<double>(value) / 100.;
        }
    }

};

} // namespace tiny::midi
