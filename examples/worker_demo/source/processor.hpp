#pragma once

#include <tinyplug/tinyplug.hpp>

namespace tiny::process {

// A waveshaper whose curve the worker designs: when Drive moves, the processor asks for a new
// curve and shapes with the old one until it arrives. A heartbeat every 100 ms measures the
// round trip.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any&) -> void {}
    auto handle(const Event::Any& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

    auto bind_worker(Worker_processor_actor worker) -> void { _worker = worker; }
    auto handle_worker_reply(const User_work::To_processor& reply) -> void;

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;

    Worker_processor_actor _worker{};
    models::Channel_frame _frame{}; // Also what the processor shapes with.
    double _drive{};
    bool _design{};       // Drive moved and no request has gone out yet.
    uint32_t _seq{};
    int64_t _clock{};
    int64_t _next_beat{};

};
static_assert(Interface<Processor>);

} // namespace tiny::process
