#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

#include "tiny_meters.hpp"

namespace tiny::meters {

// The single implementation of "what does the processor say about a meter, and
// when". Every format wrapper drives this instead of hand-rolling the loop, because
// five hand-rolled copies is exactly how the policies drifted apart.
//
// The scratch buffer handed to `process()` as `context.meters` is owned here. What
// survives the end of a block is the whole design:
//
//   Stream  a level. Persists. A block that does not write it (process skipped for
//           host bypass, auto-bypass, a flush, or a write sitting behind an `if`)
//           leaves the last value standing, which is what "latest value" has to
//           mean. Previously every wrapper zeroed the whole buffer every block, so
//           any un-written meter collapsed to 0 and stayed there.
//   Peak    a measurement of this block. Reset, so a silent block reads 0.
//   Trig    an event. Reset, and never deduplicated: comparing against a shadow
//           would swallow a repeated trigger, and the trailing zero from the reset
//           would be sent as a second, phantom one.
//
// Only `Stream` is deduplicated, and only because a constant should not spend a slot
// every block. `Peak` deliberately is not: downstream is a mailbox the reader *clears*
// when it looks, so an unchanged peak still has to be restated or a steady signal
// would read as silence. Silence is instead announced once, on the falling edge, and
// then we go quiet — which keeps an idle plug-in idle without leaving the reader
// holding a value the signal no longer has.
template<typename User_meters>
class Publisher {
public:

    static constexpr auto num_meters = User_meters::num_meters;

    // Hand this to `Dsp_context::meters`. Valid for the life of the publisher.
    auto scratch() -> std::span<float> { return {_scratch.data(), _scratch.size()}; }

    // Once per process block, after `process()` ran — or didn't; a skipped block is
    // a meaningful state, not a reason to stay quiet.
    //
    // `suppress` transmits nothing while still resetting peaks and events, so a
    // quiet stretch cannot accumulate a spike that flushes as one absurd value when
    // it ends. Two conditions want it: an offline bounce (Live corrupts its heap
    // ingesting output-parameter meters during one) and a block that rendered no
    // audio, which has made no measurement and so must not assert one.
    //
    // `send(address, value) -> bool` reports whether the transport accepted the
    // value. False is respected: nothing is recorded as delivered, so the update is
    // retried next block rather than lost until the value happens to move again.
    //
    // [audio thread]
    template<typename Send>
    auto publish(bool suppress, Send&& send) -> void
    {
        for (auto i = uint32_t{}; i < num_meters; ++i) {
            const auto policy = User_meters::spec(i).policy;

            if (!suppress) {
                switch (policy) {
                    case Policy::Trig:   _publish_event(i, send); break;
                    case Policy::Peak:   _publish_peak(i, send); break;
                    case Policy::Stream: _publish_level(i, send); break;
                    default: break;
                }
            }

            // Levels carry over; measurements and events do not. This runs even when
            // suppressed, which is what stops a quiet stretch from hoarding a spike.
            if (policy != Policy::Stream) _scratch[i] = 0.f;
        }
    }

private:

    std::array<float, num_meters> _scratch{}; // Written by the DSP each block.
    std::array<float, num_meters> _shadow{};  // Last value delivered (Stream), or
                                              // last peak sent, to spot the edge to zero.
    std::array<float, num_meters> _pending{}; // Peak maxima not yet delivered.

    template<typename Send>
    auto _publish_event(uint32_t address, Send& send) -> void
    {
        // Best effort: a trigger that fires while nothing is draining the transport
        // is dropped rather than delivered late. Nobody was watching, and a stale
        // trigger is worse than a missed one.
        const auto value = _scratch[address];
        if (value != 0.f) send(address, value);
    }

    template<typename Send>
    auto _publish_peak(uint32_t address, Send& send) -> void
    {
        // Hold the maximum across blocks the transport refused, so a stall costs
        // latency rather than the transient itself.
        auto& pending = _pending[address];
        pending = std::max(pending, _scratch[address]);

        // Silence is announced once, on the falling edge, and then we go quiet. The
        // reader holds its last delivered value when nothing arrives — that is what
        // stops a slow transport from flickering — so it needs to be told the signal
        // actually stopped. `_shadow` doubles as "was the last thing we sent a zero".
        if (pending == 0.f && _shadow[address] == 0.f) return;

        if (send(address, pending)) {
            _shadow[address] = pending;
            pending = 0.f;
        }
    }

    template<typename Send>
    auto _publish_level(uint32_t address, Send& send) -> void
    {
        const auto value = _scratch[address];
        if (value == _shadow[address]) return;
        if (send(address, value)) _shadow[address] = value;
    }

};

} // namespace tiny::meters
