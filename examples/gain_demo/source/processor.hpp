#pragma once

#include <tinyplug/tinyplug.hpp>
#include <tiny_dsp/linear_ramper.hpp>

namespace tiny::process {

// The smallest complete effect: a linear gain (0 mutes), smoothed so automation never clicks, and peak
// meters either side of it.
class Processor {
public:

    auto configure(const Config& config) -> void;
    auto reset(const Reset::Any& reset) -> void;
    auto handle(const Event::Any& event) -> void;
    auto process(Dsp_context& context) -> void;

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

private:

    using Address = models::Params::Address;
    using Meter = models::Meters::Address;

    Linear_ramp _gain{1.f};

};
static_assert(Interface<Processor>);

} // namespace tiny::process
