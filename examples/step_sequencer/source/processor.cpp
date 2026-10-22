#include "processor.hpp"

#include <algorithm>
#include <cmath>

namespace tiny::process {

auto Processor::configure(const Config& config) -> void
{
    _sr = config.sr;
    _adopt(config.params);
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Restart& r) { _adopt(r.params); },
        [](const Reset::Latency&) {}
    }, reset);
}

// The framework sends all-notes-off downstream; the pattern starts afresh.
auto Processor::_adopt(std::span<const double> params) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) _values[i] = static_cast<float>(params[i]);
    _last_step.reset();
    _held.reset();
    _num_through = 0;
}

auto Processor::handle(const Event::Param& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _values[e.address] = static_cast<float>(e.value); },
        [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.value); },
    }, event);
}

auto Processor::handle(const Note::Any& note) -> void { _pass(note); }
auto Processor::handle(const Control::Any& control) -> void { _pass(control); }

auto Processor::_pass(const Performance& event) -> void
{
    if (_num_through < _through.size()) _through[_num_through++] = event;
}

auto Processor::_release(Context& context, int64_t frame) -> void
{
    if (!_held) return;
    context.notes.send(frame, Note::Off{_held->note, 0.f});
    _held.reset();
}

auto Processor::process(Context& context) -> void
{
    // Declared as an instrument so every host loads it; the audio it owes is silence.
    for (auto* channel : context.outputs) std::fill_n(channel, context.num_frames, 0.f);

    for (auto i = size_t{}; i < _num_through; ++i) {
        std::visit([&](const auto& e) { context.notes.send(0, e); }, _through[i]);
    }
    _num_through = 0;

    const auto& timeline = context.timeline;
    auto& step_meter = context.meters[enum_raw(Meter::Step)];

    if (!(timeline.state && timeline.state->moving && timeline.beat_pos)) { // Nothing to step along without beats.
        _release(context, 0); // Stopping closes the sounding note.
        _last_step.reset();
        step_meter = static_cast<float>(num_steps);
        return;
    }

    const auto& pattern = context.state.get();
    const auto gate = static_cast<double>(_values[enum_raw(Address::Gate)]);
    const auto velocity = _values[enum_raw(Address::Velocity)];
    const auto beats_per_frame = timeline.tempo.value_or(120.) / (60. * _sr);

    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        const auto beat = *timeline.beat_pos + static_cast<double>(frame) * beats_per_frame;
        const auto f = static_cast<int64_t>(frame);

        if (_held && beat >= _held->off_beat) _release(context, f);

        const auto sixteenth = static_cast<int64_t>(std::floor(beat * 4.));
        if (_last_step == sixteenth || beat < 0.) continue;
        _last_step = sixteenth; // A loop jumping back is a new step too.

        const auto key = pattern.key[static_cast<size_t>(sixteenth % static_cast<int64_t>(num_steps))];
        if (key == models::State::rest) continue;

        _release(context, f); // A gate longer than its step ends where the next begins.
        const auto note = Note::Id{own_id | (_next_id++ & ~own_id), 0, key};
        context.notes.send(f, Note::On{note, velocity});
        _held = Held{note, (static_cast<double>(sixteenth) + gate) / 4.};
    }

    if (_last_step) step_meter = static_cast<float>(*_last_step % static_cast<int64_t>(num_steps));
}

} // namespace tiny::process
