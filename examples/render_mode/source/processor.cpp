#include "processor.hpp"

#include <cmath>
#include <numbers>

namespace tiny::plugin {

auto Processor::configure(const Config& config) -> void
{
    // Come up holding the host's values: the first block renders from this state.
    for (auto i = size_t{}; i < num_params; ++i) {
        _values[i] = static_cast<float>(config.params[i]);
    }

    _sample_rate = config.sr > 0 ? config.sr : 44100;
    _phase = 0;
    _phase_inc = 2 * std::numbers::pi * tone_hz / _sample_rate;
}

auto Processor::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            _values[e.address] = static_cast<float>(e.value);
        },
        [this](const Ramp_param& e) {
            _values[e.address] = static_cast<float>(e.target);
        },
        [this](const auto&) { /* Handle other events as needed. */ }
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    const auto level = _values[enum_raw(Param::Level)];
    const auto is_offline = context.render_mode == Render_mode::Offline;

    // White noise is the realtime-only signal; an offline render drops it,
    // leaving just the sine (the "high quality" path).
    const auto noise_gain = is_offline ? 0.0f : 0.015f;

    auto next_noise = [this]() -> float {
        // xorshift32 → float in [-1, 1).
        _rng ^= _rng << 13;
        _rng ^= _rng >> 17;
        _rng ^= _rng << 5;
        return static_cast<float>(_rng) / 2147483648.0f - 1.0f;
    };

    const auto num_channels = context.obuffers.size();
    for (size_t frame = 0; frame < context.num_frames; ++frame) {
        const auto sine = level * static_cast<float>(std::sin(_phase));
        _phase += _phase_inc;
        if (_phase >= 2 * std::numbers::pi) _phase -= 2 * std::numbers::pi;

        const auto sample = sine + noise_gain * next_noise();
        for (size_t channel = 0; channel < num_channels; ++channel) {
            context.obuffers[channel][frame] = sample;
        }
    }

    // Report render mode to the editor.
    if (!context.meters.empty())
        context.meters[enum_raw(Meter::Offline)] = is_offline ? 1.0f : 0.0f;
}

} // namespace tiny::plugin
