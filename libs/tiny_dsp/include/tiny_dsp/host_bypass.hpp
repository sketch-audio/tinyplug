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
        _sr = sr;
        clear();
        snap();
    }

    // The delay lines hold real dry audio at plug-in latency. On a discontinuity that
    // audio belongs to the other side of the seek, so a bypassed (or fading) plug-in
    // would emit pre-seek signal for a whole latency's worth of samples.
    auto clear() -> void
    {
        for (auto& delay : _delays) {
            delay.clear();
        }
    }

    // A discontinuity is a hard boundary: adopt the requested state outright rather than
    // fading from whatever the last block left behind. `set_target` first so the ramp has
    // a target to jump `_value` to; `reset` clears the ramp either way.
    auto snap() -> void
    {
        _committed = _bypassed.load(std::memory_order_acquire);
        for (auto& ramp : _ramps) {
            ramp.set_target(_committed ? 0.f : 1.f);
            ramp.reset(_sr);
        }
    }

    /**
     * Records the requested state. Callable from any thread — it touches nothing but
     * the atomic. The ramps adopt it at the next block boundary (see `process`), so
     * the bypass state a block is rendered with cannot change underneath that block.
     */
    auto set_bypassed(bool bypassed) -> void
    {
        _bypassed.store(bypassed, std::memory_order_release);
    }

    // What the host last asked for, which is what state save and host queries want.
    // The state the current block is *rendering* with is `can_skip_effect`.
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

    /**
     * Call once per block, after the caller has run (or skipped) the kernel over the
     * same buffers. Any bypass change requested during the block is adopted on the way
     * out, so the next block's `can_skip_effect` and `process` agree with each other.
     */
    auto process(std::span<const float*> inputs, std::span<float*> outputs, size_t num_frames) -> void
    {
        this->_process_block(inputs, outputs, num_frames);
        this->_commit_requested();
    }

    /**
     * Whether the caller may skip the kernel for the block it is about to render.
     *
     * This is a pure read of the committed state, and the committed state only moves at
     * a block boundary, so the answer stays true for the whole block: `process` below
     * takes the matching branch by construction. That is what makes it safe for the
     * skipping caller to leave the output buffer unwritten — nothing that happens during
     * the block can send `process` down the cross-fade path, which is the one branch
     * that reads the output back.
     */
    auto can_skip_effect() const -> bool
    {
        return _committed && !any_ramping();
    }

private:

    static constexpr auto fade_ms = 20.f;

    // Requested by the host, on any thread. `_committed` is what the render thread is
    // actually rendering with; it catches up at block boundaries only.
    std::atomic<bool> _bypassed{false};
    bool _committed{false};

    float _sr{48000};
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

    // Render thread, at the end of a block. Starting the fade here rather than in
    // `set_bypassed` is what keeps a block's bypass state fixed for its whole duration,
    // and it keeps `set_bypassed` to a single atomic store instead of writing the ramps
    // from whichever thread the host happened to call on.
    auto _commit_requested() -> void
    {
        const auto requested = _bypassed.load(std::memory_order_acquire);
        if (requested == _committed) {
            return;
        }

        _committed = requested;
        for (auto& ramp : _ramps) {
            ramp.set_target(_committed ? 0.f : 1.f);
        }
    }

    auto _process_block(std::span<const float*> inputs, std::span<float*> outputs, size_t num_frames) -> void
    {
        if (any_null(inputs) || any_null(outputs)) {
            return;
        }

        assert(inputs.size() == outputs.size() && "Mismatched number of input and output channels.");
        const auto num_channels = inputs.size();

        const auto ramping = any_ramping();

        if (!_committed && !ramping) {
            // Fully wet and settled: the kernel ran and owns the output. Feed delays only.
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

        if (!ramping) {
            // Fully bypassed and settled, so `can_skip_effect` was true and the caller
            // skipped the kernel: the output holds whatever the host left there. Write
            // the dry signal without reading it back — `mix * out[fr]` would propagate a
            // NaN or an infinity intact even with `mix` at zero.
            for (size_t ch = {}; ch < num_channels; ++ch) {
                auto& delay = _delays[ch];
                const auto* in = inputs[ch];
                auto* out = outputs[ch];

                for (size_t fr = {}; fr < num_frames; ++fr) {
                    delay.write(in[fr]);
                    out[fr] = delay.read(_latency);
                }
            }
            return;
        }

        // Ramping in either direction, so `can_skip_effect` was false and the kernel ran:
        // the wet signal in `out` is real and we can cross-fade against it.
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

    template<typename T>
    auto any_null(std::span<T> ptrs) const -> bool
    {
        return std::ranges::any_of(ptrs, [](auto ptr) { return ptr == nullptr; });
    }

};

} // namespace tiny