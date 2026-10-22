#include "processor.hpp"

#include <algorithm> // std::max

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    // Come up holding the host's values: the first block renders from this state.
    _adopt(config.params);
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Restart& r) { _adopt(r.params); },
        [](const Reset::Latency&) {}
    }, reset);
}

// Everything `configure` and `Restart` share, so the two can't drift apart. Never allocates.
auto Processor::_adopt(std::span<const double> params) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) _values[i] = static_cast<float>(params[i]);
}

auto Processor::handle(const Event::Param& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) {
            _values[e.address] = static_cast<float>(e.value);
        },
        [this](const Event::Ramp& e) {
            _values[e.address] = static_cast<float>(e.value); // You might want to handle this differently.
        },
        [this](const auto&) { /* Handle other events as needed. */ }
    }, event);
}

auto Processor::process(Context& context) -> void
{
    const auto g = _values[enum_raw(Address::Gain)];
    
    for (size_t channel = 0; channel < context.inputs.size(); ++channel) {
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.inputs[channel][frame];
            const auto output = g * input;
            context.outputs[channel][frame] = output;          
        }
    }
}

} // namespace tiny::process
