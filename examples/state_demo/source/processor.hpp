#pragma once

#include <array>

#include <tinyplug/tinyplug.hpp>

namespace tiny::process {

// A sixteenth-note gate. The pattern is the state document; Depth scales how far it cuts.
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
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_steps = models::State::num_steps;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

    double _sr{48000.};
    double _free_beats{}; // Our own clock while the host transport is stopped.
    float _gain{1.f};
    float _glide{};       // One-pole coefficient: ~3 ms, so steps don't click.

};
static_assert(Some_plug_processor<Processor>);

} // namespace tiny::process
