#pragma once

#include "tinyplug/tinyplug.hpp"

namespace tiny::models {

// One meter per policy, plus the two Stream cases that behave differently in
// practice: one that moves every block and one that never moves at all. The
// constant is the interesting one — it is transmitted once and never again, so it
// is the direct test of whether a freshly opened window learns anything from a
// mailbox that retains levels rather than from a resync that no longer exists.
struct Meters {
    enum class Address : uint32_t {
        peak_in,        // Peak   — resets each block, so silence reads 0.
        stream_lfo,     // Stream — written every block, tracks a 0.5 Hz ramp.
        stream_const,   // Stream — the sample rate. Written every block but never moves.
        stream_sparse,  // Stream — written only while signal is present. Must HOLD when it stops.
        trig_pulse,     // Trig   — one event per second, magnitude counts up 1..8.
        Num_meters
    };

    static auto make_spec(Address address) -> meters::Spec
    {
        using namespace meters;
        switch (address) {
            case Address::peak_in:
                return {.range = Range{0, 1}, .policy = Policy::Peak};
            case Address::stream_lfo:
                return {.range = Range{0, 1}, .policy = Policy::Stream};
            case Address::stream_const:
                return {.range = Range{0, 192000}, .policy = Policy::Stream};
            case Address::stream_sparse:
                return {.range = Range{0, 1}, .policy = Policy::Stream};
            case Address::trig_pulse:
                return {.range = Range{0, 8}, .policy = Policy::Trig};
            case Address::Num_meters:
            default:
                return {};
        }
    }
};
static_assert(meters::Model<Meters>);

} // namespace tiny::models
