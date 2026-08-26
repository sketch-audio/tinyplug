#include "processor.hpp"

#include <algorithm> // std::max

namespace tiny::plugin {

auto Processor::configure(const Config& config) -> void
{
    // Come up holding the host's values: the first block renders from this state.
    for (auto i = size_t{}; i < num_params; ++i) {
        _values[i] = static_cast<float>(config.params[i]);
    }
}

auto Processor::handle_event(const Render_event& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Set_param& e) {
            _values[e.address] = static_cast<float>(e.value);
        },
        [this](const Ramp_param& e) {
            _values[e.address] = static_cast<float>(e.target); // You might want to handle this differently.
        },
        [this](const auto&) { /* Handle other events as needed. */ }
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    const auto g = _values[enum_raw(Address::Gain)];
    
    for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = g * input;
            context.obuffers[channel][frame] = output;          
        }
    }
}

} // namespace tiny::plugin
