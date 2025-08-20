#pragma once

#include <atomic>

#include "clap/clap.h"

#include "tinyplug/tinyplug.h"

#include "dsp_kernel.h"
#include "param_model.h"
#include "plug_info.h"

#include "clap_adapters.h"

namespace tiny {

class Clap_kernel {
public:

    Clap_kernel(const clap_host* host) : _host{host} {};

    // CLAP
    auto reset(double sample_rate) -> void;
    auto handle_flushed(const clap_event_header* event) -> void;
    auto get_host_value(clap_id paramId) -> double;
    auto get_latency() const -> uint32_t;
    auto process(const clap_process* process) -> clap_process_status;

    // tiny
    auto pop_export(Ui_event& event) -> bool;

    // Events handled via the action queue are forwarded to the host automatically on next process call.
    // We're also using this for loading state.
    // Expects knob values.
    auto handle_action(const User_action& action) -> void;

    auto wants_latency_change() const -> bool;

private:

    const clap_host* _host{nullptr};
    bool _once{false}; // Have we been reset?
    double _sr{48000};

    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{2};
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_schannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    User_params _param_infos{}; // We have to be able to map the values to plain space.

    // Values in host space.
    using Host_value = std::atomic<double>;
    using Host_values = std::array<Host_value, num_params>;
    Host_values _hostvalues{_param_infos.make_host_defaults<Host_value>()};

    std::array<double, num_exports> _lexports{};

    std::unique_ptr<Dsp_kernel> _kernel = std::make_unique<Dsp_kernel>();
    uint32_t _latency{_kernel->latency_samps()};

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // TODO: - Use a heuristic for size.
    using From_flush_queue = Lock_free_queue<Render_event, 256>;
    using From_ui_queue = Lock_free_queue<User_action, 256>;
    using To_ui_queue = Lock_free_queue<Ui_event, 256>;

    From_flush_queue _from_flush{};
    From_ui_queue _from_ui{};
    To_ui_queue _to_ui{};

    // MARK: - private

    auto _handle_host_flushed() -> void;
    auto _handle_user_actions(const clap_output_events_t* out_events) -> void;

    // This is where we handle host events from automation or flush.
    // - Kernel needs the plain value.
    // - Host-facing atomics need updated.
    // - UI needs the knob value.
    template<bool on_audio_thread>
    auto _handle_host_event(const clap_event_header* event) -> void
    {
        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

        switch (event->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* value_event = reinterpret_cast<const clap_event_param_value*>(event);
                const auto id = value_event->param_id;
                const auto& param = _param_infos.param_for(id);

                // Send plain value to kernel.
                const auto plain_value = Value_conv::host_to_plain(value_event->value, param.semantics);
                if constexpr (on_audio_thread) {
                    // On the audio thread we can handle the event now.
                    _kernel->handle_event(Set_param{.id = id, .value = plain_value});
                }
                else {
                    // On flush, we need to push into a queue for later.
                    _from_flush.push(Set_param{.id = id, .value = plain_value});
                }

                // Maintain host atomics.
                _hostvalues[id].store(value_event->value, std::memory_order_relaxed);

                // Notify UI.
                const auto knob_value = Value_conv::host_to_knob(value_event->value, param.semantics);
                _to_ui.push(Set_param{.id = id, .value = knob_value});

                break;
            }
        }
    }
};

}