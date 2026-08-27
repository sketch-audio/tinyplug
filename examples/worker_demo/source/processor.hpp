#pragma once

#include <array>

#include "tinyplug/tinyplug.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"

namespace tiny::process {

class Processor {
public:

    auto configure(const Config& config) -> void
    {
        for (auto i = size_t{}; i < num_params; ++i) {
            _values[i] = static_cast<float>(config.params[i]);
        }
    }

    // Block-boundary sync: `Hard` restarts the stream, `Soft` lands deferred values,
    // `Latency` hands over a latency the host accepted. Nothing to do here. Never allocates.
    auto reset(const Reset::Any&) -> void {}

    auto handle(const Event::Any& event) -> void
    {
        std::visit(Inline_visitor{
            [this](const Event::Set& e) { _values[e.address] = static_cast<float>(e.value); },
            [this](const Event::Ramp& e) { _values[e.address] = static_cast<float>(e.target); },
            [](const auto&) {}
        }, event);
    }

    auto process(Dsp_context& context) -> void
    {
        const auto g = _values[enum_raw(Address::Gain)];
        for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
            for (size_t frame = 0; frame < context.num_frames; ++frame) {
                context.obuffers[channel][frame] = g * context.ibuffers[channel][frame];
            }
        }
        // Push a tick to the worker once per process call (low frequency, just to exercise the path).
        _worker.push(plugin::Tick{.sample_pos = context.musical_context.sample_pos});
    }

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

    // Optional opt-in: receive the worker actor from the wrapper.
    auto bind_worker(Worker_processor_actor a) -> void { _worker = a; }

    // Optional opt-in: receive replies from the worker.
    auto handle_worker_reply(const plugin::Worker::Model::To_processor& r) -> void
    {
        std::visit([this](const auto& a) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(a)>, plugin::Set_counter>) {
                _last_count = a.count;
            }
        }, r);
    }

private:

    using User_params = params::Infos<models::Params>;
    using Address = models::Params::Address;
    static constexpr auto num_params = User_params::num_params;

    using enum tiny::params::Space;
    std::array<float, num_params> _values{tiny::params::make_defaults<float, User_params>(Plain)};

    Worker_processor_actor _worker{};
    uint64_t _last_count{};

};
static_assert(Some_plug_processor<Processor>);

} // namespace tiny::process
