#include "processor.hpp"

#include <algorithm>
#include <cmath>

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) {
        _values[i] = static_cast<float>(config.params[i]);
    }
    _sr = config.sr;
    _glide = static_cast<float>(1. - std::exp(-1. / (0.003 * _sr)));
    _free_beats = 0.;
    _gain = 1.f;
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&) { _free_beats = 0.; },
        [](const Reset::Soft&) {},
        [](const Reset::Latency&) {}
    }, reset);
}

auto Processor::handle(const Event::Param& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _values[e.address] = static_cast<float>(e.value); },
        [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.value); },
        [](const auto&) {}
    }, event);
}

auto Processor::process(Context& context) -> void
{
    const auto& pattern = context.state.get(); // Valid for this block only.
    const auto depth = _values[enum_raw(Address::Depth)];
    const auto& timeline = context.timeline;
    const auto beats_per_sample = timeline.tempo.value_or(120.) / (60. * _sr);
    const auto moving = timeline.state && timeline.state->moving && timeline.beat_pos;
    const auto start = moving ? *timeline.beat_pos : _free_beats;
    const auto channels = std::min(context.inputs.size(), context.outputs.size());

    auto step = size_t{};
    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        const auto beat = start + static_cast<double>(frame) * beats_per_sample;
        const auto sixteenth = static_cast<int64_t>(std::floor(std::max(beat, 0.) * 4.));
        step = static_cast<size_t>(sixteenth % static_cast<int64_t>(num_steps));

        const auto level = static_cast<float>(pattern.level[step]) / 100.f;
        const auto target = 1.f - depth * (1.f - level);
        _gain += _glide * (target - _gain);

        for (auto channel = size_t{}; channel < channels; ++channel) {
            context.outputs[channel][frame] = _gain * context.inputs[channel][frame];
        }
    }

    if (!moving) _free_beats = start + static_cast<double>(context.num_frames) * beats_per_sample;
    if (context.num_frames > 0) context.meters[enum_raw(Meter::Step)] = static_cast<float>(step);
}

} // namespace tiny::process
