#include "processor.hpp"

#include <algorithm> // std::max

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    // Come up holding the host's values: the first block renders from this state.
    for (auto i = size_t{}; i < num_params; ++i) {
        _values[i] = static_cast<float>(config.params[i]);
    }

    _sr = config.sr;
    _low.reset(config.sr);
    _high.reset(config.sr);

    // Latency is final after `configure`: come up in the mode the values ask for
    // rather than negotiating up from the default on the first block.
    _curr = this->_wanted_mode();
    _wants_latency_change = false;
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        // Both delay lines are pure history, and `configure` already sized them for the
        // mode we are in, so there is nothing to restate.
        [](const Reset::Hard&) {},
        [](const Reset::Soft&) {},

        // The host has aligned its graph, so this is the moment the mode actually moves.
        // The parameter only ever states the intention — see `process`.
        [this](const Reset::Latency& e) {
            if (e.samples == _low.latency_samps()) {
                _curr = &_low;
            }
            else if (e.samples == _high.latency_samps()) {
                _curr = &_high;
            }
            else {
                assert(false && "Unexpected latency value!");
            }
        }
    }, reset);
}

auto Processor::handle(const Event::Any& event) -> void
{
    using namespace params;
    std::visit(Inline_visitor{
        [this](const Event::Set& e) {
            // Identify that we want a latency change.
            if (e.address == enum_raw(Address::Latency_mode) && e.value != _values[e.address]) {
                _wants_latency_change = true;
            }
            _values[e.address] = static_cast<float>(e.value);
        },
        [this](const Event::Ramp& e) {
            _values[e.address] = static_cast<float>(e.target); // You might want to handle this differently.
        }
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    // Propose once per parameter change, and derive the value from the *parameter* rather
    // than toggling off `_curr`. The host may not act on a proposal for a long time (Logic
    // defers until playback starts), so what matters is that every change restates the
    // intention — toggling back then supersedes the outstanding value instead of leaving a
    // stale one for the host to find. Switching still waits: `_curr` only moves on
    // `Reset::Latency`.
    if (_wants_latency_change) {
        context.propose_latency = this->_wanted_mode()->latency_samps();
        _wants_latency_change = false;
    }

    const auto g = _values[enum_raw(Address::Gain)];

    for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
        const auto left = (channel == 0);
        for (size_t frame = 0; frame < context.num_frames; ++frame) {
            const auto input = context.ibuffers[channel][frame];
            const auto output = _curr->process(input, left);
            context.obuffers[channel][frame] = g * output;
        }
    }

    // Export `latency_actual` so we can see if there are discrepancies in the UI.
    const auto actual = (_curr == &_low) ? 0.f : 1.f;
    context.meters[enum_raw(models::Meters::Address::Latency_actual)] = actual;
}

} // namespace tiny::process
