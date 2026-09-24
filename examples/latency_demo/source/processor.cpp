#include "processor.hpp"

#include <algorithm>
#include <cmath>

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) _values[i] = static_cast<float>(config.params[i]);
    _frame.sr = config.sr;

    const auto longest = static_cast<size_t>(std::ceil(models::Params::mode_ms.back() * 0.001 * config.sr));
    for (auto& line : _lines) line.resize(longest + 1);

    // Come up in the mode the values ask for: `configure` is final, so nothing is proposed.
    // A proposal still outstanding is superseded; the framework clears its side of it too.
    _clock = 0;
    _propose = false;
    _frame.pending = -1;
    _frame.wanted = _frame.current = _wanted();
    _step(Step::Kind::Configure, _frame.current);
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Hard&) {
            for (auto& line : _lines) line.clear();
            _step(Step::Kind::Hard, _frame.current);
        },
        [](const Reset::Soft&) {},
        // The host has aligned its graph: move now, to exactly what it accepted.
        [this](const Reset::Latency& e) {
            _frame.current = e.samples;
            _step(Step::Kind::Accept, e.samples, _clock - _proposed_at);
            if (_frame.pending == static_cast<int64_t>(e.samples)) _frame.pending = -1;
        },
    }, reset);
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) {
            if (e.address == enum_raw(Address::Mode) && static_cast<float>(e.value) != _values[e.address]) _propose = true;
            _values[e.address] = static_cast<float>(e.value);
        },
        [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.target); },
    }, event);
}

auto Processor::process(Dsp_context& context) -> void
{
    // Restate the intention at every change, derived from the parameter: a later proposal
    // supersedes one the host hasn't acted on yet (Logic may wait until playback starts).
    _frame.wanted = _wanted();
    if (_propose) {
        _propose = false;
        context.propose_latency = _frame.wanted;
        _frame.pending = _frame.wanted;
        _proposed_at = _clock;
        _step(Step::Kind::Propose, _frame.wanted);
    }

    const auto click = _values[enum_raw(Address::Click)] >= 0.5f;
    const auto& musical = context.musical_context;
    const auto second = std::max<int64_t>(1, std::llround(_frame.sr));
    const auto channels = std::min({context.ibuffers.size(), context.obuffers.size(), _lines.size()});
    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        const auto at = (musical.transport_state.moving ? musical.sample_pos : _clock) + static_cast<int64_t>(frame);
        const auto tick = at % second == 0 ? 1.f : 0.f;
        for (auto channel = size_t{}; channel < channels; ++channel) {
            auto& line = _lines[channel];
            line.write(click ? tick : context.ibuffers[channel][frame]);
            context.obuffers[channel][frame] = line.read(_frame.current);
        }
    }

    _clock += static_cast<int64_t>(context.num_frames);
    _frame.waiting = _frame.pending >= 0 ? _clock - _proposed_at : 0;
    context.blocks.write<Block::Handshake>() = _frame;
    context.blocks.publish<Block::Handshake>();
}

auto Processor::_wanted() const -> uint32_t
{
    const auto index = std::clamp(static_cast<size_t>(_values[enum_raw(Address::Mode)] + 0.5f), size_t{}, models::Params::mode_ms.size() - 1);
    return static_cast<uint32_t>(std::lround(models::Params::mode_ms[index] * 0.001 * _frame.sr));
}

auto Processor::_step(Step::Kind kind, uint32_t samples, int64_t waited) -> void
{
    const auto seq = _frame.next++;
    _frame.steps[seq % models::Handshake_frame::size] = {.seq = seq, .kind = kind, .samples = samples, .frame = _clock, .waited = waited};
}

} // namespace tiny::process
