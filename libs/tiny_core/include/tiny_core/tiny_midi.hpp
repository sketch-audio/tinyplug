#pragma once

#include <array>
#include <cstdint>
#include <variant>

namespace tiny::midi {

// Performance data: notes and a closed set of player controls. Events on the audio thread,
// never state: they don't write parameters, aren't saved, and never enter undo. What isn't
// carried (SysEx, system messages, MIDI 2.0, ...) is listed in plans/midi-support.md.

struct Note {
    // Which note. `id` is minted by the framework at `On` and carried by every later event for
    // that note, whatever the format supplied: match on it alone. `channel` and `key` are
    // pitch information, not identity. On output, ids are the processor's own.
    struct Id {
        uint32_t id{};
        uint8_t channel{};
        uint8_t key{};
    };

    struct On { Id note{}; float velocity{}; };   // 0…1
    struct Off { Id note{}; float velocity{}; };  // Release velocity, 0…1.
    struct Choke { Id note{}; };                  // Stop now, no release.

    // A per-note control.
    struct Expression {
        enum class Kind : uint8_t { Volume, Pan, Tuning, Vibrato, Brightness, Pressure };
        Id note{};
        Kind kind{};
        double value{}; // Tuning in semitones, Volume a gain (1 unchanged, up to 4), the rest 0…1.

        // Where every note starts: an `On` implies these until an `Expression` says otherwise.
        static constexpr auto neutral(Kind kind) -> double
        {
            if (kind == Kind::Volume) return 1.;
            if (kind == Kind::Pan || kind == Kind::Brightness) return 0.5;
            return 0.;
        }
    };

    using Any = std::variant<On, Off, Choke, Expression>;
};

// Whole-channel controls a player's hardware has. A closed set: not arbitrary CC, which
// would be a second, unrecorded writer for what should be host-automated parameters.
struct Control {
    struct Bend { uint8_t channel{}; double value{}; };      // −1…1
    struct Pressure { uint8_t channel{}; double value{}; };  // 0…1

    // A wheel or a pedal.
    struct Pedal {
        enum class Kind : uint8_t { Mod_wheel, Breath, Foot, Expression, Sustain, Sostenuto, Soft };
        uint8_t channel{};
        Kind kind{};
        double value{}; // 0…1; switches read >= 0.5.
    };

    using Any = std::variant<Bend, Pressure, Pedal>;
};

// One MIDI 1.0 channel message, sent exactly as written: any CC, program change, whatever a
// device downstream expects. Output only. Sending MIDI can't write the plug-in's own
// parameters, so the reason controls are a closed set on the way in doesn't apply on the way
// out. A malformed one (not a channel voice status, a data byte over 127) is dropped.
struct Raw {
    std::array<uint8_t, 3> bytes{};

    static constexpr auto cc(uint8_t channel, uint8_t number, uint8_t value) -> Raw
    {
        return {{static_cast<uint8_t>(0xb0 | (channel & 0x0f)), number, value}};
    }

    static constexpr auto program(uint8_t channel, uint8_t program) -> Raw
    {
        return {{static_cast<uint8_t>(0xc0 | (channel & 0x0f)), program, 0}};
    }
};

// What the editor can send the processor: notes and controls.
using Performance = std::variant<Note::Any, Control::Any>;

// What the processor can send the host: those, and raw messages.
using Outgoing = std::variant<Note::Any, Control::Any, Raw>;

} // namespace tiny::midi

namespace tiny::process {

// The process side's vocabulary, so processors write `Note::On` unqualified.
using midi::Control;
using midi::Note;
using midi::Outgoing;
using midi::Performance;

} // namespace tiny::process
