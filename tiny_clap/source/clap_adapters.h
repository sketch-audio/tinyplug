#pragma once

#include "clap/clap.h"

#include "tinyplug/tinyplug.h"

#include "plug_info.h"
#include "user/dsp_kernel.h"
#include "user/param_model.h"

namespace tiny::clap {

// MARK: - adapter

template<Some_dsp_kernel Kernel>
struct Kernel_adapter {

    Kernel_adapter() {
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
        return _hostvalues[paramId];
    }

    auto process(const clap_process* process) -> clap_process_status
    {
        // Process flushed events.
        auto kernel_event = Event{};
        while (_queue.pop(kernel_event)) {
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

        auto do_process = [this, &process](size_t num_frames, size_t offset) {
            // Assign buffer ptrs.
            _ibuffers[0] = &process->audio_inputs->data32[0][offset];
            _ibuffers[1] = &process->audio_inputs->data32[1][offset];

            if constexpr (Plug_info::wants_sidechain) {
                _ibuffers[2] = &process->audio_inputs->data32[0][offset];
                _ibuffers[3] = &process->audio_inputs->data32[1][offset];
            }

            _obuffers[0] = &process->audio_outputs->data32[0][offset];
            _obuffers[1] = &process->audio_outputs->data32[1][offset];

            // Process kernel.
            auto context = tiny::Dsp_context{
                .ibuffers = _ibuffers,
                .obuffers = _obuffers,
                .num_frames = num_frames
            };
            _kernel->process(context);
        };

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

        return CLAP_PROCESS_SLEEP;
    }

private:

    static constexpr auto num_ichannels = size_t{2 + (tiny::Plug_info::wants_sidechain ? 2 : 0)};
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<float*, num_ochannels> _obuffers{};

    using User_params = tiny::Params<tiny::Param_model>;
    User_params _params{}; // We have to be able to map the values.

    // Values in host space.
    User_params::Param_values _hostvalues{}; // these should probably be atomics

    std::unique_ptr<Kernel> _kernel = std::make_unique<Kernel>();

    using Queue = tiny::Lock_free_queue<tiny::Event, 256, tiny::Queue_concurrency::mpsc>;
    Queue _queue{}; // TODO: - Use a heuristic.

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
                    _hostvalues[id] = value_event->value;
                    _kernel->handle_event(kernel_event);
                }
                else {
                    _hostvalues[id] = value_event->value;
                    _queue.push(kernel_event);
                }

                break;
            }
        }
    }

};

// MARK: - modules

template <typename Id>
inline auto tree_to_clap_modules(const Param_node<Id>& root) -> std::vector<std::string> {
    auto result = std::vector<std::string>{};

    const auto visit = [&](const Param_node<Id>& node, const std::string& path, const auto& self) -> void {
        std::visit(
            Inline_visitor{
                [&](const Param_spec<Id>&) {
                    result.push_back(path);
                },
                [&](const Param_group<Id>& group) {
                    const auto group_path = path.empty() ? std::string{group.name} : path + "/" + group.name;

                    for (const auto& child : group.nodes) {
                        self(child, group_path, self);
                    }
                }
            }
        , node);
    };

    visit(root, "", visit);
    return result;
}

}