#include "processor.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) {
        _values[i] = static_cast<float>(config.params[i]);
    }
    _sr = static_cast<float>(config.sr);

    // Hann window, normalised so a full-scale sine reads 0 dBFS.
    auto sum = 0.f;
    for (auto i = size_t{}; i < fft_size; ++i) {
        const auto phase = 2. * std::numbers::pi * static_cast<double>(i) / static_cast<double>(fft_size);
        _window[i] = static_cast<float>(0.5 - 0.5 * std::cos(phase));
        sum += _window[i];
    }
    _window_gain = 2.f / sum;

    _clear();
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&) { _clear(); },
        [](const Reset::Soft&) {},
        [](const Reset::Latency&) {}
    }, reset);
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _values[e.address] = static_cast<float>(e.value); },
        [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.target); },
        [](const auto&) {}
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    const auto g = _values[enum_raw(Address::Gain)];
    const auto channels = std::min(context.ibuffers.size(), context.obuffers.size());
    const auto scale = channels > 0 ? 1.f / static_cast<float>(channels) : 0.f;

    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        auto mono = 0.f;
        for (auto channel = size_t{}; channel < channels; ++channel) {
            const auto output = g * context.ibuffers[channel][frame];
            context.obuffers[channel][frame] = output;
            mono += output;
        }
        mono *= scale;

        _history[_write] = mono;
        _write = (_write + 1) % fft_size;
        if (++_since_hop == hop) {
            _since_hop = 0;
            _analyze(context);
        }

        _scope(context, mono);
    }
}

auto Processor::_clear() -> void
{
    _history.fill(0.f);
    _write = 0;
    _since_hop = 0;
    _sweep_len = 0;
    _waited = 0;
    _previous = 0.f;
    _capturing = false;
}

auto Processor::_analyze(Dsp_context& context) -> void
{
    // Oldest sample first, windowed.
    for (auto i = size_t{}; i < fft_size; ++i) {
        const auto x = _history[(_write + i) % fft_size];
        _work[i] = {x * _window[i], 0.f};
    }
    _fft.forward(_work);

    auto& frame = context.blocks.write<Block::Spectrum>();
    for (auto k = size_t{}; k < fft_size / 2; ++k) {
        const auto magnitude = std::abs(_work[k]) * _window_gain;
        frame.db[k] = 20.f * std::log10(std::max(magnitude, 1e-9f));
    }
    frame.used = static_cast<std::uint32_t>(fft_size / 2);
    frame.fft_size = static_cast<std::uint32_t>(fft_size);
    frame.sample_rate = _sr;
    context.blocks.publish<Block::Spectrum>();
}

auto Processor::_scope(Dsp_context& context, float x) -> void
{
    // Arm on a rising zero crossing; free-run after 50 ms so silence and DC still draw.
    if (!_capturing) {
        const auto rising = _previous < 0.f && x >= 0.f;
        const auto patience = static_cast<size_t>(_sr * 0.05f);
        if (rising || ++_waited >= patience) {
            _capturing = true;
            _triggered = rising;
            _sweep_len = 0;
            _waited = 0;
        }
    }
    _previous = x;

    if (!_capturing) return;

    _sweep[_sweep_len++] = x;
    if (_sweep_len < sweep_size) return;

    auto& frame = context.blocks.write<Block::Scope>();
    frame.samples = _sweep;
    frame.used = static_cast<std::uint32_t>(sweep_size);
    frame.triggered = _triggered;
    context.blocks.publish<Block::Scope>();
    _capturing = false;
}

} // namespace tiny::process
