#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "clap_plugin.h"

namespace tiny {

// MARK: - plugin

bool Clap_plugin::init() noexcept
{
    return true;
}

bool Clap_plugin::activate(double sampleRate, uint32_t /*minFrameCount*/, uint32_t /*maxFrameCount*/) noexcept
{
    // Reset kernel with sample rate only first time and then when sample rate changes.
    if (!_once || sampleRate != _sr) {
        _processor->reset(sampleRate);
        _latency = _processor->latency_samps();
        _sr = sampleRate;
        _once = true;
    }

    // Are we here because the kernel wanted a latency change?
    const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (pending_latency) {
        _accepted_latency.store(*pending_latency, std::memory_order_release); // The kernel should manifest on the next process.
        _latency = *pending_latency;

        // Notify the host.
        auto* latency_ext = (const clap_host_latency_t*)_host->get_extension(_host, CLAP_EXT_LATENCY);
        if (latency_ext) latency_ext->changed(_host);
    }

    return true;
}

void Clap_plugin::deactivate() noexcept
{
}

bool Clap_plugin::startProcessing() noexcept
{
    return true;
}

void Clap_plugin::stopProcessing() noexcept
{
}

// MARK: - process

clap_process_status Clap_plugin::process(const clap_process* process) noexcept
{
    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        _processor->handle_event(Accepted_latency{*accepted_latency});
        assert(_processor->latency_samps() == *accepted_latency && "Kernel must apply the accepted latency!");
    }

    this->_handle_host_flushed();
    this->_handle_user_actions(process->out_events);

    // Get ready to process the input events.
    const auto* events = process->in_events;
    const auto event_count = events->size(events);

    auto event_index = uint32_t{};
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
    auto context = Dsp_context{.meters = _meters, .propose_latency = {}};

    // So we can process with an offset.
    auto do_process = [this, &process, &context](size_t num_frames, size_t offset) {
        // Assign buffer ptrs.
        const auto& input_port = process->audio_inputs[0];
        assert(input_port.channel_count == static_cast<uint32_t>(_ichannels));
        for (size_t i = 0; i < _ichannels; ++i) {
            _ibuffers[i] = &input_port.data32[i][offset];
        }

        auto& output_port = process->audio_outputs[0];
        assert(output_port.channel_count == static_cast<uint32_t>(_ochannels));
        for (size_t i = 0; i < _ochannels; ++i) {
            _obuffers[i] = &output_port.data32[i][offset];
        }

        if constexpr (Plug_info::wants_sidechain) {
            const auto& sidechain_port = process->audio_inputs[1];
            assert(sidechain_port.channel_count == static_cast<uint32_t>(_schannels));
            for (size_t i = 0; i < _schannels; ++i) {
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
            .beat_pos = beat_pos + frames_to_beats(static_cast<int64_t>(offset), tempo, _sr),
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

        context.ibuffers = {_ibuffers.begin(), _ichannels};
        context.obuffers = {_obuffers.begin(), _ochannels};
        context.sbuffers = {_sbuffers.begin(), _schannels};
        context.num_frames = num_frames;
        
        _processor->process(context);
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
    for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
        if (context.meters[i] != _last_meters[i]) {
            // Send export and cache.
            const auto value = context.meters[i];
            _to_editor.push(Set_meter{.address = i, .value = value});
            _last_meters[i] = value;
        }
        _meters[i] = 0; // Reset for peak meters.
    }

    // Has the kernel proposed a new latency?
    if (const auto proposed_latency = context.propose_latency; proposed_latency && *proposed_latency != _latency) {
        // Notify controller and sit on the pending latency.
        _host->request_restart(_host);
        _pending_latency.store(*proposed_latency, std::memory_order_release);
    }

    const auto tail = _processor->tail_samps();
    if (tail != _tail) {
        const auto* tail_ext = (const clap_host_tail*)_host->get_extension(_host, CLAP_EXT_TAIL);
        if (tail_ext) tail_ext->changed(_host);
        _tail = tail;
    }

    return CLAP_PROCESS_CONTINUE;
}

void Clap_plugin::reset() noexcept
{
}

void Clap_plugin::onMainThread() noexcept
{
}

const void* Clap_plugin::extension(const char* /*id*/) noexcept
{
    return nullptr;
}

bool Clap_plugin::enableDraftExtensions() const noexcept
{
    return false;
}

// MARK: - save state

bool Clap_plugin::stateSave(const clap_ostream* stream) noexcept
{
    if (!stream) return false;

    const auto edit_state = _editor->save_state();
    const auto num_editor_items = static_cast<uint32_t>(edit_state.size());

    // Write header.
    const auto header = State_header{
        Plug_info::framework_code, // Reserved
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_params, // Processor state
        num_editor_items
    };
    const auto expected = sizeof(header);
    const auto result = stream->write(stream, header.data(), expected);
    if (result != expected) {
        return false;
    }

    // Helpers
    auto write_value = [&](const auto& data) {
        const auto expected = sizeof(data);
        const auto result = stream->write(stream, &data, expected);
        return result == expected;
    };

    auto write_container = [&](const auto& data) {
        const auto num = static_cast<uint32_t>(data.size());
        const auto num_result = stream->write(stream, &num, sizeof(num));
        if (num_result != sizeof(num)) {
            return false;
        }
        const auto data_result = stream->write(stream, data.data(), sizeof(data[0]) * num);
        return static_cast<size_t>(data_result) == sizeof(data[0]) * num;
    };

    // Write processor state.
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        const auto host_value = static_cast<float>(_hostvalues[i].load(std::memory_order_relaxed));
        if (!write_value(host_value)) {
            return false;
        }
    }

    // Write editor state.
    for (const auto& [key, val] : edit_state) {
        // Write key.
        if (!write_container(key)) {
            return false;
        }

        // Write the type tag.
        const auto tag = tag_for(val);
        if (!write_value(tag)) {
            return false;
        }

        // Write the value according to the tag.
        switch (tag) {
            case State_tag::bool_: {
               const auto value = std::get_if<bool>(&val);
               if (value && write_value(*value)) {
                   break;
               }
               return false;
            }
            case State_tag::int_: {
                const auto value = std::get_if<int32_t>(&val);
                if (value && write_value(*value)) {
                    break;
                }
                return false;
            }
            case State_tag::double_: {
                const auto value = std::get_if<double>(&val);
                if (value && write_value(*value)) {
                    break;
                }
                return false;
            }
            case State_tag::string_: {
                const auto value = std::get_if<std::string>(&val);
                if (value && write_container(*value)) {
                    break;
                }
                return false;
            }
            case State_tag::bytes_: {
                const auto value = std::get_if<std::vector<uint8_t>>(&val);
                if (value && write_container(*value)) {
                    break;
                }
                return false;
            }
            default:
                assert(false && "Unknown editor state type.");
                return false;
        }
    }

    return true;
}

// MARK: - load state

bool Clap_plugin::stateLoad(const clap_istream* stream) noexcept
{
    if (!stream) return false;

    auto header = State_header{};
    const auto expected = sizeof(header);
    const auto result = stream->read(stream, header.data(), expected);
    if (result != expected) {
        return false;
    }

    // Validate header.
    assert(header[0] == Plug_info::framework_code && "Unexpected framework code.");
    assert(header[1] == Plug_info::manufacturer_code && "Unexpected manufacturer code.");
    assert(header[2] == Plug_info::plugin_code && "Unexpected plug-in code.");

    const auto num_state_params = header[3];
    const auto num_kv_pairs = header[4];

    // Helpers
    auto read_value = [&](auto& data) {
        const auto expected = sizeof(data);
        const auto result = stream->read(stream, &data, expected);
        return result == expected;
    };

    auto read_container = [&](auto& data) {
        auto num = uint32_t{};
        const auto num_result = stream->read(stream, &num, sizeof(num));
        if (num_result != sizeof(num)) {
            return false;
        }
        data.resize(num);
        const auto data_result = stream->read(stream, data.data(), sizeof(data[0]) * num);
        return static_cast<size_t>(data_result) == sizeof(data[0]) * num;
    };

    // Notify kernel and view (if not an interface parameter).
    auto do_notify = [this](auto& param, auto knob_value) {
        if (param.policy != Host_policy::interface) {
            this->_handle_user_action(Set_param{.address = param.address, .value = knob_value});

            if (_view) {
                _view->set_param(param.address, knob_value);
            }
        }
    };

    // Read processor state into temporary vector.
    auto state_params = std::vector<float>(static_cast<size_t>(num_state_params), 0.f);
    for (auto i = decltype(num_state_params){}; i < num_state_params; ++i) {
        auto host_value = float{};
        if (!read_value(host_value)) {
            return false;
        }
        state_params[i] = host_value;
    }

    if (num_params <= num_state_params) {
        // Read as many values as we can.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto host_value = state_params[i];
            const auto& param = User_params::param_spec(i);
            const auto knob_value = Value_conv::host_to_knob(host_value, param.semantics);
            do_notify(param, knob_value);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_state_params){}; i < num_state_params; ++i) {
            const auto host_value = state_params[i];
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
            const auto knob_value = Value_conv::host_to_knob(host_value, param.semantics);
            do_notify(param, knob_value);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_state_params; i < num_params; ++i) {
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
            const auto knob_value = get_knob_default(param);
            do_notify(param, knob_value);
        }
    }

    // Read editor state into temporary map.
    auto edit_state = State_map{};
    for (auto i = decltype(num_kv_pairs){}; i < num_kv_pairs; ++i) {
        // Read key.
        auto key = std::string{};
        if (!read_container(key)) {
            return false;
        }

        // Read the type tag.
        auto tag = State_tag{};
        if (!read_value(tag)) {
            return false;
        }

        // Read the value according to the tag.
        auto value = State_item{};
        switch (tag) {
            case State_tag::bool_: {
                auto v = bool{};
                if (!read_value(v)) {
                    return false;
                }
                value = v;
                break;
            }
            case State_tag::int_: {
                auto v = int32_t{};
                if (!read_value(v)) {
                    return false;
                }
                value = v;
                break;
            }
            case State_tag::double_: {
                auto v = double{};
                if (!read_value(v)) {
                    return false;
                }
                value = v;
                break;
            }
            case State_tag::string_: {
                auto v = std::string{};
                if (!read_container(v)) {
                    return false;
                }
                value = std::move(v);
                break;
            }
            case State_tag::bytes_: {
                auto v = std::vector<uint8_t>{};
                if (!read_container(v)) {
                    return false;
                }
                value = std::move(v);
                break;
            }
        }

        edit_state.emplace(std::move(key), std::move(value));
    }

    _editor->load_state(edit_state);

    return true;
}

// MARK: - audio ports

uint32_t Clap_plugin::audioPortsCount(bool isInput) const noexcept
{
    return isInput ? (Plug_info::wants_sidechain ? 2 : 1) : 1;
}

bool Clap_plugin::audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept
{
    if (!info) return false;

    const auto is_main = (index == 0);
    const char* port_name = isInput ? (is_main ? "Input" : "Sidechain") : "Output";

    const auto channel_count = isInput ? (is_main ? _ichannels : _schannels) : _ochannels;
    const auto port_type = (channel_count == 2) ? CLAP_PORT_STEREO : CLAP_PORT_MONO;

    *info = {};
    info->id = index;
    std::strncpy(info->name, port_name, CLAP_NAME_SIZE);
    info->flags = is_main ? CLAP_AUDIO_PORT_IS_MAIN : uint32_t{};
    info->channel_count = static_cast<uint32_t>(channel_count); // 
    info->port_type = port_type;
    info->in_place_pair = CLAP_INVALID_ID;

    return true;
}

// MARK: - configurable audio ports

bool Clap_plugin::configurableAudioPortsCanApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) const noexcept
{
    if (!requests) return false;

    if (request_count == 0) return true; // No change.

    auto ichannels = uint32_t{};
    auto schannels = uint32_t{};
    auto ochannels = uint32_t{};

    auto check_port_type = [](const clap_audio_port_configuration_request& request) {
        const auto mono_is_mono = (request.channel_count == 1 && strcmp(request.port_type, CLAP_PORT_MONO) == 0);
        const auto stereo_is_stereo = (request.channel_count == 2 && strcmp(request.port_type, CLAP_PORT_STEREO) == 0);
        return mono_is_mono || stereo_is_stereo;
    };

    const auto requests_ = std::span{requests, static_cast<size_t>(request_count)};
    for (const auto& request : requests_) {
        // Check port types match channel count.
        if (!check_port_type(request)) continue;

        const auto is_main = (request.port_index == 0);
        if (request.is_input && is_main) {
            ichannels = request.channel_count;
        }
        else if (is_main) {
            ochannels = request.channel_count;
        }
        else if (request.is_input) {
            schannels = request.channel_count;
        }
    }

    const auto sidechain_ok = [&]() {
        if constexpr (Plug_info::wants_sidechain) {
            return schannels > 0;
        }
        return schannels == 0;
    }();
    const auto wants_mono = (ichannels == 1 && ochannels == 1);
    const auto wants_stereo = (ichannels == 2 && ochannels == 2);

    const auto config_ok = (wants_stereo || (Plug_info::can_process_mono && wants_mono)) && sidechain_ok;
    return config_ok;
}

bool Clap_plugin::configurableAudioPortsApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) noexcept
{
    if (!requests) return false;

    if (request_count == 0) return true; // No change.

    const auto requests_ = std::span{requests, static_cast<size_t>(request_count)};
    for (const auto& request : requests_) {
        const auto is_main = (request.port_index == 0);
        if (request.is_input && is_main) {
            _ichannels = request.channel_count;
        }
        else if (is_main) {
            _ochannels = request.channel_count;
        }
        else if (request.is_input) {
            _schannels = request.channel_count;
        }
    }

    return true;
}

// MARK: - params

uint32_t Clap_plugin::paramsCount() const noexcept
{
    return num_params;
}

bool Clap_plugin::paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept
{
    // The index is the order of appearance in the UI, and isn't necessarily the same as the id.
    if (paramIndex >= num_params || !info) return false;

    const auto& params = User_params::param_specs(Param_order::Presentation); // Report params in presentation order!

    const auto& param = params[paramIndex];
    const auto& path = _modules[paramIndex];

    *info = {}; // Clear.
    info->id = param.address;
    info->flags = [policy = param.policy]() {
        using enum Host_policy;
        switch (policy) {
            case automation: return uint32_t{CLAP_PARAM_IS_AUTOMATABLE};
            case control: return uint32_t{}; // Do any hosts actually show a control here?
            case hidden: return uint32_t{CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY};
            case interface: return uint32_t{CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY};
            default: return uint32_t{};
        }
    }();
    info->cookie = nullptr;
    std::strncpy(info->name, param.name, CLAP_NAME_SIZE);
    std::strncpy(info->module, path.c_str(), CLAP_NAME_SIZE);

    // CLAP uses host values.
    // Set min, max, default based on semantics.
    std::visit(Inline_visitor{
        [&](const Bool_semantics& b) {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = 0;
            info->max_value = 1;
            info->default_value = b.def_val ? 1 : 0;
        },
        [&](const List_semantics& l) {
            info->flags |= (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM);
            info->min_value = 0;
            info->max_value = static_cast<double>(l.items.size() - 1);
            info->default_value = static_cast<double>(l.def_val);
        },
        [&](const Int_semantics& i) {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = i.min_val;
            info->max_value = i.max_val;
            info->default_value = i.def_val;
        },
        [&](const Fixed_semantics& f) {
            info->min_value = f.min_val;
            info->max_value = f.max_val;
            info->default_value = f.def_val;
        },
        [&](const Real_semantics& r) {
            info->min_value = 0;
            info->max_value = 1;
            info->default_value = plain_to_norm(r.def_val, r);
        },
    }, param.semantics);

    return true;
}

bool Clap_plugin::paramsValue(clap_id paramId, double* value) noexcept
{
    if (paramId >= num_params) return false;
    *value = _hostvalues[paramId].load(std::memory_order_relaxed);
    return true;
}

bool Clap_plugin::paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept
{
    if (paramId >= num_params || !display) return false;

    const auto& param = User_params::param_spec(paramId);
    const auto str = Host_formatter::format_string(value, param.semantics);
    std::strncpy(display, str.c_str(), size);
    display[size - 1] = '\0'; // In case str is longer than display.

    return true;
}

bool Clap_plugin::paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept
{
    if (paramId >= num_params || !display) return false;

    const auto& param = User_params::param_spec(paramId);
    const auto str = std::string{display};

    if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
        *value = Value_conv::plain_to_host(*plain, param.semantics);
        return true;
    }

    return false;
}

void Clap_plugin::paramsFlush(const clap_input_events* in, const clap_output_events* /*out*/) noexcept
{
    if (!in) return;

    const auto size = in->size(in);

    for (auto i = decltype(size){}; i < size; ++i) {
        const auto* event = in->get(in, i);
        this->_handle_host_event<false>(event);
    }
}

// MARK: - gui

bool Clap_plugin::guiIsApiSupported(const char* api, bool isFloating) noexcept
{
    return !isFloating && strcmp(api, gui_preferred_api) == 0;
}

bool Clap_plugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept
{
    *api = gui_preferred_api;
    *isFloating = false;
    return true;
}

bool Clap_plugin::guiCreate(const char* /*api*/, bool /*isFloating*/) noexcept
{
    // Make the UI connection.
    auto receiver = Ui_receiver{
        .get_knob_value = [this](auto id) {
            const auto& param = User_params::param_spec(id);
            const auto host_value = _hostvalues[id].load(std::memory_order_relaxed);
            const auto knob_value = Value_conv::host_to_knob(host_value, param.semantics);
            return knob_value;
        },
        .pop_event = [this](auto& event) {
            return _to_editor.pop(event);
        },
        .action_handler = [this](auto& action) {
            this->_handle_user_action(action);
        }
    };

    _view = std::make_unique<Clap_view>(Clap_view::Deps{.editor = &(*_editor), .receiver = std::move(receiver), .tasks = &_tasks});
    _view->on_create();
    return true;
}

void Clap_plugin::guiDestroy() noexcept
{
    _view->on_destroy();
    _view = nullptr;
}

bool Clap_plugin::guiSetScale(double /*scale*/) noexcept
{
    return true;
}

bool Clap_plugin::guiShow() noexcept
{
    if (!_view) return false;
    _view->on_show();
    return true;
}

bool Clap_plugin::guiHide() noexcept
{
    if (!_view) return false;
    _view->on_hide();
    return true;
}

bool Clap_plugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept
{
    _view->get_size(width, height);
    return true;
}

bool Clap_plugin::guiCanResize() const noexcept
{
    return true;
}

bool Clap_plugin::guiGetResizeHints(clap_gui_resize_hints_t* /*hints*/) noexcept
{
    // *hints = {
    //     .can_resize_horizontally = true,
    //     .can_resize_vertically = true,
    //     .preserve_aspect_ratio = false,
    //     .aspect_ratio_width = 0,
    //     .aspect_ratio_height = 0
    // };
    return true;
}

bool Clap_plugin::guiAdjustSize(uint32_t* /*width*/, uint32_t* /*height*/) noexcept
{
    return true;
}

bool Clap_plugin::guiSetSize(uint32_t width, uint32_t height) noexcept
{
    return _view->set_size(width, height);
}

void Clap_plugin::guiSuggestTitle(const char* /*title*/) noexcept
{
    // floating only
}

bool Clap_plugin::guiSetParent(const clap_window* window) noexcept
{
    if (!_view) return false;
    return _view->set_parent(window);
}

bool Clap_plugin::guiSetTransient(const clap_window* /*window*/) noexcept
{
    return false; // floating only
}

// MARK: - latency

uint32_t Clap_plugin::latencyGet() const noexcept
{
    return _latency;
}

// MARK: - tail

uint32_t Clap_plugin::tailGet() const noexcept
{
    // CLAP will interpret anything >= INT32_MAX as infinite.
    return _tail;
}

// MARK: - private

auto Clap_plugin::_handle_host_flushed() -> void
{
    auto kernel_event = Render_event{};
    while (_from_flush.pop(kernel_event)) {
        _processor->handle_event(kernel_event);
    }
}

auto Clap_plugin::_handle_user_actions(const clap_output_events_t* out_events) -> void
{
    // The host only needs to know about changes where there might be automation or a control in the host UI.
    auto wants_host_notify = [](Host_policy policy) {
        using enum Host_policy;
        return policy == automation || policy == control;
    };
    
    auto user_action = User_action{};
    while (_from_ui.pop(user_action)) {
        std::visit(Inline_visitor{
            [&](const Action_start& a) {
                const auto& param = User_params::param_spec(a.address);
                if (wants_host_notify(param.policy)) {
                    const auto e = clap_event_param_gesture{
                        .header = {
                            .size = sizeof(clap_event_param_gesture),
                            .time = {},
                            .space_id = CLAP_CORE_EVENT_SPACE_ID,
                            .type = CLAP_EVENT_PARAM_GESTURE_BEGIN,
                            .flags = {},
                        },
                        .param_id = param.address
                    };
                    out_events->try_push(out_events, &e.header);
                }
            },
            [&](const Set_param& a) {
                const auto& param = User_params::param_spec(a.address);

                if (wants_host_notify(param.policy)) {
                    const auto host_value = Value_conv::knob_to_host(a.value, param.semantics);
                    const auto e = clap_event_param_value{
                        .header = {
                            .size = sizeof(clap_event_param_value),
                            .time = {},
                            .space_id = CLAP_CORE_EVENT_SPACE_ID,
                            .type = CLAP_EVENT_PARAM_VALUE,
                            .flags = {},
                        },
                        .param_id = param.address,
                        .value = host_value,
                    };
                    out_events->try_push(out_events, &e.header);
                }

                const auto plain_value = Value_conv::knob_to_plain(a.value, param.semantics);
                _processor->handle_event(Set_param{param.address, plain_value});
            },
            [&](const Action_end& a) {
                const auto& param = User_params::param_spec(a.address);
                if (wants_host_notify(param.policy)) {
                    const auto e = clap_event_param_gesture{
                        .header = {
                            .size = sizeof(clap_event_param_gesture),
                            .time = {},
                            .space_id = CLAP_CORE_EVENT_SPACE_ID,
                            .type = CLAP_EVENT_PARAM_GESTURE_END,
                            .flags = {},
                        },
                        .param_id = param.address
                    };
                    out_events->try_push(out_events, &e.header);
                }
            },
        }, user_action);
    }
}

auto Clap_plugin::_handle_user_action(const User_action& action) -> void
{
    // Maintain host values immediately.
    if (const auto* a = std::get_if<Set_param>(&action)) {
        const auto& param = User_params::param_spec(a->address);
        const auto host_value = Value_conv::knob_to_host(a->value, param.semantics);
        _hostvalues[param.address].store(host_value, std::memory_order_relaxed);
    }
    [[maybe_unused]] const auto success = _from_ui.push(action);
    assert(success && "UI to processor queue full, increase queue size!");
}

} // namespace tiny