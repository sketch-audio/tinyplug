#include "processor.hpp"

#include <algorithm>
#include <cmath>

namespace tiny::process {

namespace {

// The curve until the worker's first arrives: no shaping.
auto identity() -> models::Curve_table
{
    auto table = models::Curve_table{};
    for (auto i = size_t{}; i < table.size(); ++i) table[i] = -1.f + 2.f * static_cast<float>(i) / static_cast<float>(table.size() - 1);
    return table;
}

auto shape(const models::Curve_table& table, float x) -> float
{
    const auto pos = (std::clamp(x, -1.f, 1.f) + 1.f) * 0.5f * static_cast<float>(table.size() - 1);
    const auto i = std::min(static_cast<size_t>(pos), table.size() - 2);
    const auto frac = pos - static_cast<float>(i);
    return table[i] + frac * (table[i + 1] - table[i]);
}

} // namespace

auto Processor::configure(const Config& config) -> void
{
    _frame = models::Channel_frame{.drive = 1., .table = identity(), .sr = config.sr};
    _drive = config.params[enum_raw(Address::Drive)];
    _design = true;
    _clock = 0;
    _next_beat = 0;
}

auto Processor::handle(const Event::Any& event) -> void
{
    std::visit(Inline_visitor{
        [this](const Event::Set& e) { _drive = e.value; _design = true; },
        [this](const Event::Ramp& e) { _drive = e.target; _design = true; },
    }, event);
}

auto Processor::handle_worker_reply(const User_work::To_processor& reply) -> void
{
    std::visit(Inline_visitor{
        [this](const models::Curve& c) {
            _frame.table = c.table; // A later request may still be out; each curve replaces the last.
            _frame.drive = c.drive;
            ++_frame.curves;
        },
        [this](const models::Echo& e) {
            _frame.round_trip = _clock - e.frame;
            ++_frame.echoes;
        },
    }, reply);
}

auto Processor::process(Dsp_context& context) -> void
{
    // Requests go out from here, lock-free; a full queue just means trying again next block.
    if (_design && _worker.push(models::Design{++_seq, _drive})) {
        _design = false;
        ++_frame.designs;
    }
    if (_clock >= _next_beat && _worker.push(models::Heartbeat{++_seq, _clock})) {
        _next_beat = _clock + static_cast<int64_t>(_frame.sr * 0.1);
        ++_frame.heartbeats;
    }

    const auto channels = std::min(context.ibuffers.size(), context.obuffers.size());
    for (auto channel = size_t{}; channel < channels; ++channel) {
        for (auto frame = size_t{}; frame < context.num_frames; ++frame) {
            context.obuffers[channel][frame] = shape(_frame.table, context.ibuffers[channel][frame]);
        }
    }

    _clock += static_cast<int64_t>(context.num_frames);
    context.blocks.write<Block::Channel>() = _frame;
    context.blocks.publish<Block::Channel>();
}

} // namespace tiny::process
