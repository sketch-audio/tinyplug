#pragma once

#include <cassert>
#include <concepts>
#include <optional>
#include <span>

#include "tiny_events.hpp"
#include "tiny_utils.hpp"

namespace tiny {

inline auto frames_to_beats(int64_t frames, double tempo, double sample_rate) noexcept -> double
{
    assert(sample_rate > 0 && "Sample rate must be greater than zero.");
    return static_cast<double>(frames) * tempo / (60 * sample_rate);
}

struct Transport_state {
    bool moving{};
    bool cycling{};
    bool recording{};
};

struct Time_sig {
    int32_t numer{4};
    int32_t denom{4};
};

// TODO: consider whether fields like beat_pos, cycle_start/end, tempo should be optional for hosts that don't provide them.
struct Musical_context {
    int64_t sample_pos{};
    double beat_pos{};
    double cycle_start{}; // cycle start, end in beats
    double cycle_end{};
    double tempo_ideal{120};
    double tempo_real{tempo_ideal};
    Time_sig time_sig{};
    Transport_state transport_state{};
};

// Whether the host is rendering offline (bounce / freeze / export) rather than
// in real time. Lets a kernel switch to a higher-quality / non-realtime-safe
// path during a bounce. Best-effort: a host that never signals offline stays
// realtime.
enum class Render_mode { Realtime, Offline };

struct Dsp_context {
    Musical_context musical_context{};
    std::span<const float*> ibuffers{};
    std::span<const float*> sbuffers{};
    std::span<float*> obuffers{};
    size_t num_frames{};
    std::span<float> meters{};
    std::optional<uint32_t> propose_latency{}; // samples.
    Render_mode render_mode{Render_mode::Realtime};
};

template<typename T>
concept Some_plug_processor = requires(T t) {
    // Three tiers of state, weakest last. What is guaranteed is that each tier is
    // *individually invocable* — `clear` and `snap` never allocate and never need a
    // `reset` first — not that an implementation confines itself to exactly its own tier.
    // Doing more is allowed and common: our adapters end `reset` with `clear(); snap();`,
    // and a parameter smoother's `clear` necessarily lands it. The wrapper knows what the
    // host asked for, so it calls every tier at or below it, strongest first, and any
    // overlap is idempotent:
    //
    //   sample rate / activate        reset(sr) -> clear() -> snap()
    //   discontinuity (seek, bounce)              clear() -> snap()
    //   values without audio (flush)                         snap()
    //
    // Resources: size and allocate for this sample rate. Off the audio thread. Leaves
    // history undefined and values unmanifested, so it is never sufficient on its own.
    { t.reset(double{/*sample_rate*/}) } -> std::same_as<void>;

    // History: forget it — delay lines, filter state, oscillator phase, transport
    // position. Must not allocate, and must not touch parameter values or latency (a
    // ramp in flight is a realized *value*; landing it belongs to `snap`).
    { t.clear() } -> std::same_as<void>;

    // Values: manifest any deferred parameter changes now, with no glide.
    { t.snap() } -> std::same_as<void>;

    { t.handle_event(std::declval<const Render_event&>(/*event*/)) } -> std::same_as<void>;
    { t.process(std::declval<Dsp_context&>(/*context*/)) } -> std::same_as<void>;
    { t.latency_samps() } -> std::same_as<uint32_t>;
    { t.tail_samps() } -> std::same_as<uint32_t>;
};

}