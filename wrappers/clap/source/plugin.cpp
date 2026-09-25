#include "plugin.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

namespace tiny::clap {

// MARK: - plugin

bool Plugin::init() noexcept
{
    return true;
}

bool Plugin::activate(double sampleRate, uint32_t /*minFrameCount*/, uint32_t maxFrameCount) noexcept
{
    using namespace params;

    _silence.assign(maxFrameCount, 0.f);

    const auto sr_changed = (_sr != sampleRate); // Need this below.
    _sr = sampleRate;

#if TINY_HAS_WORKER
    _worker_runner.start(sampleRate);
#endif

    auto notify = [this]() {
        auto ext = _host->get_extension(_host, CLAP_EXT_LATENCY);
        auto latency_ext = static_cast<const clap_host_latency_t*>(ext);
        if (latency_ext) {
            latency_ext->changed(_host);
        }
    };

    // Are we here because the processor wanted a latency change?
    const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (pending_latency.has_value() && !sr_changed) {
        _accepted_latency.store(*pending_latency, std::memory_order_release); // The kernel should manifest on the next process.
        _latency = *pending_latency;
        notify();

        // Skip configure.
        return true;
    }

    // Finish clearing handshake state for reconfigure.
    _accepted_latency.store(std::nullopt, std::memory_order_release);

    // Get the initial state for this configuration.
    auto config_values = std::array<double, num_params>{};
    for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
        // Convert to plain space.
        const auto& param = User_params::param_spec(addr);
        const auto host = _hostvalues[addr].load(std::memory_order_relaxed);
        const auto plain = Value_helper::host_to_plain(host, param.semantics);
        config_values[addr] = plain;
    }

    _processor->configure(process::Config{
        .sr = sampleRate,
        .params = config_values
    });
    _latency = _processor->latency_samps();

    _bypass.reset(static_cast<float>(sampleRate));
    _bypass.set_max_latency(process::max_latency_of(*_processor));
    _bypass.set_latency(_latency);

    // Did activate result in a changed latency?
    const auto latency_changed = (_latency != _reported_latency.load(std::memory_order_relaxed));
    if (latency_changed) {
        _reported_latency.store(_latency, std::memory_order_relaxed);
        notify();
    }

    return true;
}

void Plugin::deactivate() noexcept
{
    _notes.clear(); // The audio thread is stopped; anything sounding downstream is released at the next block.
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
    const auto denormals = Denormal_guard{}; // Restores the host's FP mode on the way out.

    this->_drain_worker_to_processor();

    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        const auto new_latency = *accepted_latency;
        _processor->reset(process::Reset::Latency{new_latency});
        _bypass.set_latency(new_latency);
        assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
    }

    // Discontinuity requested by the host — forget history before anything this block
    // delivers lands.
    if (_needs_clear.exchange(false, std::memory_order_relaxed)) {
        _processor->reset(process::Reset::Hard{});
        _notes.clear();
        _bypass.clear();    // Its delay lines hold pre-seek dry audio.
        _bypass.snap();
    }

    // Resolved once so both queues agree on whether this block is a resync.
    const auto needs_resync = _needs_resync.exchange(false, std::memory_order_relaxed);

    // Manifest, on helper functions will discard their queues.
    if (needs_resync) {
        using namespace params;
        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
            const auto host_value = _hostvalues[addr].load(std::memory_order_relaxed);
            const auto& spec = User_params::param_spec(addr);
            const auto plain = Value_helper::host_to_plain(host_value, spec.semantics);
            _processor->handle(process::Event::Set{.address = addr, .value = plain});
        }

        // Manifest immediately — a client reading realized state (e.g. an open editor)
        // shouldn't see stale values for the whole inactive/sleeping stretch.
        _processor->reset(process::Reset::Soft{});
    }
    this->_handle_host_flushed();
    this->_handle_user_actions(process->out_events, needs_resync);

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
    auto context = process::Dsp_context{.propose_latency = {}};
#if TINY_HAS_STATE
    auto state_block = _state.begin_block(); // Applies a staged edit now; publishes when process returns.
    context.state = state::Access_for<models::Resolved::State>{&state_block};
#endif
#if TINY_HAS_METERS
    context.meters = _meters.scratch();
#endif
#if TINY_HAS_BLOCKS
    context.blocks = blocks::Writer{&_blocks};
#endif
    context.render_mode = _offline.load(std::memory_order_relaxed) ? process::Render_mode::Offline : process::Render_mode::Realtime;

    // We need to resync on render mode edge.
    // Realtime <-> offline is a discontinuity: the audio either side is unrelated, so
    // forget history as well as manifesting values.
    if (_last_render_mode != context.render_mode) {
        _processor->reset(process::Reset::Hard{});
        _notes.clear();
        _bypass.clear();    // Its delay lines hold pre-bounce dry audio.
        _bypass.snap();
        _last_render_mode = context.render_mode;
    }

    // Resolve transport.
    const auto block_context = this->_resolve_transport(process);

    // A bypassed note effect forwards its input instead of running; an edge releases what was sounding.
#if TINY_HAS_NOTES_OUT
    const auto bypassed = _notes.bypassed(_bypass.is_bypassed());
    _passing = bypassed && Plug_info::kind == Plugin_kind::Note_effect;
#else
    const auto bypassed = false;
#endif
    (void)bypassed;

#if TINY_HAS_NOTES_IN
    // The editor's notes land at the top of the block.
    _notes.drain_editor([this](const process::Input& input) { _input(input, 0); });
#endif

    // So we can process with an offset.
    auto do_process = [this, &process, &context, &block_context, bypassed](size_t num_frames, size_t offset) {
        // Assign buffer ptrs.
        if constexpr (Plug_info::Wants::audio_in) {
            const auto& input_port = process->audio_inputs[0];
            assert(input_port.channel_count == static_cast<uint32_t>(_ichannels));
            for (size_t i = 0; i < _ichannels; ++i) {
                _ibuffers[i] = &input_port.data32[i][offset];
            }
        }

        if constexpr (Plug_info::Wants::audio_out) {
            auto& output_port = process->audio_outputs[0];
            assert(output_port.channel_count == static_cast<uint32_t>(_ochannels));
            for (size_t i = 0; i < _ochannels; ++i) {
                _obuffers[i] = &output_port.data32[i][offset];
            }
        }

#if TINY_HAS_NOTES_OUT
        _notes.outbox().begin_slice(static_cast<int64_t>(offset), static_cast<int64_t>(num_frames));
        context.notes = _notes.writer(bypassed);
#else
        (void)bypassed;
#endif
        (void)process; // Unused when the plug-in carries no audio.

        if constexpr (Plug_info::wants_sidechain) {
            const auto& sidechain_port = process->audio_inputs[sidechain_index];
            assert(sidechain_port.channel_count == static_cast<uint32_t>(_schannels));
            for (size_t i = 0; i < _schannels; ++i) {
                _sbuffers[i] = &sidechain_port.data32[i][offset];
            }
        }

        // Advance the block's musical context to this segment's offset.
        const auto beat_off = process::frames_to_beats(static_cast<int64_t>(offset), block_context.tempo_ideal, _sr);
        context.musical_context = block_context;
        context.musical_context.sample_pos = block_context.sample_pos + static_cast<int64_t>(offset);
        context.musical_context.beat_pos = block_context.beat_pos + beat_off;

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

    // Interpret zero frame count as a flush.
    const auto renders_audio = frame_count > 0;

    // Resuming from a stretch where advance_rampers() didn't run (can_skip skips
    // process()) — settle before this block's own automation lands, not after.
    if (_was_skipped && !can_skip) {
        _processor->reset(process::Reset::Soft{});
    }
    _was_skipped = can_skip;

    if (can_skip || !renders_audio) {
        // Manifest events until end of block.
        auto delivered = false;
        while (event) {
            this->_handle_host_event<true>(event);
            next_event();
            delivered = true;
        }
        if (!renders_audio && delivered) {
            _processor->reset(process::Reset::Soft{});
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

            // Clamp
            const auto frames_until_event = event->time > now ? std::min(event->time - now, remaining) : uint32_t{};

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

    // Host bypass. Without an audio input the dry signal is silence, so bypass fades out.
    if constexpr (Plug_info::Wants::audio_out) {
        auto in_buffers = [&]() {
            auto arr = std::array<const float*, max_ochannels>{};
            for (size_t i = 0; i < _ochannels; ++i) {
                if constexpr (Plug_info::Wants::audio_in) arr[i] = &process->audio_inputs[0].data32[std::min(i, _ichannels - 1)][0];
                else arr[i] = _silence.data();
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

        const auto num_channels = Plug_info::Wants::audio_in ? std::min(_ichannels, _ochannels) : _ochannels;
        const auto frames = Plug_info::Wants::audio_in ? size_t{frame_count} : std::min<size_t>(frame_count, _silence.size());
        _bypass.process({in_buffers.begin(), num_channels}, {out_buffers.begin(), num_channels}, frames);
    }

#if TINY_HAS_NOTES_OUT
    _send_notes(process->out_events);
#endif

    // Send exports. Quiet during an offline bounce, and quiet on a flush block
    // (frames_count == 0) which ran no audio and so has measured nothing — the
    // publisher still resets peaks either way, so neither can hoard a spike.
    const auto offline = (context.render_mode == process::Render_mode::Offline);
#if TINY_HAS_METERS
    _meters.publish(offline || !renders_audio, [this](uint32_t address, float value) {
        _mailbox.post(address, value);
        return true; // A slot array has no capacity to refuse.
    });
#endif
#if TINY_HAS_BLOCKS
    _blocks.transmit(offline, [this](auto address, const auto& frame) {
        return _block_mailbox.post(address, frame);
    });
#endif

    // Did the processor propose a new (unreported) latency? Quiet during an offline
    // bounce for the same reason the meters are: `request_restart` makes the host
    // deactivate and reactivate mid-render, and a bounce cannot usefully renegotiate
    // delay compensation. The pending value survives for the next `latencyGet`.
    const auto reported = _reported_latency.load(std::memory_order_relaxed);
    if (const auto proposed = _bypass.admit(context.propose_latency); proposed.has_value() && *proposed != reported) {
        // Set pending latency, mark reported, and request a restart.
        _pending_latency.store(*proposed, std::memory_order_release);
        _reported_latency.store(*proposed, std::memory_order_relaxed);
        if (!offline) _host->request_restart(_host);
    }

    const auto tail = _processor->tail_samps();
    if (tail != _tail.load(std::memory_order_relaxed)) {
        _tail.store(tail, std::memory_order_relaxed); // Before `changed`: the host may read it right away.
        const auto* tail_ext = (const clap_host_tail*)_host->get_extension(_host, CLAP_EXT_TAIL);
        if (tail_ext) tail_ext->changed(_host);
    }

    return CLAP_PROCESS_CONTINUE;
}

auto Plugin::_resolve_transport(const clap_process* process) -> process::Musical_context
{
    const auto* transport = process->transport;

    // If we can't get a transport, at least keep the sample position advancing.
    if (!transport) {
        const auto steady = process->steady_time;
        const auto sample_pos = steady >= 0 ? steady : _free_run_pos;
        _free_run_pos += static_cast<int64_t>(process->frames_count);

        return process::Musical_context{.sample_pos = sample_pos}; // Defaults: 120bpm, 4/4, stopped.
    }

    // We will derive the sample time from the time in seconds.
    const auto sec_pos = static_cast<double>(transport->song_pos_seconds) / CLAP_SECTIME_FACTOR;
    const auto sample_pos = std::round(sec_pos * _sr);
    const auto tempo = transport->tempo > 0 ? transport->tempo : 120.; // Never divide by zero.

    const auto flags = transport->flags;
    const auto has_flag = [](auto x, auto f) { return (x & f) > 0; };

    return process::Musical_context{
        .sample_pos = static_cast<int64_t>(sample_pos),
        .beat_pos = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR,
        .cycle_start = static_cast<double>(transport->loop_start_beats) / CLAP_BEATTIME_FACTOR,
        .cycle_end = static_cast<double>(transport->loop_end_beats) / CLAP_BEATTIME_FACTOR,
        .tempo_ideal = tempo,
        .time_sig = {transport->tsig_num, transport->tsig_denom},
        .transport_state = {
            .moving = has_flag(flags, CLAP_TRANSPORT_IS_PLAYING),
            .cycling = has_flag(flags, CLAP_TRANSPORT_IS_LOOP_ACTIVE),
            .recording = has_flag(flags, CLAP_TRANSPORT_IS_RECORDING)
        }
    };
}

// The host is about to feed us discontinuous audio (seek, loop jump, un-mute, offline
// render). Deferred to `process` so it can't race a block already in flight.
void Plugin::reset() noexcept
{
    _needs_clear.store(true, std::memory_order_relaxed);
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

    auto edit_state = _editor->save_state();
    drop_reserved_keys(edit_state);

    // Inject the framework-owned editor window size (from our own cache) so the window
    // reopens pre-sized. The app editor never emits these keys.
    if (_last_size) {
        editor_size_state::inject(edit_state, _last_size->w, _last_size->h);
    }

    const auto num_editor_items = static_cast<uint32_t>(edit_state.size());

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
    
    // Write the header (as value).
    const auto header = State_rules::Clap::Header{
        Plug_info::framework_code,
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_params,
        num_editor_items
    };
    if (!write_value(header)) {
        return false;
    }

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

#if TINY_HAS_STATE
    // -- Write the state record, last so older builds stop before it --
    auto doc = models::Resolved::State{};
    _state.snapshot(doc);
    const auto record = state::encode_record(doc);

    auto sent = size_t{};
    const auto* ptr = reinterpret_cast<const char*>(record.data());
    while (sent < record.size()) {
        const auto n = stream->write(stream, ptr + sent, record.size() - sent);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
#endif

    return true;
}

// MARK: - load state

auto Plugin::_update_state(const Maybe_values<double>& knob_values, const State_map& editor_state,
                           [[maybe_unused]] std::span<const std::byte> record, bool bypass_changed) -> void
{
    using namespace params;

#if TINY_HAS_STATE
    const auto doc = state::decode_record_or_default<models::Resolved::State>(record);
    _state.on_session_load(doc);
#endif

    // Snapshot for host-load undo capture (knob space, pre-load).
    const auto before = _snapshot_knob_params();

    // Notify kernel and view (if not an interface parameter).
    auto notify = [&](const auto& param, auto knob_value) {
        const auto can_notify = knob_value.has_value() && State_rules::is_persistent(param);
        if (can_notify) {
            this->_load_param(param.identity.address, *knob_value);
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

    // Editor. Prime the framework-owned size cache (present only in session state, not
    // presets) so a subsequent guiCreate opens pre-sized, then strip the keys so the
    // app editor never sees them.
    auto editor_only = editor_state;
    if (const auto size = editor_size_state::extract(editor_only)) {
        _last_size = Rect_size{size->first, size->second};
    }
    editor_size_state::strip(editor_only);
    _editor->load_state(editor_only);

    // Record the host load as one coalesced undo step (works editor open or closed)
    // and notify the editor synchronously, so it can fold its marker params into the
    // same step via add_param. Dispatching here (not the view loop) means each load
    // notifies on its own step, even with the window closed — no intermediate is lost.
    const auto after = _snapshot_knob_params();
    auto changes = std::vector<Set_param>{};
    _undo_history.push_host_load(before, after, changes);
#if TINY_HAS_STATE
    _state_link.load(doc); // Into the same step.
#endif

    auto add_param = [this](uint32_t addr, double knob) {
        if (addr >= num_params) return;
        const auto& spec = User_params::param_spec(addr);
        const auto from = Value_helper::host_to_knob(_hostvalues[addr].load(std::memory_order_relaxed), spec.semantics);
        this->_load_param(addr, knob);                   // With the load's own values.
        _undo_history.amend_host_load(addr, from, knob); // Fold into the load's single step.
    };
    _editor->notify(Host_event{Host_preset_loaded{
        .changes = changes,
        .params = after,
        .add_param = add_param,
    }});

    // Notify if host values changed, bypass included.
    const auto settled = _snapshot_knob_params();
    const auto values_changed = bypass_changed || !std::ranges::equal(before, settled);

    if (values_changed) {
        if (auto* params_ext = (const clap_host_params_t*)_host->get_extension(_host, CLAP_EXT_PARAMS); params_ext) {
            params_ext->rescan(_host, CLAP_PARAM_RESCAN_VALUES);
        }
    }

    _host->request_process(_host); // We're using process to flush.
}

bool Plugin::stateLoad(const clap_istream* stream) noexcept
{
    try {
        return this->_read_state_chunk(stream);
    }
    catch (...) {
        return false;
    }
}

auto Plugin::_read_state_chunk(const clap_istream* stream) -> bool
{
    using namespace params;

    if (!stream) return false;

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

    auto header = State_rules::Clap::Header{};
    if (!read_value(header)) {
        return false;
    }

    // Validate the header for real, not just in debug. Hosts hand us chunks from other
    // plug-ins, truncated session files, and (in the validator's case) megabytes of
    // random bytes; every count below this point is attacker-controlled until checked.
    if (header[0] != Plug_info::framework_code) return false;
    if (header[1] != Plug_info::manufacturer_code) return false;
    if (header[2] != Plug_info::plugin_code) return false;

    const auto num_stored_values = header[3];
    const auto num_stored_pairs = header[4];

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

        // Chunk reads in case we got garbage.
        constexpr auto slice_items = size_t{4096};
        data.clear();

        auto done = size_t{};
        while (done < num) {
            const auto want = std::min<size_t>(slice_items, num - done);
            data.resize(done + want);

            const auto total = sizeof(data[0]) * want;
            auto got = size_t{};
            auto* ptr = reinterpret_cast<char*>(data.data() + done); // After resize; it may reallocate.
            while (got < total) {
                const auto n = stream->read(stream, ptr + got, total - got);
                if (n <= 0) return false;
                got += static_cast<size_t>(n);
            }
            done += want;
        }
        return true;
    };

    const auto usable_values = std::min<size_t>(num_stored_values, num_params);
    auto stored_values = Maybe_values<double>(usable_values, std::nullopt);

    for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
        // Read floats from state.
        auto host_value = float{};
        if (!read_value(host_value)) {
            return false;
        }

        // Keep stream aligned but don't write into stored values if the chunk has more values than we can use.
        if (i >= num_params) continue;

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
                return false;
        }

        edit_state.emplace(std::move(key), std::move(value));
    }

    // Try to read the host bypass value.
    auto bypass_value = float{};
    const auto has_bypass = read_value(bypass_value);
    const auto bypass_before = _bypass.is_bypassed();
    if (has_bypass) {
        const auto bypass = bypass_value >= 0.5f;
        _bypass.set_bypassed(bypass);
    }
    else {
        //_bypass.set_bypassed(false);
    }

    // The state record follows the bypass; a session without one loads the default document.
    auto record = std::vector<std::byte>{};
#if TINY_HAS_STATE
    if (has_bypass) {
        record = state::read_record([&](std::byte* out, size_t size) {
            auto got = size_t{};
            while (got < size) {
                const auto n = stream->read(stream, out + got, size - got);
                if (n <= 0) return false;
                got += static_cast<size_t>(n);
            }
            return true;
        });
    }
#endif

    this->_update_state(stored_values, edit_state, record, _bypass.is_bypassed() != bypass_before);

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
        const auto record = _state_adapter.state_record(json);
        this->_update_state(params, editor_state, record);

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
    if (isInput) return (Plug_info::Wants::audio_in ? 1 : 0) + (Plug_info::wants_sidechain ? 1 : 0);
    return Plug_info::Wants::audio_out ? 1 : 0;
}

bool Plugin::audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept
{
    if (!info) return false;
    if (index >= audioPortsCount(isInput)) return false;

    // Without a main input, an instrument's only input port is its sidechain.
    const auto is_main = (index == 0) && (!isInput || Plug_info::Wants::audio_in);
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

    // A request describes changes to specific ports, not a complete restatement, so seed from the
    // current configuration. Starting at zero rejects any request that simply omits a port — which
    // for a sidechain plug-in is nearly all of them.
    auto ichannels = static_cast<uint32_t>(_ichannels);
    auto schannels = static_cast<uint32_t>(_schannels);
    auto ochannels = static_cast<uint32_t>(_ochannels);

    auto check_port_type = [](const clap_audio_port_configuration_request& request) {
        if (!request.port_type) {
            return request.channel_count == 1 || request.channel_count == 2;
        }
        const auto mono_is_mono = (request.channel_count == 1 && strcmp(request.port_type, CLAP_PORT_MONO) == 0);
        const auto stereo_is_stereo = (request.channel_count == 2 && strcmp(request.port_type, CLAP_PORT_STEREO) == 0);
        return mono_is_mono || stereo_is_stereo;
    };

    const auto requests_ = std::span{requests, static_cast<size_t>(request_count)};
    for (const auto& request : requests_) {
        if (!check_port_type(request)) return false;

        const auto is_main = (request.port_index == 0) && (!request.is_input || Plug_info::Wants::audio_in);
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
    // An instrument has no main input: only its output's width is asked about.
    if constexpr (!Plug_info::Wants::audio_in) ichannels = ochannels;
    const auto wants_mono = (ichannels == 1 && ochannels == 1);
    const auto wants_stereo = (ichannels == 2 && ochannels == 2);

    const auto config_ok = (wants_stereo || (Plug_info::can_process_mono && wants_mono)) && sidechain_ok;
    return config_ok;
}

bool Plugin::configurableAudioPortsApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) noexcept
{
    if (!requests) return false;

    if (request_count == 0) return true; // No change.

    // Don't apply something we just said we couldn't.
    if (!this->configurableAudioPortsCanApplyConfiguration(requests, request_count)) {
        return false;
    }

    const auto requests_ = std::span{requests, static_cast<size_t>(request_count)};
    for (const auto& request : requests_) {
        const auto is_main = (request.port_index == 0) && (!request.is_input || Plug_info::Wants::audio_in);
        if (request.is_input && is_main) {
            _ichannels = std::min<size_t>(request.channel_count, max_ichannels);
        }
        else if (is_main) {
            _ochannels = std::min<size_t>(request.channel_count, max_ochannels);
        }
        else if (request.is_input) {
            _schannels = std::min<size_t>(request.channel_count, max_schannels);
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
    info->id = param.identity.address;
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
    std::snprintf(info->name, CLAP_NAME_SIZE, "%s", param.name.c_str());
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
#if TINY_HAS_METERS
        .read_meters = [this](std::span<float> out) {
            _mailbox.read(out);
        },
#endif
#if TINY_HAS_BLOCKS
        .read_blocks = [this](blocks::Frames<models::Resolved::Blocks>& out) {
            _block_mailbox.read(out);
        },
#endif
        .action_handler = [this](auto& action) {
            this->_handle_user_action(action);
        },
#if TINY_HAS_STATE
        .sync_state = [this]() { _state_link.sync(); },
#endif
    };

    _view = std::make_unique<View>(View::Deps{
        .editor = &(*_editor),
        .receiver = std::move(receiver),
        .tasks = &_tasks,
        .undo_history = &_undo_history,
        .actions = &_actions,
        .initial_size = _last_size.value_or(User_editor::preferred_size()),
        .request_resize = [this](uint32_t w, uint32_t h) {
            auto* gui_ext = static_cast<const clap_host_gui_t*>(_host->get_extension(_host, CLAP_EXT_GUI));
            if (gui_ext && gui_ext->request_resize) gui_ext->request_resize(_host, w, h);
        },
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
    // Host-echoed resize (host frame drag or our own request_resize). Keep the size
    // cache current so the latest size is what gets persisted.
    _last_size = Rect_size{static_cast<int32_t>(width), static_cast<int32_t>(height)};
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
    return _tail.load(std::memory_order_relaxed);
}

// MARK: - private

auto Plugin::_handle_host_flushed() -> void
{
    // Delivered after a resync too: a value here was pushed after its `_hostvalues` store, so it
    // is at least as new as what the resync read, and dropping it could lose that store.
    const auto deliver = [this](uint32_t address, double value) {
        _processor->handle(process::Event::Set{.address = address, .value = value});
    };
    const auto loaded = _from_load.consume(deliver);
    const auto flushed = _from_flush.consume(deliver); // After: a flush is the newer word from the host.
    if (loaded || flushed) {
        _processor->reset(process::Reset::Soft{});
    }
}

auto Plugin::_handle_user_actions(const clap_output_events_t* out_events, bool needs_resync) -> void
{
    // Don't replay stale events.
    if (needs_resync) {
        auto discarded = User_action{};
        while (_from_ui.pop(discarded)) {}
        return;
    }

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
                        .param_id = param.identity.address
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
                        .param_id = param.identity.address,
                        .value = host_value,
                    };
                    out_events->try_push(out_events, &e.header);
                }

                const auto plain_value = Value_helper::knob_to_plain(a.value, param.semantics);
                _processor->handle(process::Event::Set{param.identity.address, plain_value});
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
                        .param_id = param.identity.address
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
        _hostvalues[param.identity.address].store(host_value, std::memory_order_relaxed);
    }
    // Full while the host isn't processing (deactivated, asleep) and the editor keeps going. Not an
    // error: the next process restates everything from _hostvalues.
    if (!_from_ui.push(action)) _needs_resync.store(true, std::memory_order_relaxed);
}

// A host load's value: host atomics now, the kernel at the top of the next block. Coalesces, so any
// number of loads between blocks costs nothing and nothing overflows.
auto Plugin::_load_param(uint32_t address, double knob) -> void
{
    using namespace params;
    const auto& param = User_params::param_spec(address);
    _hostvalues[address].store(Value_helper::knob_to_host(knob, param.semantics), std::memory_order_relaxed);
    _from_load.push(process::Event::Set{.address = address, .value = Value_helper::knob_to_plain(knob, param.semantics)});
    if (_view) _view->set_param(address, knob);
}

// MARK: - notes

uint32_t Plugin::notePortsCount(bool isInput) const noexcept
{
    return (isInput ? Plug_info::Wants::notes_in : Plug_info::Wants::notes_out) ? 1 : 0;
}

bool Plugin::notePortsInfo(uint32_t index, bool isInput, clap_note_port_info* info) const noexcept
{
    if (!info || index >= notePortsCount(isInput)) return false;
    *info = {};
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    if (isInput && Plug_info::Wants::note_expression) info->supported_dialects |= CLAP_NOTE_DIALECT_MIDI_MPE;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, CLAP_NAME_SIZE, "%s", isInput ? "Notes In" : "Notes Out");
    return true;
}

// [audio] One input for the processor, and for the output while a bypassed note effect forwards.
auto Plugin::_input([[maybe_unused]] const process::Input& input, [[maybe_unused]] uint32_t time) -> void
{
#if TINY_HAS_NOTES_IN
    process::deliver(*_processor, input);
#if TINY_HAS_NOTES_OUT
    if (_passing) {
        std::visit(Inline_visitor{
            [](const process::Event::Any&) {},
            [&](const auto& e) { _notes.outbox().pass(static_cast<int32_t>(time), e); },
        }, input);
    }
#endif
#endif
}

auto Plugin::_handle_note_event([[maybe_unused]] const clap_event_header* event) -> void
{
#if TINY_HAS_NOTES_IN
    using namespace process;
    const auto time = event->time;
    const auto emit = [&](const Input& input) { _input(input, time); };

    switch (event->type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        case CLAP_EVENT_NOTE_CHOKE: {
            const auto* e = reinterpret_cast<const clap_event_note*>(event);
            // A wildcard (no id, no key) releases a channel, or everything.
            if (event->type != CLAP_EVENT_NOTE_ON && e->note_id < 0 && e->key < 0) {
                _notes.release_host(e->channel, emit);
                return;
            }
            if (e->key < 0 && event->type == CLAP_EVENT_NOTE_ON) return;

            const auto id = Note::Id{0, static_cast<uint8_t>(std::max<int16_t>(e->channel, 0)), static_cast<uint8_t>(std::max<int16_t>(e->key, 0))};
            const auto velocity = static_cast<float>(e->velocity);
            auto note = event->type == CLAP_EVENT_NOTE_ON ? Note::Any{Note::On{id, velocity}}
                      : event->type == CLAP_EVENT_NOTE_OFF ? Note::Any{Note::Off{id, velocity}}
                      : Note::Any{Note::Choke{id}};
            _notes.from_host(process::mpe_enabled(*_processor), e->note_id, note, emit); // A note on a member channel inherits its MPE values.
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION: {
            const auto* e = reinterpret_cast<const clap_event_note_expression*>(event);
            using Kind = Note::Expression::Kind;
            auto kind = std::optional<Kind>{};
            switch (e->expression_id) {
                case CLAP_NOTE_EXPRESSION_VOLUME: kind = Kind::Volume; break;
                case CLAP_NOTE_EXPRESSION_PAN: kind = Kind::Pan; break;
                case CLAP_NOTE_EXPRESSION_TUNING: kind = Kind::Tuning; break;
                case CLAP_NOTE_EXPRESSION_VIBRATO: kind = Kind::Vibrato; break;
                case CLAP_NOTE_EXPRESSION_BRIGHTNESS: kind = Kind::Brightness; break;
                case CLAP_NOTE_EXPRESSION_PRESSURE: kind = Kind::Pressure; break;
                default: break;
            }
            if (!kind || (e->note_id < 0 && e->key < 0)) return; // No per-channel form.
            const auto id = Note::Id{0, static_cast<uint8_t>(std::max<int16_t>(e->channel, 0)), static_cast<uint8_t>(std::max<int16_t>(e->key, 0))};
            auto note = Note::Any{Note::Expression{id, *kind, e->value}};
            if (_notes.from_host(e->note_id, note)) emit(Input{note});
            break;
        }
        case CLAP_EVENT_MIDI: {
            const auto* e = reinterpret_cast<const clap_event_midi*>(event);
            _notes.from_midi(process::mpe_enabled(*_processor), e->data[0], e->data[1], e->data[2], emit);
            break;
        }
        default:
            break;
    }
#endif
}

// [audio] What the processor sent this block, after all-notes-off if the stream broke.
auto Plugin::_send_notes([[maybe_unused]] const clap_output_events* out) -> void
{
#if TINY_HAS_NOTES_OUT
    using namespace process;
    auto& box = _notes.outbox();

    const auto note_event = [](uint16_t type, uint32_t time, int32_t id, int16_t channel, int16_t key, double velocity) {
        return clap_event_note{
            .header = {.size = sizeof(clap_event_note), .time = time, .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = type, .flags = 0},
            .note_id = id, .port_index = 0, .channel = channel, .key = key, .velocity = velocity,
        };
    };

    if (out && _notes.take_all_off()) {
        const auto e = note_event(CLAP_EVENT_NOTE_OFF, 0, -1, -1, -1, 0.); // Every note, every channel.
        out->try_push(out, &e.header);
    }

    for (const auto& entry : box.events()) {
        if (!out) break;
        const auto time = static_cast<uint32_t>(entry.frame);
        std::visit(Inline_visitor{
            [&](const Note::Any& note) {
                std::visit(Inline_visitor{
                    [&](const Note::On& e) {
                        const auto c = note_event(CLAP_EVENT_NOTE_ON, time, static_cast<int32_t>(e.note.id & 0x7fffffff), e.note.channel, e.note.key, e.velocity);
                        out->try_push(out, &c.header);
                    },
                    [&](const Note::Off& e) {
                        const auto c = note_event(CLAP_EVENT_NOTE_OFF, time, static_cast<int32_t>(e.note.id & 0x7fffffff), e.note.channel, e.note.key, e.velocity);
                        out->try_push(out, &c.header);
                    },
                    [&](const Note::Choke& e) {
                        const auto c = note_event(CLAP_EVENT_NOTE_CHOKE, time, static_cast<int32_t>(e.note.id & 0x7fffffff), e.note.channel, e.note.key, 0.);
                        out->try_push(out, &c.header);
                    },
                    [&](const Note::Expression& e) {
                        static constexpr auto ids = std::array{CLAP_NOTE_EXPRESSION_VOLUME, CLAP_NOTE_EXPRESSION_PAN, CLAP_NOTE_EXPRESSION_TUNING,
                                                               CLAP_NOTE_EXPRESSION_VIBRATO, CLAP_NOTE_EXPRESSION_BRIGHTNESS, CLAP_NOTE_EXPRESSION_PRESSURE};
                        const auto c = clap_event_note_expression{
                            .header = {.size = sizeof(clap_event_note_expression), .time = time, .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = CLAP_EVENT_NOTE_EXPRESSION, .flags = 0},
                            .expression_id = ids[static_cast<size_t>(e.kind)],
                            .note_id = static_cast<int32_t>(e.note.id & 0x7fffffff), .port_index = 0, .channel = e.note.channel, .key = e.note.key, .value = e.value,
                        };
                        out->try_push(out, &c.header);
                    },
                }, note);
            },
            [&](const auto& control) { // Control::Any or midi::Raw: both go out as MIDI bytes.
                const auto bytes = midi::encode(control);
                if (bytes.size == 0) return;
                auto c = clap_event_midi{
                    .header = {.size = sizeof(clap_event_midi), .time = time, .space_id = CLAP_CORE_EVENT_SPACE_ID, .type = CLAP_EVENT_MIDI, .flags = 0},
                    .port_index = 0, .data = {bytes.data[0], bytes.data[1], bytes.data[2]},
                };
                out->try_push(out, &c.header);
            },
        }, entry.event);
    }
    box.clear();
#endif
}

} // namespace tiny::clap