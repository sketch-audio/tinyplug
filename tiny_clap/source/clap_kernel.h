#pragma once

#include <atomic>

#include "clap/clap.h"

#include "plug_info.h"
#include "user/dsp_kernel.h"
#include "user/param_model.h"

#include "clap_adapters.h"

namespace tiny {

class Clap_kernel {
public:

    Clap_kernel() {
        // Initialize host values with defaults.
        for (const auto& param : _param_infos.kernel_specs()) {
            _hostvalues[param.id] = get_host_default(param);
        }
    }

    // CLAP
    auto reset(double sample_rate, size_t max_frames) -> void;
    auto handle_flushed(const clap_event_header* event) -> void;
    auto get_host_value(clap_id paramId) -> double;
    auto process(const clap_process* process) -> clap_process_status;

    // tiny
    auto pop_export(Ui_event& event) -> bool;
    auto handle_action(const User_action& action) -> void;

private:

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
    using Host_values = std::array<std::atomic<double>, num_params>;
    Host_values _hostvalues{};

    std::array<double, num_exports> _lexports{};

    std::unique_ptr<Dsp_kernel> _kernel = std::make_unique<Dsp_kernel>();

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

    template<bool on_audio_thread>
    auto _handle_host_event(const clap_event_header* event) -> void
    {
        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

        const auto& params = _param_infos.kernel_specs();

        switch (event->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* value_event = reinterpret_cast<const clap_event_param_value*>(event);
                const auto id = value_event->param_id;
                const auto& param = params[id];
                const auto plain_value = Value_conv::host_to_plain(value_event->value, param.semantics);
                const auto knob_value = Value_conv::host_to_knob(value_event->value, param.semantics);

                const auto kernel_event = Set_param{.id = id, .value = plain_value};

                if constexpr (on_audio_thread) {
                    // On the audio thread we can handle the event now.
                    _kernel->handle_event(kernel_event);
                }
                else {
                    // On flush, we need to push into a queue for later.
                    _from_flush.push(kernel_event);
                }

                // Either way we need to update the host values and notify the UI.
                _hostvalues[id].store(value_event->value, std::memory_order_relaxed);
                _to_ui.push(Set_param{.id = id, .value = knob_value});

                break;
            }
        }
    }
};

}