#pragma once

#include <cassert>
#include <concepts>
#include <optional>
#include <span>

#include "tiny_events.h"
#include "tiny_utils.h"

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

struct Dsp_context {
    Musical_context musical_context{};
    std::span<const float*> ibuffers{};
    std::span<const float*> sbuffers{};
    std::span<float*> obuffers{};
    size_t num_frames{};
    std::span<float> exports{};
    std::optional<uint32_t> propose_latency{}; // samples.
};

template<typename T>
concept Some_dsp_kernel = requires(T t) {
    { t.reset(double{/*sample_rate*/}) } -> std::same_as<void>;
    { t.handle_event(std::declval<const Render_event&>(/*event*/)) } -> std::same_as<void>;
    { t.process(std::declval<Dsp_context&>(/*context*/)) } -> std::same_as<void>;
    { t.latency_samps() } -> std::same_as<uint32_t>;
};

}