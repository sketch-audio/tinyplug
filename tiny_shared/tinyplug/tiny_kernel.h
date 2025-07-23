#pragma once

#include <concepts>
#include <span>

#include "tiny_events.h"

namespace tiny {

struct Dsp_context {
    std::span<const float*> ibuffers{};
    std::span<const float*> sbuffers{};
    std::span<float*> obuffers{};
    size_t num_frames{};
};

template<typename T>
concept Some_dsp_kernel = requires(T t) {
    { t.reset(double{/*sample_rate*/}, size_t{/*max_frames*/}) } -> std::same_as<void>;
    { t.handle_event(std::declval<const Event&>(/*event*/)) } -> std::same_as<void>;
    { t.process(std::declval<Dsp_context&>(/*context*/)) } -> std::same_as<void>;
};

}