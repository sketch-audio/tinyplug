#pragma once

#include <atomic>

#include "clap/clap.h"

#include "plug_info.h"
#include "user/dsp_kernel.h"
#include "user/param_model.h"

#include "clap_adapters.h"

namespace tiny::clap {

struct Clap_kernel {

    Clap_kernel() {
        // Initialize host defaults
        for (const auto& param : _params.get_kernel_specs()) {
            const auto idx = to_underlying(param.id);
            _hostvalues[idx] = get_host_default(param);
        }
    }

    auto reset(double sample_rate, size_t max_frames) -> void
    {
        _kernel->reset(sample_rate, max_frames);
    }

    auto handle_event(const clap_event_header* event) -> void
    {
        _handle_event<false>(event); // Not audio thread
    }

    auto get_host_value(clap_id paramId) -> double
    {
        return _hostvalues[paramId].load(std::memory_order_relaxed);
    }

    auto pop_export(Export_event& event) -> bool
    {
        return _oqueue.pop(event);
    }

    auto process(const clap_process* process) -> clap_process_status
    {
        // Process flushed events.
        auto kernel_event = Event{};
        while (_iqueue.pop(kernel_event)) {
            _kernel->handle_event(kernel_event);
        }

        // Get ready to process the input events.
        const auto* events = process->in_events;
        const auto event_count = events->size(events);

        auto event_index = size_t{};
        const auto* event = event_count > 0 ? events->get(events, event_index) : nullptr;

        auto next_event = [&]() {
            ++event_index;
            if (event_index >= event_count) {
                event = nullptr;
            }
            else {
                event = events->get(events, event_index);
            }
        };

        // Create the context.
        auto context = Dsp_context{.exports = _exports};

        // So we can process with an offset.
        auto do_process = [this, &process, &context](size_t num_frames, size_t offset) {
            // Assign buffer ptrs.
            for (size_t i = 0; i < num_ichannels; ++i) {
                const auto& input_port = process->audio_inputs[0];
                _ibuffers[i] = &input_port.data32[i][offset];
            }
            for (size_t i = 0; i < num_ochannels; ++i) {
                auto& output_port = process->audio_outputs[0];
                _obuffers[i] = &output_port.data32[i][offset];
            }
            if constexpr (Plug_info::wants_sidechain) {
                for (size_t i = 0; i < num_schannels; ++i) {
                    const auto& sidechain_port = process->audio_inputs[1];
                    _sbuffers[i] = &sidechain_port.data32[i][offset];
                }
            }

            context.ibuffers = _ibuffers;
            context.obuffers = _obuffers;
            context.sbuffers = _sbuffers;
            context.num_frames = num_frames;
            
            _kernel->process(context);
        };

        // Do process loop.
        const auto frame_count = process->frames_count;
        auto now = decltype(frame_count){};
        auto remaining = frame_count;

        while (remaining > 0) {
            if (!event) {
                const auto offset = frame_count - remaining;
                do_process(remaining, offset);
                break;
            }

            const auto frames_until_event = std::max({}, event->time - now);

            if (frames_until_event > 0) {
                const auto offset = frame_count - remaining;
                do_process(frames_until_event, offset);
                remaining -= frames_until_event;
                now += frames_until_event;
            }

            do {
                this->_handle_event<true>(event);
                next_event();
            } while (event && event->time <= now);
        }

        // Send exports.
        for (size_t i = 0; i < num_exports; ++i) {
            if (context.exports[i] != _lexports[i]) {
                // Send an output event.
                _oqueue.push({
                    .id = static_cast<uint32_t>(i),
                    .value = context.exports[i]
                });

                // Cache for next time.
                _lexports[i] = context.exports[i];
            }

            _exports[i] = 0; // Reset for peak meters.
        }

        return CLAP_PROCESS_SLEEP;
    }

private:
    
    using User_params = tiny::Params<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{2};
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_ichannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    User_params _params{}; // We have to be able to map the values to plain space.

    // Values in host space.
    std::array<std::atomic<double>, num_params> _hostvalues{};

    std::array<double, num_exports> _lexports{};

    std::unique_ptr<Dsp_kernel> _kernel = std::make_unique<Dsp_kernel>();

    // TODO: - Use a heuristic for size.
    using Event_queue = tiny::Lock_free_queue<tiny::Event, 256>;
    using Export_queue = tiny::Lock_free_queue<Export_event, 256>;
    Event_queue _iqueue{}; 
    Export_queue _oqueue{};

    // MARK: - private

    template<bool on_audio_thread>
    auto _handle_event(const clap_event_header* event) -> void
    {
        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

        const auto& params = _params.get_kernel_specs();

        switch (event->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* value_event = reinterpret_cast<const clap_event_param_value*>(event);
                const auto id = value_event->param_id;
                const auto& spec = params[id];

                const auto kernel_event = Set_param{
                    .id = id,
                    .value = host_to_plain_space(value_event->value, spec)
                };

                if constexpr (on_audio_thread) {
                    _kernel->handle_event(kernel_event);
                }
                else {
                    _iqueue.push(kernel_event);
                }

                // Maintain host values.
                _hostvalues[id].store(value_event->value, std::memory_order_relaxed);

                break;
            }
        }
    }
};

}