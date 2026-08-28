#include "processor.hpp"

#include <cmath>

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) {
        _values[i] = static_cast<float>(config.params[i]);
    }
    _sr = static_cast<float>(config.sr);
    _pos = 0;
    _next_trig = static_cast<int64_t>(_sr); // First pulse one second in.
    _trig_count = 0;
}

auto Processor::reset(const Reset::Any&) -> void
{
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _values[e.address] = static_cast<float>(e.value); },
        [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.target); },
        [this](const auto&) {}
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    const auto g = _values[enum_raw(Address::Gain)];

    auto block_peak = 0.f;
    for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.ibuffers[channel][frame];
            block_peak = std::max(block_peak, std::abs(input));
            context.obuffers[channel][frame] = g * input;
        }
    }

    if (context.meters.empty()) return;

    // Peak — the measurement for *this* block. The publisher clears it afterwards,
    // so a silent block reports 0 without us writing one.
    context.meters[enum_raw(Meter::peak_in)] = block_peak;

    // Stream, moving — a 0.5 Hz ramp. Written every block.
    const auto period = static_cast<double>(_sr) * 2.;
    const auto phase = std::fmod(static_cast<double>(_pos), period) / period;
    context.meters[enum_raw(Meter::stream_lfo)] = static_cast<float>(phase);

    // Stream, constant — never moves after the first block, so it only reaches a
    // newly opened window if that window's attach triggered a resync.
    context.meters[enum_raw(Meter::stream_const)] = _sr;

    // Stream, sparse — a ramp that only advances while signal is present, and is
    // written ONLY on those blocks. Deliberately not a function of amplitude: if it
    // tracked the signal it would decay towards zero along with it, and "held" would
    // be indistinguishable from "collapsed". A ramp freezes mid-travel instead, which
    // is unmistakable. Stop the audio and this bar must stay exactly where it is.
    if (block_peak > 0.001f) {
        _sparse_ramp += static_cast<float>(context.num_frames) / (_sr * 3.f); // ~3 s sweep
        if (_sparse_ramp > 1.f) _sparse_ramp -= 1.f;
        context.meters[enum_raw(Meter::stream_sparse)] = _sparse_ramp;
    }

    // Trig — one event per second, carrying a magnitude that counts 1..8 and wraps.
    // The count is what makes double-fires and swallowed repeats visible: the editor
    // should see every number in order, once each.
    const auto block_end = _pos + static_cast<int64_t>(context.num_frames);
    if (block_end >= _next_trig) {
        _trig_count = (_trig_count % 8) + 1;
        context.meters[enum_raw(Meter::trig_pulse)] = static_cast<float>(_trig_count);
        _next_trig += static_cast<int64_t>(_sr);
    }

    _pos = block_end;
}

} // namespace tiny::process
