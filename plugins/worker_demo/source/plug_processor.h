#pragma once

#include <array>

#include "tinyplug/tinyplug.h"
#include "models/meter_model.h"
#include "models/param_model.h"

namespace tiny {

class Plug_processor {
public:

    auto reset(double /*sample_rate*/) -> void {}

    auto handle_event(const Render_event& event) -> void
    {
        std::visit(Inline_visitor{
            [this](const Set_param& e) { _values[e.address] = static_cast<float>(e.value); },
            [this](const Ramp_param& e) { _values[e.address] = static_cast<float>(e.target); },
            [](const auto&) {}
        }, event);
    }

    auto process(Dsp_context& context) -> void
    {
        const auto g = _values[enum_raw(Param_address::gain)];
        for (size_t channel = 0; channel < context.ibuffers.size(); ++channel) {
            for (size_t frame = 0; frame < context.num_frames; ++frame) {
                context.obuffers[channel][frame] = g * context.ibuffers[channel][frame];
            }
        }
        // Push a tick to the worker once per process call (low frequency, just to exercise the path).
        _worker.push(Tick{.sample_pos = context.musical_context.sample_pos});
    }

    auto latency_samps() const -> uint32_t { return 0; }
    auto tail_samps() const -> uint32_t { return 0; }

    // Optional opt-in: receive the worker actor from the wrapper.
    auto bind_worker(Worker_processor_actor a) -> void { _worker = a; }

    // Optional opt-in: receive replies from the worker.
    auto handle_worker_reply(const Plug_worker::To_processor& r) -> void
    {
        std::visit([this](const auto& a) {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(a)>, Set_counter>) {
                _last_count = a.count;
            }
        }, r);
    }

private:

    using User_params = Param_infos<Param_model>;
    using Param_address = Param_model::Param_address;
    static constexpr auto num_params = User_params::num_params;

    std::array<float, num_params> _values{User_params::make_defaults<float>(Value_space::Plain)};

    Worker_processor_actor _worker{};
    uint64_t _last_count{};

};
static_assert(Some_plug_processor<Plug_processor>);

} // namespace tiny
