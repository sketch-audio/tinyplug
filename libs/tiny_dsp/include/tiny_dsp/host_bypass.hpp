#pragma once

#include <atomic>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "delay_line.hpp"
#include "linear_ramper.hpp"

namespace tiny {

class Host_bypass {
public:

    auto reset(float sr) -> void
    {
        for (auto& ramp : _ramps) {
            ramp.reset(sr);
        }
    }

    auto set_bypassed(bool bypassed) -> void
    {
        _bypassed.store(bypassed, std::memory_order_release);
        for (auto& ramp : _ramps) {
            ramp.set_target(bypassed ? 0.f : 1.f);
        }
    }

    auto is_bypassed() const -> bool
    {
        return _bypassed.load(std::memory_order_acquire);
    }

    auto set_latency(size_t samples) -> void
    {
        _latency = static_cast<uint32_t>(samples);
        for (auto& delay : _delays) {
            delay.resize(samples);
        }
    }

    auto process(std::span<const float*> inputs, std::span<float*> outputs, size_t num_frames) -> void
    {
        if (any_null(inputs) || any_null(outputs)) {
            return;
        }

        assert(inputs.size() == outputs.size() && "Mismatched number of input and output channels.");
        const auto num_channels = inputs.size();

        if (!_bypassed.load(std::memory_order_relaxed) && !any_ramping()) {
            // Feed delays only.
            for (size_t ch = {}; ch < num_channels; ++ch) {
                auto& delay = _delays[ch];
                const auto* in = inputs[ch];
                for (size_t fr = {}; fr < num_frames; ++fr) {
                    delay.write(in[fr]);
                }
            }
            return;
        }

        assert(num_channels <= max_channels && "Bypasser can only handle up to max_channels channels.");
        for (size_t ch = {}; ch < num_channels; ++ch) {
            auto& delay = _delays[ch];
            auto& ramp = _ramps[ch];
            const auto* in = inputs[ch];
            auto* out = outputs[ch];

            for (size_t fr = {}; fr < num_frames; ++fr) {
                delay.write(in[fr]);
                const auto dry = delay.read(_latency);
                const auto mix = ramp.process(); // Bypass
                out[fr] = (1 - mix) * dry + mix * out[fr];
            }
        }
    }

    auto can_skip_effect() const -> bool
    {
        return _bypassed.load(std::memory_order_acquire) && !any_ramping();
    }

private:

    static constexpr auto fade_ms = 20.f;

    std::atomic<bool> _bypassed{false};
    uint32_t _latency{0};

    static constexpr auto max_channels = size_t{2};
    std::array<Delay_line, max_channels> _delays{{{}, {}}};
    
    std::array<Linear_ramp, max_channels> _ramps{{
        Linear_ramp{1, fade_ms}, // initial, fade_ms
        Linear_ramp{1, fade_ms}
    }};

    auto any_ramping() const -> bool
    {
        return std::ranges::any_of(_ramps, [](const auto& ramp) { return ramp.is_ramping(); });
    }

    template<typename T>
    auto any_null(std::span<T> ptrs) const -> bool
    {
        return std::ranges::any_of(ptrs, [](auto ptr) { return ptr == nullptr; });
    }

};

} // namespace tiny