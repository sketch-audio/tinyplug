#include "processor.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace tiny::process {

namespace {

constexpr auto trace_seconds = 4.;

constexpr auto kind_name(models::Log_entry::Kind kind) -> const char*
{
    using enum models::Log_entry::Kind;
    switch (kind) {
        case Configure: return "configure";
        case Set: return "set";
        case Ramp: return "ramp";
        case Restart: return "restart";
        case Latency: return "latency";
        case Render: return "render";
    }
    return "?";
}

} // namespace

auto Processor::configure(const Config& config) -> void
{
#if TINY_LOG_ENABLED
    log::init(); // Here, off the audio thread, so a log call there never pays for it.
#endif
    _sr = config.sr;
    _adopt(config.params);

    _clock = 0;
    _render.reset();
    _trace = models::Trace_frame{.bucket = std::max(1, static_cast<int32_t>(config.sr * trace_seconds / models::Trace_frame::size))};
    _lo = 1.f;
    _hi = 0.f;
    _filled = 0;
    _record(Kind::Configure, enum_raw(Address::Value), _values[enum_raw(Address::Value)], static_cast<int32_t>(config.sr));
}

auto Processor::reset(const Reset::Any& reset) -> void
{
    std::visit(Inline_visitor{
        [this](const Reset::Restart& r) { _adopt(r.params); _record(Kind::Restart, 0, _value.value); },
        [this](const Reset::Latency& e) { _record(Kind::Latency, 0, 0., static_cast<int32_t>(e.samples)); },
    }, reset);
}

auto Processor::handle(const Event::Param& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) {
            _values[e.address] = static_cast<float>(e.value);
            if (e.address == enum_raw(Address::Value)) _value.snap(static_cast<float>(e.value));
            _record(Kind::Set, e.address, e.value);
        },
        [this](const Event::Ramp& e) {
            _values[e.address] = static_cast<float>(e.value);
            if (e.address == enum_raw(Address::Value)) _value.start(static_cast<float>(e.value), static_cast<int32_t>(e.samples));
            _record(Kind::Ramp, e.address, e.value, static_cast<int32_t>(e.samples));
        },
    }, event);
}

auto Processor::process(Context& context) -> void
{
    if (_render != context.render_mode) {
        _render = context.render_mode;
        _record(Kind::Render, 0, context.render_mode == Render_mode::Offline ? 1. : 0.);
    }

    const auto dc = _values[enum_raw(Address::Dc_out)] >= 0.5f;
    const auto probe = _values[enum_raw(Address::Phase_out)] >= 0.5f;
    for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
        const auto value = _value.next();
        _trace_sample(value, _clock + static_cast<int64_t>(frame));

        _smooth += std::clamp(value - _smooth, -slew, slew);
        _phase += (20. + 2000. * static_cast<double>(_smooth)) / _sr;
        _phase -= std::floor(_phase);
        const auto sine = static_cast<float>(0.5 * std::sin(2. * std::numbers::pi * _phase));

        for (auto channel = size_t{}; channel < context.outputs.size(); ++channel) {
            const auto in = channel < context.inputs.size() ? context.inputs[channel][frame] : 0.f;
            context.outputs[channel][frame] = probe ? sine : dc ? value : in;
        }
    }
    _clock += static_cast<int64_t>(context.num_frames);

    context.blocks.write<Block::Log>() = _log;
    context.blocks.publish<Block::Log>();
    context.blocks.write<Block::Trace>() = _trace;
    context.blocks.publish<Block::Trace>();
}

// Everything `configure` and `Restart` share: the values exactly, and no history.
auto Processor::_adopt(std::span<const double> params) -> void
{
    for (auto i = size_t{}; i < num_params; ++i) _values[i] = static_cast<float>(params[i]);
    _value.snap(_values[enum_raw(Address::Value)]);
    _smooth = _value.value;
    _phase = 0.;
}

auto Processor::_record(Kind kind, uint32_t address, double value, int32_t dur) -> void
{
    const auto seq = _log.next++;
    _log.entries[seq % models::Log_frame::size] = {
        .seq = seq, .kind = kind, .address = static_cast<uint8_t>(address), .dur = dur, .frame = _clock, .value = value,
    };
    TINY_LOG_INFO(params, "#{} frame={} {} address={} value={} dur={}", seq, _clock, kind_name(kind), address, value, dur);
}

auto Processor::_trace_sample(float value, int64_t at) -> void
{
    _lo = std::min(_lo, value);
    _hi = std::max(_hi, value);
    if (++_filled < _trace.bucket) return;

    _trace.lo[_trace.write] = _lo;
    _trace.hi[_trace.write] = _hi;
    _trace.write = static_cast<uint32_t>((_trace.write + 1) % models::Trace_frame::size);
    _trace.head = at + 1;
    _lo = 1.f;
    _hi = 0.f;
    _filled = 0;
}

} // namespace tiny::process
