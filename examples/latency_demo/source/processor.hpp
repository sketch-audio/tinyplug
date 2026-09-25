#pragma once

#include <array>

#include <tinyplug/tinyplug.hpp>
#include <tiny_dsp/delay_line.hpp>

namespace tiny::process {

// Delays its input by the selected mode, and changes latency the way every processor should:
// a new mode is only proposed, and the delay moves when the host accepts (`Reset::Latency`),
// never before. The editor shows the handshake in flight.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any& reset) -> void;
    auto handle(const Event::Any& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return _frame.current; }
    auto max_latency_samps() const -> uint32_t; // The longest mode: the framework sizes bypass for it at configure.
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;
    using Step = models::Handshake_frame::Step;
    static constexpr auto num_params = User_params::num_params;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};
    std::array<Delay_line, 2> _lines{};

    models::Handshake_frame _frame{}; // Also the processor's own record of the handshake.
    int64_t _clock{};
    int64_t _proposed_at{};
    bool _propose{};

    auto _wanted() const -> uint32_t;
    auto _step(Step::Kind kind, uint32_t samples, int64_t waited = 0) -> void;

};
static_assert(Interface<Processor>);

} // namespace tiny::process
