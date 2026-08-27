#include "processor.hpp"

#include <algorithm> // std::max

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    _ramper.reset(config.sr);
    _ramper.set_immediate(static_cast<float>(config.params[enum_raw(Address::Gain)]));
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&) { _ramper.settle(); },
        [this](const Reset::Soft&) { _ramper.settle(); },
        [](const Reset::Latency&) {}
    }, reset);
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) {
            if (e.address == enum_raw(Address::Gain)) {
                _ramper.set_immediate(static_cast<float>(e.value));
            }
        },
        [this](const Event::Ramp& e) {
            if (e.address == enum_raw(Address::Gain)) {
                _ramper.set_ramp(static_cast<float>(e.target), e.dur_samples);
            }
        }
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    const auto id = enum_raw(Address::Gain);
    
    for (size_t frame = 0; frame < context.num_frames; ++frame) {
        _values[id] = _ramper.process();
        const auto g = _values[id];

        for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = g; // !!!
            context.obuffers[channel][frame] = output;
        }
    }
}

} // namespace tiny::process
