#include "processor.hpp"

#include <algorithm>
#include <cmath>

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    _gain.set_target(static_cast<float>(config.params[enum_raw(Address::Gain)]));
    _gain.reset(static_cast<float>(config.sr)); // Lands on the target: no glide up from the default.
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&) { _gain.snap(); },
        [this](const Reset::Soft&) { _gain.snap(); },
        [](const Reset::Latency&) {},
    }, reset);
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _gain.set_target(static_cast<float>(e.value)); },
        [this](const Event::Ramp& e) { _gain.set_target(static_cast<float>(e.target)); }, // The ramp's own smoothing does the gliding.
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    auto peaks = std::array<float, 4>{};
    const auto channels = std::min(context.ibuffers.size(), context.obuffers.size());
    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        const auto g = _gain.process();
        for (auto channel = size_t{}; channel < channels; ++channel) {
            const auto in = context.ibuffers[channel][frame];
            const auto out = g * in;
            context.obuffers[channel][frame] = out;
            const auto side = std::min<size_t>(channel, 1);
            peaks[side] = std::max(peaks[side], std::abs(in));
            peaks[2 + side] = std::max(peaks[2 + side], std::abs(out));
        }
    }
    if (channels == 1) { peaks[1] = peaks[0]; peaks[3] = peaks[2]; } // Mono: both bars.

    for (auto i = size_t{}; i < peaks.size(); ++i) context.meters[i] = peaks[i];
}

} // namespace tiny::process
