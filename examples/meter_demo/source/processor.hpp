#pragma once

#include <array>

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::process {

// Drives one meter per policy with a signal you can predict from the outside, so
// a host can be checked against expected behaviour rather than "looks about right".
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any&) -> void;
    auto handle(const Event::Any& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using User_params = params::Infos<models::Params>;
    using Address = models::Params::Address;
    using Meter = models::Meters::Address;
    static constexpr auto num_params = User_params::num_params;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

    float _sr{48000};
    int64_t _pos{};        // Samples since configure; drives the LFO and the pulse.
    int64_t _next_trig{};  // Sample position of the next trigger.
    uint32_t _trig_count{}; // Magnitude carried by the trigger, 1..8 and wrapping.
    float _sparse_ramp{};   // Advances only while signal is present.

};
static_assert(Some_plug_processor<Processor>);

} // namespace tiny::process
