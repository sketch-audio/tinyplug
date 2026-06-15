#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "plugin.hpp"

#include <nlohmann/json.hpp>

namespace tiny::clap {

// MARK: - plugin

bool Plugin::init() noexcept
{
    return true;
}

bool Plugin::activate(double sampleRate, uint32_t /*minFrameCount*/, uint32_t /*maxFrameCount*/) noexcept
{
    // Reset kernel with sample rate only first time and then when sample rate changes.
    if (!_once || sampleRate != _sr) {
        _processor->reset(sampleRate);
        _latency = _processor->latency_samps();
        _sr = sampleRate;
        _once = true;

        _bypass.reset(static_cast<float>(sampleRate));
        _bypass.set_latency(_latency);
    }

#if TINY_HAS_WORKER
    _worker_runner.start(sampleRate);
#endif

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

void Plugin::deactivate() noexcept
{
}

bool Plugin::startProcessing() noexcept
{
    return true;
}

void Plugin::stopProcessing() noexcept
{
}

// MARK: - process

clap_process_status Plugin::process(const clap_process* process) noexcept
{
    this->_drain_worker_to_processor();

    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        const auto new_latency = *accepted_latency;
        _processor->handle_event(Accepted_latency{new_latency});
        _bypass.set_latency(new_latency);
        assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
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
    context.render_mode = _offline.load(std::memory_order_relaxed)
        ? Render_mode::Offline
        : Render_mode::Realtime;

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
        const auto doff = static_cast<double>(offset);

        context.musical_context = {
            .sample_pos = static_cast<int64_t>(sample_pos + doff),
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

    const auto can_skip = _bypass.can_skip_effect();

    if (can_skip) {
        // Manifest events until end of block.
        while (event) {
            this->_handle_host_event<true>(event);
            next_event();
        }
    }
    else {
        // Process with events.
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
    }

    auto in_buffers = [&]() {
        auto arr = std::array<const float*, max_ichannels>{};
        const auto& input_port = process->audio_inputs[0];
        assert(input_port.channel_count == static_cast<uint32_t>(_ichannels));
        for (size_t i = 0; i < _ichannels; ++i) {
            arr[i] = &input_port.data32[i][0];
        }
        return arr;
    }();

    auto out_buffers = [&]() {
        auto arr = std::array<float*, max_ochannels>{};
        const auto& output_port = process->audio_outputs[0];
        assert(output_port.channel_count == static_cast<uint32_t>(_ochannels));
        for (size_t i = 0; i < _ochannels; ++i) {
            arr[i] = &output_port.data32[i][0];
        }
        return arr;
    }();

    const auto min_channels = std::min(process->audio_inputs[0].channel_count, process->audio_outputs[0].channel_count);
    const auto num_channels = static_cast<size_t>(min_channels);
    _bypass.process({in_buffers.begin(), num_channels}, {out_buffers.begin(), num_channels}, frame_count);

    // Send exports.
    for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
        if (context.meters[i] != _last_meters[i]) {
            // Send export and cache.
            const auto value = context.meters[i];
            _meter_queue.push(Set_meter{.address = i, .value = value});
            _last_meters[i] = value;
        }
        _meters[i] = 0; // Reset for peak meters.
    }

    // Has the kernel proposed a new latency?
    if (const auto proposed_latency = context.propose_latency; proposed_latency/* && *proposed_latency != _latency*/) {
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

void Plugin::reset() noexcept
{
}

void Plugin::onMainThread() noexcept
{
}

bool Plugin::renderSetMode(clap_plugin_render_mode mode) noexcept
{
    _offline.store(mode == CLAP_RENDER_OFFLINE, std::memory_order_relaxed);
    return true;
}

const void* Plugin::extension(const char* /*id*/) noexcept
{
    return nullptr;
}

bool Plugin::enableDraftExtensions() const noexcept
{
    return false;
}

// MARK: - save state

bool Plugin::stateSave(const clap_ostream* stream) noexcept
{
    using namespace params;

    if (!stream) return false;

    const auto edit_state = _editor->save_state();
    const auto num_editor_items = static_cast<uint32_t>(edit_state.size());

    // Write header.
    const auto header = State_rules::Clap::Header{
        Plug_info::framework_code,
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_params,
        num_editor_items
    };
    {
        const auto total = sizeof(header);
        auto sent = size_t{};
        while (sent < total) {
            const auto n = stream->write(stream, header.data() + sent, total - sent);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
    }

    // Helpers — loop until full chunk accepted (hosts may accept fewer bytes than requested).
    auto write_value = [&](const auto& data) {
        const auto total = sizeof(data);
        auto sent = size_t{};
        const auto* ptr = reinterpret_cast<const char*>(&data);
        while (sent < total) {
            const auto n = stream->write(stream, ptr + sent, total - sent);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    auto write_container = [&](const auto& data) {
        // Write number of items.
        const auto num = static_cast<uint32_t>(data.size());
        {
            auto sent = size_t{};
            const auto* ptr = reinterpret_cast<const char*>(&num);
            while (sent < sizeof(num)) {
                const auto n = stream->write(stream, ptr + sent, sizeof(num) - sent);
                if (n <= 0) return false;
                sent += static_cast<size_t>(n);
            }
        }
        if (num == 0) return true;

        // Write items.
        const auto total = sizeof(data[0]) * num;
        auto sent = size_t{};
        const auto* ptr = reinterpret_cast<const char*>(data.data());
        while (sent < total) {
            const auto n = stream->write(stream, ptr + sent, total - sent);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    // --- Write the processor state ---
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        const auto& spec = User_params::param_spec(i);
        auto raw = _hostvalues[i].load(std::memory_order_relaxed);

        if (std::get_if<params::Semantics::Fixed>(&spec.semantics)) {
            raw = Value_helper::quantize(raw, spec.semantics); // Clamp fixed to step values.
        }

        const auto host_value = static_cast<float>(raw);
        const auto to_write = State_rules::is_persistent(spec) ? host_value : State_rules::no_value;

        if (!write_value(to_write)) {
            return false;
        }
    }
    // ---

    // --- Write the editor state ---
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
            case State_tag::Bool: {
               const auto value = std::get_if<bool>(&val);
               if (value && write_value(*value)) {
                   break;
               }
               return false;
            }
            case State_tag::Int: {
                const auto value = std::get_if<int32_t>(&val);
                if (value && write_value(*value)) {
                    break;
                }
                return false;
            }
            case State_tag::Double: {
                const auto value = std::get_if<double>(&val);
                if (value && write_value(*value)) {
                    break;
                }
                return false;
            }
            case State_tag::String: {
                const auto value = std::get_if<std::string>(&val);
                if (value && write_container(*value)) {
                    break;
                }
                return false;
            }
            default: {
                assert(false && "Unknown editor state type.");
                return false;
            }
        }
    }
    // ---

    // -- Write the host bypass value --
    const auto bypass_value = _bypass.is_bypassed() ? 1.f : 0.f;
    if (!write_value(bypass_value)) {
        return false;
    }

    return true;
}

// MARK: - load state

auto Plugin::_update_state(const Maybe_values<double>& knob_values, const State_map& editor_state) -> void
{
    using namespace params;

    // Notify kernel and view (if not an interface parameter).
    auto notify = [&](const auto& param, auto knob_value) {
        const auto can_notify = knob_value.has_value() && State_rules::is_persistent(param);
        if (can_notify) {
            this->_handle_user_action(Set_param{.address = param.address, .value = *knob_value});
            if (_view) {
                _view->set_param(param.address, *knob_value);
            }
        }
    };

    const auto num_stored_values = knob_values.size();

    if (num_params <= num_stored_values) {
        // Read as many values as we can.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            notify(param, knob_values[i]);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
            notify(param, knob_values[i]);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_stored_values; i < num_params; ++i) {
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
            const auto knob_value = Value_helper::default_value(param, Space::Knob);
            notify(param, std::optional<double>{knob_value});
        }
    }

    // Editor
    _editor->load_state(editor_state);

    _host->request_process(_host); // We're using process to flush.
}

bool Plugin::stateLoad(const clap_istream* stream) noexcept
{
    using namespace params;

    if (!stream) return false;

    auto header = State_rules::Clap::Header{};
    {
        const auto total = sizeof(header);
        auto got = size_t{};
        while (got < total) {
            const auto n = stream->read(stream, header.data() + got, total - got);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
    }

    // Validate header.
    assert(header[0] == Plug_info::framework_code && "Unexpected framework code.");
    assert(header[1] == Plug_info::manufacturer_code && "Unexpected manufacturer code.");
    assert(header[2] == Plug_info::plugin_code && "Unexpected plug-in code.");

    const auto num_stored_values = header[3];
    const auto num_stored_pairs = header[4];

    // Helpers — loop until full chunk received (hosts may deliver fewer bytes than requested).
    auto read_value = [&](auto& data) {
        const auto total = sizeof(data);
        auto got = size_t{};
        auto* ptr = reinterpret_cast<char*>(&data);
        while (got < total) {
            const auto n = stream->read(stream, ptr + got, total - got);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    };

    auto read_container = [&](auto& data) {
        auto num = uint32_t{};
        {
            auto got = size_t{};
            auto* ptr = reinterpret_cast<char*>(&num);
            while (got < sizeof(num)) {
                const auto n = stream->read(stream, ptr + got, sizeof(num) - got);
                if (n <= 0) return false;
                got += static_cast<size_t>(n);
            }
        }
        data.resize(num);
        if (num == 0) return true;
        const auto total = sizeof(data[0]) * num;
        auto got = size_t{};
        auto* ptr = reinterpret_cast<char*>(data.data());
        while (got < total) {
            const auto n = stream->read(stream, ptr + got, total - got);
            if (n <= 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    };

    // Read processor state into temporary vector.
    auto stored_values = Maybe_values<double>(static_cast<size_t>(num_stored_values), std::nullopt);
    for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
        // Read floats from state.
        auto host_value = float{};
        if (!read_value(host_value)) {
            return false;
        }

        // Do we have a meaningful value?
        if (host_value != State_rules::no_value) {
            const auto& spec = User_params::param_spec(i);
            const auto knob_value = Value_helper::host_to_knob(static_cast<double>(host_value), spec.semantics);
            stored_values[i] = knob_value;
        }
    }

    // Read editor state into temporary map.
    auto edit_state = State_map{};
    for (auto i = decltype(num_stored_pairs){}; i < num_stored_pairs; ++i) {
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
            case State_tag::Bool: {
                auto v = bool{};
                if (!read_value(v)) {
                    return false;
                }
                value = v;
                break;
            }
            case State_tag::Int: {
                auto v = int32_t{};
                if (!read_value(v)) {
                    return false;
                }
                value = v;
                break;
            }
            case State_tag::Double: {
                auto v = double{};
                if (!read_value(v)) {
                    return false;
                }
                value = v;
                break;
            }
            case State_tag::String: {
                auto v = std::string{};
                if (!read_container(v)) {
                    return false;
                }
                value = std::move(v);
                break;
            }
            default:
                break;
        }

        edit_state.emplace(std::move(key), std::move(value));
    }

    // Try to read the host bypass value.
    auto bypass_value = float{};
    if (read_value(bypass_value)) {
        const auto bypass = bypass_value >= 0.5f;
        _bypass.set_bypassed(bypass);
    }
    else {
        //_bypass.set_bypassed(false);
    }

    this->_update_state(stored_values, edit_state);

    return true;
}

// MARK: - preset load

bool Plugin::presetLoadFromLocation(uint32_t location_kind, const char* location, const char* load_key) noexcept
{
    if (location_kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) return false;
    if (!location) return false;

    const auto preset_path = std::filesystem::path{location};
    if (!std::filesystem::exists(preset_path)) {
        return false;
    }

    // load to json
    auto file = std::ifstream{preset_path};
    if (file) {
        using Json = nlohmann::ordered_json;
        auto json = Json{};
        try {
            file >> json;
        } catch (...) {
            return false;
        }
        const auto params = _state_adapter.param_values(json);
        const auto editor_state = _state_adapter.editor_state(json);
        this->_update_state(params, editor_state);

        // Tell the host
        if (auto* preset_ext = (const clap_host_preset_load_t*)_host->get_extension(_host, CLAP_EXT_PRESET_LOAD); preset_ext) {
            preset_ext->loaded(_host, location_kind, location, load_key);
        }
        return true;
    }

    return false;
}

// MARK: - audio ports

uint32_t Plugin::audioPortsCount(bool isInput) const noexcept
{
    return isInput ? (Plug_info::wants_sidechain ? 2 : 1) : 1;
}

bool Plugin::audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept
{
    if (!info) return false;

    const auto is_main = (index == 0);
    const char* port_name = isInput ? (is_main ? "Input" : "Sidechain") : "Output";

    const auto channel_count = isInput ? (is_main ? _ichannels : _schannels) : _ochannels;
    const auto port_type = (channel_count == 2) ? CLAP_PORT_STEREO : CLAP_PORT_MONO;

    *info = {};
    info->id = index;
    std::snprintf(info->name, CLAP_NAME_SIZE, "%s", port_name);
    info->flags = is_main ? CLAP_AUDIO_PORT_IS_MAIN : uint32_t{};
    info->channel_count = static_cast<uint32_t>(channel_count); // 
    info->port_type = port_type;
    info->in_place_pair = CLAP_INVALID_ID; // No in-place.

    return true;
}

// MARK: - configurable audio ports

bool Plugin::configurableAudioPortsCanApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) const noexcept
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

bool Plugin::configurableAudioPortsApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) noexcept
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

uint32_t Plugin::paramsCount() const noexcept
{
    return num_params + 1; // Bypass presents at the end.
}

bool Plugin::paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept
{
    using namespace params;

    if (!info) return false;

    if (paramIndex == num_params) {
        *info = {};
        info->id = Reserved::bypass_id;
        info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_AUTOMATABLE;
        info->cookie = nullptr;
        std::snprintf(info->name, CLAP_NAME_SIZE, "%s", "Bypass");
        info->min_value = 0;
        info->max_value = 1;
        info->default_value = 0;
        return true;
    }

    // The index is the order of appearance in the UI, and isn't necessarily the same as the id.
    if (paramIndex >= num_params) return false;

    const auto& params = User_params::param_specs(Param_order::Presentation); // Report params in presentation order!

    const auto& param = params[paramIndex];
    const auto& path = _modules[paramIndex];

    *info = {}; // Clear.
    info->id = param.address;
    info->flags = [policy = param.policy]() {
        using enum params::Policy;
        switch (policy) {
            case Automation: return uint32_t{CLAP_PARAM_IS_AUTOMATABLE};
            case Control: return uint32_t{}; // Do any hosts actually show a control here?
            case Hidden: return uint32_t{CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY};
            case Interface: return uint32_t{CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY};
            default: return uint32_t{};
        }
    }();
    info->cookie = nullptr;
    std::snprintf(info->name, CLAP_NAME_SIZE, "%s", std::string{param.name}.c_str());
    std::snprintf(info->module, CLAP_NAME_SIZE, "%s", path.c_str());

    // CLAP uses host values.
    // Set min, max, default based on semantics.
    std::visit(Inline_visitor{
        [&](const params::Semantics::Bool& b) {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = 0;
            info->max_value = 1;
            info->default_value = b.def_val ? 1 : 0;
        },
        [&](const params::Semantics::List& l) {
            info->flags |= (CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM);
            info->min_value = 0;
            info->max_value = static_cast<double>(l.items.size() - 1);
            info->default_value = static_cast<double>(l.def_val);
        },
        [&](const params::Semantics::Int& i) {
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = i.min_val;
            info->max_value = i.max_val;
            info->default_value = i.def_val;
        },
        [&](const params::Semantics::Fixed& f) {
            info->min_value = f.min_val;
            info->max_value = f.max_val;
            info->default_value = f.def_val;
        },
        [&](const params::Semantics::Real& r) {
            info->min_value = 0;
            info->max_value = 1;
            info->default_value = Value_helper::plain_to_knob(r.def_val, r);
        },
    }, param.semantics);

    return true;
}

bool Plugin::paramsValue(clap_id paramId, double* value) noexcept
{
    using namespace params;

    if (paramId == Reserved::bypass_id) {
        const auto bypass = _bypass.is_bypassed();
        *value = bypass ? 1. : 0.;
        return true;
    }

    if (paramId >= num_params) return false;

    const auto& spec = User_params::param_spec(paramId);
    const auto raw = _hostvalues[paramId].load(std::memory_order_relaxed);
    auto result = raw;

    if (std::get_if<params::Semantics::Fixed>(&spec.semantics)) {
        // Snap to nearest step so paramsValue() round-trips through state save/load.
        result = Value_helper::quantize(raw, spec.semantics);
    }

    *value = static_cast<double>(static_cast<float>(result)); // We dump state as floats so the cast makes sure we round-trip through state save/load.
    return true;
}

bool Plugin::paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept
{
    if (paramId == Reserved::bypass_id) {
        const auto str = value >= 0.5 ? "On" : "Off";
        std::snprintf(display, size, "%s", str);
        display[size - 1] = '\0'; // In case str is longer than display.
        return true;
    }

    if (paramId >= num_params || !display) return false;

    const auto& param = User_params::param_spec(paramId);
    const auto str = Host_formatter::to_string(value, param.semantics);
    std::snprintf(display, size, "%s", str.c_str());
    display[size - 1] = '\0'; // In case str is longer than display.

    return true;
}

bool Plugin::paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept
{
    using namespace params;

    if (!display) return false;

    if (paramId == Reserved::bypass_id) {
        if (std::strcmp(display, "On")  == 0) { *value = 1.0; return true; }
        if (std::strcmp(display, "Off") == 0) { *value = 0.0; return true; }
        return false;
    }

    if (paramId >= num_params) return false;

    const auto& param = User_params::param_spec(paramId);
    const auto str = std::string{display};

    if (const auto plain = Host_formatter::to_value(str, param.semantics)) {
        const auto clamped = Value_helper::clamp(*plain, param.semantics); // Clamp fixes some round-tripping issues flagged by the validator.
        *value = Value_helper::plain_to_host(clamped, param.semantics);
        return true;
    }

    return false;
}

void Plugin::paramsFlush(const clap_input_events* in, const clap_output_events* /*out*/) noexcept
{
    if (!in) return;

    const auto size = in->size(in);

    for (auto i = decltype(size){}; i < size; ++i) {
        const auto* event = in->get(in, i);
        this->_handle_host_event<false>(event);
    }
}

// MARK: - gui

bool Plugin::guiIsApiSupported(const char* api, bool isFloating) noexcept
{
#if TINY_PLATFORM_MACOS
    constexpr auto gui_preferred_api = CLAP_WINDOW_API_COCOA;
#elif TINY_PLATFORM_WINDOWS
    constexpr auto gui_preferred_api = CLAP_WINDOW_API_WIN32;
#endif

    return !isFloating && strcmp(api, gui_preferred_api) == 0;
}

bool Plugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept
{
#if TINY_PLATFORM_MACOS
    constexpr auto gui_preferred_api = CLAP_WINDOW_API_COCOA;
#elif TINY_PLATFORM_WINDOWS
    constexpr auto gui_preferred_api = CLAP_WINDOW_API_WIN32;
#endif

    *api = gui_preferred_api;
    *isFloating = false;

    return true;
}

bool Plugin::guiCreate(const char* /*api*/, bool /*isFloating*/) noexcept
{
    using namespace params;
    // Make the UI connection.
    auto receiver = Ui_receiver{
        .get_param = [this](auto id) {
            const auto& param = User_params::param_spec(id);
            const auto host_value = _hostvalues[id].load(std::memory_order_relaxed);
            const auto knob_value = Value_helper::host_to_knob(host_value, param.semantics);
            return knob_value;
        },
        .pop_meter = [this](auto& event) {
            return _meter_queue.pop(event);
        },
        .action_handler = [this](auto& action) {
            this->_handle_user_action(action);
        }
    };

    _view = std::make_unique<View>(View::Deps{
        .editor = &(*_editor),
        .receiver = std::move(receiver),
        .tasks = &_tasks,
#if TINY_HAS_WORKER
        .drain_worker_to_editor = [this]() { this->_drain_worker_to_editor(); }
#endif
    });
    _view->on_create();
    return true;
}

void Plugin::guiDestroy() noexcept
{
    _view->on_destroy();
    _view = nullptr;
}

bool Plugin::guiSetScale(double /*scale*/) noexcept
{
    return true;
}

bool Plugin::guiShow() noexcept
{
    if (!_view) return false;
    _view->on_show();
    return true;
}

bool Plugin::guiHide() noexcept
{
    if (!_view) return false;
    _view->on_hide();
    return true;
}

bool Plugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept
{
    _view->get_size(width, height);
    return true;
}

bool Plugin::guiCanResize() const noexcept
{
    return true;
}

bool Plugin::guiGetResizeHints(clap_gui_resize_hints_t* /*hints*/) noexcept
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

bool Plugin::guiAdjustSize(uint32_t* /*width*/, uint32_t* /*height*/) noexcept
{
    return true;
}

bool Plugin::guiSetSize(uint32_t width, uint32_t height) noexcept
{
    return _view->set_size(width, height);
}

void Plugin::guiSuggestTitle(const char* /*title*/) noexcept
{
    // floating only
}

bool Plugin::guiSetParent(const clap_window* window) noexcept
{
    if (!_view) return false;
    return _view->set_parent(window);
}

bool Plugin::guiSetTransient(const clap_window* /*window*/) noexcept
{
    return false; // floating only
}

// MARK: - latency

uint32_t Plugin::latencyGet() const noexcept
{
    return _latency;
}

// MARK: - tail

uint32_t Plugin::tailGet() const noexcept
{
    // CLAP will interpret anything >= INT32_MAX as infinite.
    return _tail;
}

// MARK: - private

auto Plugin::_handle_host_flushed() -> void
{
    auto kernel_event = Render_event{};
    while (_from_flush.pop(kernel_event)) {
        _processor->handle_event(kernel_event);
    }
}

auto Plugin::_handle_user_actions(const clap_output_events_t* out_events) -> void
{
    // The host only needs to know about changes where there might be automation or a control in the host UI.
    auto wants_host_notify = [](params::Policy policy) {
        using enum params::Policy;
        return policy == Automation || policy == Control;
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
                using namespace params;

                const auto& param = User_params::param_spec(a.address);

                if (wants_host_notify(param.policy)) {
                    const auto host_value = Value_helper::knob_to_host(a.value, param.semantics);
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

                const auto plain_value = Value_helper::knob_to_plain(a.value, param.semantics);
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
            [](const auto&) {}
        }, user_action);
    }
}

auto Plugin::_handle_user_action(const User_action& action) -> void
{
    using namespace params;
    // Maintain host values immediately.
    if (const auto* a = std::get_if<Set_param>(&action)) {
        const auto& param = User_params::param_spec(a->address);
        const auto host_value = Value_helper::knob_to_host(a->value, param.semantics);
        _hostvalues[param.address].store(host_value, std::memory_order_relaxed);
    }
    [[maybe_unused]] const auto success = _from_ui.push(action);
    assert(success && "UI to processor queue full, increase queue size!");
}

} // namespace tiny::clap