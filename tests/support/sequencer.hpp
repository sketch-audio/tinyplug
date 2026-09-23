#pragma once

#include <cstdint>
#include <type_traits>

namespace demo {

// A plausible worst-ish case: 8 tracks x 64 steps x 8-voice poly x 9 modifiers.
// Laid out with no padding so byte comparison never sees phantom differences.

struct Note {
    uint8_t key{};       // 0 = empty
    uint8_t vel{};
};

struct Mod {
    uint8_t rule{};      // Rule enum
    uint8_t active{};
    int16_t value{};
};

struct Step {
    Note notes[8]{};     // 16
    Mod mods[9]{};       // 36
};                       // 52, alignof 2

struct Track {
    Step steps[64]{};    // 3328
};

struct Pattern {
    Track tracks[8]{};   // 26624
    uint8_t length[8]{}; // 8
};                       // 26632

static_assert(sizeof(Step) == 52);
static_assert(sizeof(Track) == 3328);
static_assert(sizeof(Pattern) == 26632);
static_assert(std::is_trivially_copyable_v<Pattern>);
static_assert(std::has_unique_object_representations_v<Pattern>); // no padding

} // namespace demo
