#include "clap_kernel.h"
#include <cmath>

namespace tiny {

auto Clap_kernel::reset(double sample_rate, size_t max_frames) -> void
{
    _kernel->reset(sample_rate, max_frames);
    _sr = sample_rate;
}

auto Clap_kernel::handle_flushed(const clap_event_header* event) -> void
{
    this->_handle_host_event<false>(event); // Not audio thread
}

auto Clap_kernel::handle_action(const User_action& action) -> void
{
    _from_ui.push(action);
}

auto Clap_kernel::get_host_value(clap_id paramId) -> double
{
    return _hostvalues[paramId].load(std::memory_order_relaxed);
}

auto Clap_kernel::pop_export(Ui_event& event) -> bool
{
    return _to_ui.pop(event);
}

auto Clap_kernel::process(const clap_process* process) -> clap_process_status
{
    this->_handle_host_flushed();
    this->_handle_user_actions(process->out_events);

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

        // Resolve the musical context.
        const auto* transport = process->transport;

        // We will derive the sample time from the time in seconds.
        const auto sec_pos = static_cast<double>(transport->song_pos_seconds) / CLAP_SECTIME_FACTOR;
        const auto sample_pos = std::round(sec_pos * _sr);
        const auto beat_pos = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR;
        const auto cycle_start = static_cast<double>(transport->loop_start_beats) / CLAP_BEATTIME_FACTOR;
        const auto cycle_end = static_cast<double>(transport->loop_end_beats) / CLAP_BEATTIME_FACTOR;
        const auto tempo = transport->tempo;
        const auto ts_numer = transport->tsig_num;
        const auto ts_denom = transport->tsig_denom;

        const auto flags = transport->flags;
        const auto has_flag = [](auto x, auto f) { return (x & f) > 0; };

        context.musical_context = {
            .sample_pos = static_cast<int64_t>(sample_pos + offset),
            .beat_pos = beat_pos + frames_to_beats(offset, tempo, _sr),
            .cycle_start = cycle_start,
            .cycle_end = cycle_end,
            .tempo_ideal = tempo,
            .time_sig = {ts_numer, ts_denom},
            .transport_state = {
                .moving = has_flag(flags, CLAP_TRANSPORT_IS_PLAYING),
                .cycling = has_flag(flags, CLAP_TRANSPORT_IS_LOOP_ACTIVE),
                .recording = has_flag(flags, CLAP_TRANSPORT_IS_RECORDING)
            }
        };

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
            this->_handle_host_event<true>(event);
            next_event();
        } while (event && event->time <= now);
    }

    // Send exports.
    for (auto i = decltype(num_exports){}; i < num_exports; ++i) {
        if (context.exports[i] != _lexports[i]) {
            // Send export and cache.
            const auto value = context.exports[i];
            _to_ui.push(Set_export{.id = i, .value = value});
            _lexports[i] = value;
        }
        _exports[i] = 0; // Reset for peak meters.
    }

    return CLAP_PROCESS_SLEEP;
}

// MARK: - private

auto Clap_kernel::_handle_host_flushed() -> void
{
    auto kernel_event = Render_event{};
    while (_from_flush.pop(kernel_event)) {
        _kernel->handle_event(kernel_event);
    }
}

auto Clap_kernel::_handle_user_actions(const clap_output_events_t* out_events) -> void
{
    auto user_action = User_action{};
    while (_from_ui.pop(user_action)) {
        std::visit(Inline_visitor{
            [&](const Action_start& a) {
                auto e = clap_event_param_gesture{};
                e.header.size = sizeof(clap_event_param_gesture);
                e.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
                e.header.time = 0;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.flags = 0;
                e.param_id = a.id;
                out_events->try_push(out_events, &e.header);
            },
            [&](const Set_param& a) {
                // Notify host
                auto e = clap_event_param_value{};
                e.header.size = sizeof(clap_event_param_value);
                e.header.type = CLAP_EVENT_PARAM_VALUE;
                e.header.time = 0;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.flags = 0;
                e.param_id = a.id;
                e.value = a.value;
                out_events->try_push(out_events, &e.header);

                // Send to DSP
                const auto& params = _param_infos.kernel_specs();
                const auto& param = params[a.id];
                const auto plain_value = Value_conv::knob_to_plain(a.value, param.semantics);
                _kernel->handle_event(Set_param{a.id, plain_value});

                // Maintain host values
                const auto host_value = Value_conv::knob_to_host(a.value, param.semantics);
                _hostvalues[a.id].store(host_value, std::memory_order_relaxed);
            },
            [&](const Action_end& a) {
                auto e = clap_event_param_gesture{};
                e.header.size = sizeof(clap_event_param_gesture);
                e.header.type = CLAP_EVENT_PARAM_GESTURE_END;
                e.header.time = 0;
                e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
                e.header.flags = 0;
                e.param_id = a.id;
                out_events->try_push(out_events, &e.header);
            },
        }, user_action);
    }
}

} // namespace tiny