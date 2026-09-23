#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <variant>

#include "tiny_midi.hpp"
#include "tiny_utils.hpp"

namespace tiny::midi {

// MIDI 1.0 bytes <-> the framework's notes and controls, for the formats that speak MIDI
// (AUv2, AUv3, AAX, CLAP's MIDI dialect). Notes come out named by channel + key only; the
// framework mints their ids afterwards.

// The controllers that become `Control::Pedal`. Everything else is dropped.
inline constexpr auto pedal_numbers = std::array<uint8_t, 7>{1, 2, 4, 11, 64, 66, 67}; // In `Pedal::Kind` order.
inline constexpr auto all_notes_off_number = uint8_t{123};

// CC 123 (and 120, all sound off): release every held note on the channel.
struct All_off { uint8_t channel{}; };

using Decoded = std::variant<std::monostate, midi::Note::Any, midi::Control::Any, All_off>;

struct Bytes {
    std::array<uint8_t, 3> data{};
    uint8_t size{};
};

namespace detail {

inline auto unit(uint8_t v) -> double { return static_cast<double>(v & 0x7f) / 127.; }

inline auto seven(double v) -> uint8_t
{
    return static_cast<uint8_t>(std::clamp(std::lround(v * 127.), 0l, 127l));
}

// A 14-bit bend as −1…1; 0 and 16383 are the ends.
inline auto bend(uint8_t lsb, uint8_t msb) -> double
{
    const auto raw = static_cast<double>(static_cast<int>((lsb & 0x7f) | ((msb & 0x7f) << 7)) - 8192);
    return raw < 0. ? raw / 8192. : raw / 8191.;
}

inline auto pedal_kind(uint8_t number) -> std::optional<midi::Control::Pedal::Kind>
{
    for (auto i = size_t{}; i < pedal_numbers.size(); ++i) {
        if (pedal_numbers[i] == number) return static_cast<midi::Control::Pedal::Kind>(i);
    }
    return std::nullopt;
}

} // namespace detail

// One channel message. A note-on with velocity 0 is a note-off, as the spec says.
inline auto decode(uint8_t status, uint8_t d1, uint8_t d2) -> Decoded
{
    using namespace process;
    const auto channel = static_cast<uint8_t>(status & 0x0f);
    const auto key = static_cast<uint8_t>(d1 & 0x7f);
    const auto note = Note::Id{.id = 0, .channel = channel, .key = key};

    switch (status & 0xf0) {
        case 0x80: return Note::Any{Note::Off{note, static_cast<float>(detail::unit(d2))}};
        case 0x90:
            if ((d2 & 0x7f) == 0) return Note::Any{Note::Off{note, 0.f}};
            return Note::Any{Note::On{note, static_cast<float>(detail::unit(d2))}};
        case 0xa0: return Note::Any{Note::Expression{note, Note::Expression::Kind::Pressure, detail::unit(d2)}};
        case 0xb0:
            if (key == all_notes_off_number || key == 120) return All_off{channel};
            if (const auto kind = detail::pedal_kind(key)) {
                return Control::Any{Control::Pedal{channel, *kind, detail::unit(d2)}};
            }
            return std::monostate{};
        case 0xd0: return Control::Any{Control::Pressure{channel, detail::unit(d1)}};
        case 0xe0: return Control::Any{Control::Bend{channel, detail::bend(d1, d2)}};
        default: return std::monostate{};
    }
}

inline auto encode(const midi::Note::Any& note) -> Bytes
{
    using namespace process;
    return std::visit(Inline_visitor{
        [](const Note::On& e) {
            const auto velocity = std::max<uint8_t>(detail::seven(e.velocity), 1); // 0 would read as an off.
            return Bytes{{static_cast<uint8_t>(0x90 | (e.note.channel & 0x0f)), static_cast<uint8_t>(e.note.key & 0x7f), velocity}, 3};
        },
        [](const Note::Off& e) {
            return Bytes{{static_cast<uint8_t>(0x80 | (e.note.channel & 0x0f)), static_cast<uint8_t>(e.note.key & 0x7f), detail::seven(e.velocity)}, 3};
        },
        [](const Note::Choke& e) { // MIDI has no choke: the closest is an immediate off.
            return Bytes{{static_cast<uint8_t>(0x80 | (e.note.channel & 0x0f)), static_cast<uint8_t>(e.note.key & 0x7f), 0}, 3};
        },
        [](const Note::Expression& e) {
            if (e.kind != Note::Expression::Kind::Pressure) return Bytes{}; // MIDI 1.0 carries only pressure per note.
            return Bytes{{static_cast<uint8_t>(0xa0 | (e.note.channel & 0x0f)), static_cast<uint8_t>(e.note.key & 0x7f), detail::seven(e.value)}, 3};
        },
    }, note);
}

inline auto encode(const midi::Control::Any& control) -> Bytes
{
    using namespace process;
    return std::visit(Inline_visitor{
        [](const Control::Bend& e) {
            const auto v = std::clamp(e.value, -1., 1.);
            const auto raw = std::clamp(std::lround(v < 0. ? v * 8192. : v * 8191.) + 8192, 0l, 16383l);
            return Bytes{{static_cast<uint8_t>(0xe0 | (e.channel & 0x0f)), static_cast<uint8_t>(raw & 0x7f), static_cast<uint8_t>((raw >> 7) & 0x7f)}, 3};
        },
        [](const Control::Pressure& e) {
            return Bytes{{static_cast<uint8_t>(0xd0 | (e.channel & 0x0f)), detail::seven(e.value), 0}, 2};
        },
        [](const Control::Pedal& e) {
            const auto number = pedal_numbers[static_cast<size_t>(e.kind)];
            return Bytes{{static_cast<uint8_t>(0xb0 | (e.channel & 0x0f)), number, detail::seven(e.value)}, 3};
        },
    }, control);
}

// A channel voice message as written; nothing for anything else (system messages, a data byte
// over 127), which every format then drops.
inline auto encode(const midi::Raw& raw) -> Bytes
{
    const auto status = raw.bytes[0];
    if (status < 0x80 || status >= 0xf0 || raw.bytes[1] > 0x7f || raw.bytes[2] > 0x7f) return {};
    const auto type = status & 0xf0;
    const auto size = static_cast<uint8_t>((type == 0xc0 || type == 0xd0) ? 2 : 3);
    return Bytes{{status, raw.bytes[1], size == 3 ? raw.bytes[2] : uint8_t{0}}, size};
}

inline auto all_notes_off(uint8_t channel) -> Bytes
{
    return Bytes{{static_cast<uint8_t>(0xb0 | (channel & 0x0f)), all_notes_off_number, 0}, 3};
}

} // namespace tiny::midi
