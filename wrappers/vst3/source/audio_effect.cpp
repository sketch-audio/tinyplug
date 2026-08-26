#include "audio_effect.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <tinyplug/denormal_guard.hpp>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "base/source/fstreamer.h"

#include "models/meters.hpp"
#include "models/params.hpp"
#include "plug_info.hpp"

#include "adapters.hpp"
#include "messaging.hpp"

namespace tiny::vst3 {

// MARK: - worker

#if TINY_HAS_WORKER

constexpr auto k_worker_from_processor_id = "tiny/worker/from_processor";
constexpr auto k_worker_to_processor_id   = "tiny/worker/to_processor";

auto Audio_effect::_setup_worker() -> void
{
    // Realtime-safe push from the audio thread: lock-free SPSC push,
    // no allocation. The shuttle thread forwards over IMessage.
    try_bind_worker(*_processor, Worker_processor_actor{
        [this](const auto& m) -> bool { return _worker_outbound.push(m); }
    });

    // Shuttle drain: pop pending From_processor messages and send via
    // IMessage. Runs on the shuttle thread (non-realtime).
    _shuttle.register_drain([this]() {
        auto m = typename User_worker::Model::From_processor{};
        while (_worker_outbound.pop(m)) {
            _to_ctrl.send_variant(k_worker_from_processor_id, m);
        }
    });

    // Worker → processor replies arrive via IMessage on notify().
    _router.register_handler(k_worker_to_processor_id, [this](std::span<const std::byte> bytes, uint32_t tag) {
        using To_proc = typename User_worker::Model::To_processor;
        _worker_to_proc_inbox.push(vst3::reconstruct_variant<To_proc>(bytes, tag));
    });
}

Steinberg::tresult PLUGIN_API Audio_effect::notify(Steinberg::Vst::IMessage* message)
{
    if (_router.dispatch(message)) return Steinberg::kResultOk;
    return Super::notify(message);
}

#endif // TINY_HAS_WORKER

auto Audio_effect::_drain_worker_to_processor() -> void
{
#if TINY_HAS_WORKER
    try_drain_worker_to_processor(*_processor, _worker_to_proc_inbox);
#endif
}


// MARK: - initialize

Steinberg::tresult PLUGIN_API Audio_effect::initialize(Steinberg::FUnknown* context)
{
    // Here the Plug-in will be instantiated.

    // Initialize the parent.
    auto result = Super::initialize(context);

    if (result != Steinberg::kResultOk) {
        return result;
    }

    // Create the audio IO.

    using namespace Steinberg::Vst; // SpeakerArr, BusTypes

    const auto input_count = Plug_info::wants_sidechain ? 2 : 1;

    for (auto i = decltype(input_count){}; i < input_count; ++i) {
        const auto is_main = (i == 0);

        const auto* input_name = is_main ? u"Input" : u"Sidechain";
        const auto bus_type = is_main ? BusTypes::kMain : BusTypes::kAux;

        addAudioInput(input_name, SpeakerArr::kStereo, bus_type);
    }

    addAudioOutput(u"Output", SpeakerArr::kStereo, BusTypes::kMain);

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Audio_effect::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Steinberg::Vst::AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API Audio_effect::setupProcessing(Steinberg::Vst::ProcessSetup& newSetup)
{
    using namespace params;

    // Clear handshake state for reconfigure.
    _pending_latency.store(std::nullopt, std::memory_order_release);
    _accepted_latency.store(std::nullopt, std::memory_order_release);
    _did_peek.store(false, std::memory_order_relaxed);

    // Get the initial state for this configuration.
    auto config_values = std::array<double, num_params>{};
    for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
        // Convert to plain space.
        const auto& param = User_params::param_spec(addr);
        const auto knob = _host_values[addr].load(std::memory_order_relaxed);
        const auto plain = Value_helper::knob_to_plain(knob, param.semantics);
        config_values[addr] = plain;
    }

    _processor->configure(Config{
        .sr = newSetup.sampleRate,
        .params = config_values
    });
    _latency = _processor->latency_samps();
    _needs_report.store(true, std::memory_order_relaxed); // Defer the normal-path latency notification to `process`.

    _bypass.reset(static_cast<float>(newSetup.sampleRate));
    _bypass.set_latency(_latency);

    const auto max_samples = static_cast<size_t>(newSetup.maxSamplesPerBlock);
    for (auto& channel : _input_data) {
        channel.resize(max_samples);
        std::fill(channel.begin(), channel.end(), 0.f);
    }

    // Create the event IO.
    static constexpr auto events_size = [](auto samples) {
        const auto state = 4 * num_params;
        const auto scale = std::max(samples / 256, size_t{1});
        const auto automation = scale * 64 * std::bit_width(num_params); // We expect number of automated parameters to be small but we need to be able to handle a lot of flux.
        return state + automation + 1;
    };
    _events.reserve(events_size(max_samples)); // Want fixed size event vector.

    return Steinberg::Vst::AudioEffect::setupProcessing(newSetup);
}

Steinberg::tresult PLUGIN_API Audio_effect::setActive(Steinberg::TBool state)
{
    // Called when the Plug-in is enable/disable (On/Off).

    // When activating, we need to see if there is a pending latency.
    if (state) {
        // Host accepted.
        const auto pending = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (pending.has_value()) {
            _accepted_latency.store(*pending, std::memory_order_release);
            _latency.store(*pending, std::memory_order_relaxed);
        }
#if TINY_HAS_WORKER
        _shuttle.start(User_worker::Model::update_period);
#endif
    }
    else {
#if TINY_HAS_WORKER
        _shuttle.stop();
#endif
    }

    return Steinberg::Vst::AudioEffect::setActive(state);
}

// The stream is discontinuous on both edges of this call. Deferred to `process` so it
// can't race a block already in flight.
Steinberg::tresult PLUGIN_API Audio_effect::setProcessing(Steinberg::TBool state)
{
    _needs_clear.store(true, std::memory_order_relaxed);
    return Steinberg::Vst::AudioEffect::setProcessing(state);
}

Steinberg::tresult PLUGIN_API Audio_effect::setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns, Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts)
{
    if (!inputs || !outputs) return Steinberg::kResultFalse;

    using namespace Steinberg::Vst;

    const auto expected_ins = Plug_info::wants_sidechain ? 2 : 1;
    const auto expected_outs = 1;

    if (numIns != expected_ins || numOuts != expected_outs) return Steinberg::kResultFalse;

    // What does the host want to do?
    auto& input_arr = inputs[0];
    auto& output_arr = outputs[0];
    const auto wants_mono = SpeakerArr::getChannelCount(input_arr) == 1 && SpeakerArr::getChannelCount(output_arr) == 1;
    const auto wants_stereo = SpeakerArr::getChannelCount(input_arr) == 2 && SpeakerArr::getChannelCount(output_arr) == 2;

    // We will accept either mono or stereo sidechain.
    auto accept_sidechain = [&]() {
        if constexpr (Plug_info::wants_sidechain) {
            auto& sidechain_arr = inputs[1];
            getAudioInput(1)->setArrangement(sidechain_arr);
            _schannels = static_cast<size_t>(SpeakerArr::getChannelCount(sidechain_arr));
        }
    };

    if (wants_mono && Plug_info::can_process_mono) {
        // The host wants mono --> mono.
        getAudioInput(0)->setArrangement(input_arr);
        getAudioOutput(0)->setArrangement(output_arr);

        _ichannels = _ochannels = 1;
        accept_sidechain();

        return Steinberg::kResultTrue;
    }
    else if (wants_stereo) {
        // The host wants stereo --> stereo.
        getAudioInput(0)->setArrangement(input_arr);
        getAudioOutput(0)->setArrangement(output_arr);

        _ichannels = _ochannels = 2;
        accept_sidechain();

        return Steinberg::kResultTrue;
    }

    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Audio_effect::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
    // By default kSample32 is supported.
    if (symbolicSampleSize == Steinberg::Vst::kSample32)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

// MARK: - process

Steinberg::tresult PLUGIN_API Audio_effect::process(Steinberg::Vst::ProcessData& data)
{
    const auto denormals = Denormal_guard{}; // Restores the host's FP mode on the way out.
    this->_drain_worker_to_processor();

    // Latency fallback completes handshake on first `started` after a proposal.
    const auto [playing, started] = [&]() {
        auto context = data.processContext;
        if (!context) return std::pair{false, false};

        const auto state = context->state;
        const auto has_flag = [](auto x, auto f) { return (x & f) > 0; };

        const auto p = has_flag(state, Steinberg::Vst::ProcessContext::kPlaying);
        const auto s = !_was_moving && p;
        return std::pair{p, s};
    }();
    _was_moving = playing;

    // Fallback complete handshake now in case of non-conforming host..
    if (started && _did_peek.load(std::memory_order_relaxed)) {
        if (const auto pending = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel)) {
            _accepted_latency.store(*pending, std::memory_order_release); // The kernel should manifest on the next process.
            _latency.store(*pending, std::memory_order_relaxed); // The non-conforming path.
        }
        _did_peek.store(false, std::memory_order_relaxed);
    }

    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        const auto new_latency = static_cast<uint32_t>(*accepted_latency);
        _processor->handle_event(Accepted_latency{new_latency});
        _bypass.set_latency(new_latency);
        assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
    }

    // Discontinuity requested by the host — forget history before anything this block
    // delivers lands.
    if (_needs_clear.exchange(false, std::memory_order_relaxed)) {
        _processor->clear();
        _processor->snap(); // Paired: a discontinuity warrants both.
        _bypass.clear();    // Its delay lines hold pre-seek dry audio.
        _bypass.snap();
    }

    // Process events in state queue.
    auto state_event = Set_param{};
    while (_queue.pop(state_event)) {
        _processor->handle_event(state_event);
    }

    // Validate shape up front.
    const auto has_inputs = data.numInputs > 0 && data.inputs;
    const auto has_sidechain = data.numInputs > 1 && data.inputs;
    const auto has_outputs = data.numOutputs > 0 && data.outputs;

    const auto required_in_channels = static_cast<Steinberg::int32>(_ichannels);
    const auto required_out_channels = static_cast<Steinberg::int32>(_ochannels);
    const auto required_sc_channels = static_cast<Steinberg::int32>(_schannels);

    const auto inputs_shape_ok = !has_inputs
        || (data.inputs[0].channelBuffers32 != nullptr && data.inputs[0].numChannels >= required_in_channels);
    const auto outputs_shape_ok = !has_outputs
        || (data.outputs[0].channelBuffers32 != nullptr && data.outputs[0].numChannels >= required_out_channels);
    const auto sidechain_shape_ok = !(has_sidechain && Plug_info::wants_sidechain)
        || (data.inputs[1].channelBuffers32 != nullptr && data.inputs[1].numChannels >= required_sc_channels);

    const auto shape_ok = inputs_shape_ok && outputs_shape_ok && sidechain_shape_ok;

    // Guarded on `shape_ok`: a null `channelBuffers32` is exactly what the shape check
    // above rejects, and indexing it to look for null *channels* would dereference it.
    auto main_input_pointers_ok = true;
    if (shape_ok && has_inputs) {
        for (size_t i = 0; i < _ichannels; ++i) {
            if (data.inputs[0].channelBuffers32[i] == nullptr) {
                main_input_pointers_ok = false;
                break;
            }
        }
    }

    auto main_output_pointers_ok = true;
    if (shape_ok && has_outputs) {
        for (size_t i = 0; i < _ochannels; ++i) {
            if (data.outputs[0].channelBuffers32[i] == nullptr) {
                main_output_pointers_ok = false;
                break;
            }
        }
    }

    auto sidechain_pointers_ok = true;
    if (shape_ok && has_sidechain && Plug_info::wants_sidechain) {
        for (size_t i = 0; i < _schannels; ++i) {
            if (data.inputs[1].channelBuffers32[i] == nullptr) {
                sidechain_pointers_ok = false;
                break;
            }
        }
    }

    const auto pointers_ok = main_input_pointers_ok && main_output_pointers_ok && sidechain_pointers_ok;

    // Well-formed *and* actually carrying samples on both the main input and the main
    // output. Everything else is a flush.
    const auto renders_audio = shape_ok && pointers_ok
        && has_inputs && has_outputs && data.numSamples > 0;

    _events.clear(); // Events only valid for this render cycle.
    this->normalize_input_events(data, renders_audio);

    // Now we have the events organized how we want.
    const auto event_count = _events.size();
    auto event_index = size_t{};
    const auto* event = event_count > 0 ? &_events[event_index] : nullptr;

    auto next_event = [&]() {
        ++event_index;
        if (event_index >= event_count) {
            event = nullptr;
        }
        else {
            event = &_events[event_index];
        }
    };

    // Create the context.
    _meters.fill(0);
    auto context = Dsp_context{.meters = _meters};

    // kPrefetch (sampler pre-roll / variable-rate playback) is not a bounce → realtime.
    context.render_mode = (data.processMode == Steinberg::Vst::kOffline) ? Render_mode::Offline : Render_mode::Realtime;
    const auto is_offline_bounce = (data.processMode == Steinberg::Vst::kOffline);

    // A processMode transition (entering/leaving an offline bounce) is a discontinuity:
    // the audio either side is unrelated, so forget history as well as manifesting values
    // rather than gliding in.
    if (data.processMode != _last_process_mode) {
        _processor->clear();
        _processor->snap();
        _bypass.clear();    // Its delay lines hold pre-bounce dry audio.
        _bypass.snap();
        _last_process_mode = data.processMode;
    }

    // Copy main input to internal buffers in case of in-place processing.
    if (renders_audio) {
        for (size_t i = 0; i < _ichannels; ++i) {
            const auto* in = data.inputs[0].channelBuffers32[i];
            auto& channel = _input_data[i];
            assert(channel.size() >= static_cast<size_t>(data.numSamples) && "Input buffer too small!");
            std::copy(in, in + data.numSamples, channel.begin());
        }
    }

    // So we can process with an offset.
    auto do_process = [this, &data, &context, has_inputs, has_outputs, has_sidechain](size_t num_frames, size_t offset) {
        assert(offset + num_frames <= static_cast<size_t>(data.numSamples) && "Offset + num_frames exceeds data.numSamples!");

        // Assign buffer ptrs.
        if (has_inputs) {
            assert(data.inputs[0].numChannels >= static_cast<Steinberg::int32>(_ichannels));
            for (size_t i = 0; i < _ichannels; ++i) {
                _ibuffers[i] = &_input_data[i][offset];
            }
        }
        if (has_outputs) {
            assert(data.outputs[0].numChannels >= static_cast<Steinberg::int32>(_ochannels));
            for (size_t i = 0; i < _ochannels; ++i) {
                _obuffers[i] = &data.outputs[0].channelBuffers32[i][offset];
            }
        }
        if (has_sidechain && Plug_info::wants_sidechain) {
            assert(data.inputs[1].numChannels >= static_cast<Steinberg::int32>(_schannels));
            for (size_t i = 0; i < _schannels; ++i) {
                _sbuffers[i] = &data.inputs[1].channelBuffers32[i][offset]; // Assume sidechain not "in-place"
            }
        }

        // Resolve the musical context.
        context.musical_context = Musical_context{}; // Default in case processContext is null.
        if (const auto* vst_context = data.processContext; vst_context) {
            const auto sample_pos = vst_context->projectTimeSamples;
            const auto beat_pos = vst_context->projectTimeMusic;
            const auto cycle_start = vst_context->cycleStartMusic;
            const auto cycle_end = vst_context->cycleEndMusic;
            const auto tempo = vst_context->tempo;
            const auto sr = vst_context->sampleRate;
            const auto ts_numer = vst_context->timeSigNumerator;
            const auto ts_denom = vst_context->timeSigDenominator;

            using enum Steinberg::Vst::ProcessContext::StatesAndFlags;
            const auto transport_state = vst_context->state;
            const auto has_flag = [](auto x, auto f) { return (x & f) > 0; };

            context.musical_context = {
                .sample_pos = sample_pos + static_cast<int64_t>(offset),
                .beat_pos = beat_pos + frames_to_beats(static_cast<int64_t>(offset), tempo, sr),
                .cycle_start = cycle_start,
                .cycle_end = cycle_end,
                .tempo_ideal = tempo,
                .time_sig = {ts_numer, ts_denom},
                .transport_state = {
                    .moving = has_flag(transport_state, kPlaying),
                    .cycling = has_flag(transport_state, kCycleActive),
                    .recording = has_flag(transport_state, kRecording)
                }
            };
        }

        using In = std::span<const float*>;
        using Out = std::span<float*>;
        context.ibuffers = has_inputs ? In{_ibuffers.begin(), _ichannels} : In{};
        context.obuffers = has_outputs ? Out{_obuffers.begin(), _ochannels} : Out{};
        context.sbuffers = has_sidechain ? In{_sbuffers.begin(), _schannels} : In{};
        context.num_frames = num_frames;

        _processor->process(context);
    };

    // Do process loop.
    const auto frame_count = data.numSamples;
    auto now = decltype(frame_count){};
    auto remaining = frame_count;

    const auto can_skip = _bypass.can_skip_effect();

    // Resuming from a stretch where advance_rampers() didn't run (can_skip skips
    // process()) — settle before this block's own automation lands, not after.
    if (_was_skipped && !can_skip) {
        _processor->snap();
    }
    _was_skipped = can_skip;

    if (can_skip || !renders_audio) {
        // No kernel run to interleave the events with, so deliver them all.
        auto delivered = false;
        while (event) {
            _processor->handle_event(event->event);
            next_event();
            delivered = true;
        }

        // Resync on flush block.
        if (!renders_audio && delivered) {
            _processor->snap();
        }
    }
    else {
        while (remaining > 0) {
            if (!event) {
                const auto offset = frame_count - remaining; // remaining strictly <= frame_count.
                do_process(static_cast<size_t>(remaining), static_cast<size_t>(offset));
                break;
            }

            // Clamp.
            // Compute in 64-bit to avoid signed overflow on pathological offsets.
            const auto delta64 = static_cast<int64_t>(event->offset) - static_cast<int64_t>(now);
            const auto clamped64 = std::clamp<int64_t>(delta64, 0, static_cast<int64_t>(remaining));
            const auto frames_until_event = static_cast<decltype(remaining)>(clamped64);

            if (frames_until_event > 0) {
                const auto offset = frame_count - remaining;
                do_process(static_cast<size_t>(frames_until_event), static_cast<size_t>(offset));
                remaining -= frames_until_event;
                now += frames_until_event;
            }

            do {
                _processor->handle_event(event->event);
                next_event();
            } while (event && event->offset <= now);
        }
    }

    // Host bypass.
    if (renders_audio) {
        auto in_buffers = [&]() {
            auto arr = std::array<const float*, max_ichannels>{};
            for (size_t i = 0; i < _ichannels; ++i) {
                arr[i] = &_input_data[i][0];
            }
            return arr;
        }();

        auto out_buffers = [&]() {
            auto arr = std::array<float*, max_ochannels>{};
            for (size_t i = 0; i < _ochannels; ++i) {
                arr[i] = &data.outputs[0].channelBuffers32[i][0];
            }
            return arr;
        }();

        const auto min_channels = std::min({
            data.inputs[0].numChannels,
            data.outputs[0].numChannels,
            static_cast<Steinberg::int32>(max_ichannels),
            static_cast<Steinberg::int32>(max_ochannels)
        });
        const auto num_channels = static_cast<size_t>(min_channels);
        _bypass.process(
            {in_buffers.begin(), num_channels},
            {out_buffers.begin(), num_channels},
            static_cast<size_t>(data.numSamples)
        );
    }

    auto add_output_event = [&](int32_t id, double value) {
        auto event_index = Steinberg::int32{};
        if (!data.outputParameterChanges) return;
        auto* queue = data.outputParameterChanges->addParameterData(static_cast<uint32_t>(id), event_index);
        if (!queue) return;
        auto point_index = Steinberg::int32{};
        queue->addPoint(0, value, point_index); // offset, value, index
    };

    // Don't meter during flush blocks.
    // Live can crash if we meter using output params during bounce!
    if (renders_audio && !is_offline_bounce) {
        for (size_t i = 0; i < num_meters; ++i) {
            if (context.meters[i] != _last_meters[i]) {
                // Send normalized value to UI per VST spec.
                const auto val = context.meters[i];
                const auto& spec = User_meters::spec(static_cast<uint32_t>(i));
                const auto norm = plain_to_norm(val, spec.range);
                add_output_event(export_param_offset + static_cast<int32_t>(i), norm);

                _last_meters[i] = context.meters[i];
            }
        }
    }

    auto notify = [&, this] {
        _change_count += 1.;
        const auto value = std::fmod(_change_count / max_change_count, 1.);
        add_output_event(latency_param_id, value);
    };

    // Latency notifications, now only when actually changed!
    const auto reported = _reported_latency.load(std::memory_order_relaxed);
    if (const auto proposed = context.propose_latency; proposed.has_value() && *proposed != reported) {
        // Set pending, mark reported, & notify.
        _pending_latency.store(*proposed, std::memory_order_release);
        _did_peek.store(false, std::memory_order_relaxed);
        _reported_latency.store(*proposed, std::memory_order_relaxed);
        notify();
    }

    // Normal path latency still needs a notification.
    if (_needs_report.exchange(false, std::memory_order_relaxed)) {
        // Only send latency change if reset actually results in new latency.
        const auto latency = _latency.load(std::memory_order_relaxed);
        if (latency != _reported_latency.load(std::memory_order_relaxed)) {
            _reported_latency.store(latency, std::memory_order_relaxed);
            notify();
        }
    }

    return Steinberg::kResultOk;
}

// MARK: - state load

Steinberg::tresult PLUGIN_API Audio_effect::setState(Steinberg::IBStream* state)
{
    using namespace params;

    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    auto header = State_rules::Vst3::Header{};
    if (!streamer.readInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    // Validate for real, not just in debug: hosts hand us chunks from other plug-ins
    // and truncated session files, and every count below is untrusted until checked.
    if (header[0] != Plug_info::framework_code) return Steinberg::kResultFalse;
    if (header[1] != Plug_info::manufacturer_code) return Steinberg::kResultFalse;
    if (header[2] != Plug_info::plugin_code) return Steinberg::kResultFalse;

    const auto num_stored_values = header[3];

    auto notify = [&](const auto& spec, float knob_value) {
        if (!State_rules::is_persistent(spec)) return;

        const auto address = spec.identity.address;
        const auto plain_value = Value_helper::knob_to_plain(knob_value, spec.semantics);

        // Queue sends events to processor at next process call. 
        _queue.push(Set_param{address, plain_value}); // Overwrite queue, won't overflow.

        // Maintain host values.
        _host_values[address].store(knob_value, std::memory_order_relaxed);
    };

    auto read_and_notify = [&](const auto& knob_values, auto index) {
        // Do we have a real value?
        if (const auto knob_value = knob_values[index]; knob_value != State_rules::no_value) {
            notify(User_params::param_spec(index), knob_value);
        }
    };

    // Sized by what we can use, not by what the chunk claims; the stream is still read
    // in full so the bypass float below stays aligned.
    const auto usable_values = std::min<size_t>(num_stored_values, num_params);
    auto stored_values = std::vector<float>(usable_values);
    for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
        auto value = float{};
        if (!streamer.readFloat(value)) {
            return Steinberg::kResultFalse;
        }
        if (i < num_params) stored_values[i] = value;
    }

    if (num_params <= num_stored_values) {
        // Set values stored in state.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            read_and_notify(stored_values, i);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
            read_and_notify(stored_values, i);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_stored_values; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            notify(param, static_cast<float>(Value_helper::default_value(param, Space::Knob)));
        }
    }

    // Try to read bypass state.
    auto bypass_value = float{};
    if (streamer.readFloat(bypass_value)) {
        _bypass.set_bypassed(bypass_value >= 0.5f);
    }
    else {
        //_bypass.set_bypassed(false);
    }

    return Steinberg::kResultOk;
}

// MARK: - state save

Steinberg::tresult PLUGIN_API Audio_effect::getState(Steinberg::IBStream* state)
{
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    // Generate the header.
    auto header = State_rules::Vst3::Header{
        Plug_info::framework_code, // Reserved
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_params
    };

    if (!streamer.writeInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        // Grab state from host values.
        const auto knob_value = static_cast<float>(_host_values[i].load(std::memory_order_relaxed));

        const auto& spec = User_params::param_spec(i);
        const auto to_write = State_rules::is_persistent(spec) ? knob_value : State_rules::no_value;

        if (!streamer.writeFloat(to_write)) {
            return Steinberg::kResultFalse;
        }
    }

    // Write bypass.
    const auto bypass_value = _bypass.is_bypassed() ? 1.f : 0.f;
    if (!streamer.writeFloat(bypass_value)) {
        return Steinberg::kResultFalse;
    }

    return Steinberg::kResultOk;
}

// MARK: - latency, tail

Steinberg::uint32 PLUGIN_API Audio_effect::getLatencySamples()
{
    // Peek pending latency (host got notified already).
    if (const auto pending = _pending_latency.load(std::memory_order_acquire)) {
        _did_peek.store(true, std::memory_order_relaxed); // Fallback for non-conforming hosts.
        return *pending;
    }

    return _latency.load(std::memory_order_relaxed);
}

Steinberg::uint32 PLUGIN_API Audio_effect::getTailSamples()
{
    // Resolve to Steinberg's named constants.
    using namespace Steinberg::Vst;
    const auto tail = _processor->tail_samps();
    const auto inf_tail = std::numeric_limits<uint32_t>::max();
    return tail == 0 ? kNoTail : (tail == inf_tail ? kInfiniteTail : tail);
}

Steinberg::uint32 PLUGIN_API Audio_effect::getProcessContextRequirements()
{
    auto requirements = Steinberg::Vst::ProcessContextRequirements{};
    requirements.needProjectTimeMusic();
    requirements.needCycleMusic();
    requirements.needTempo();
    requirements.needTimeSignature();
    requirements.needTransportState();
    return requirements.flags;
}

// MARK: - private

auto Audio_effect::normalize_input_events(Steinberg::Vst::ProcessData& data, bool renders_audio) -> void
{
    using namespace params;

    if (!data.inputParameterChanges) return;
    auto& param_changes = *data.inputParameterChanges;
    const auto num_changes = param_changes.getParameterCount();

    for (auto i = decltype(num_changes){}; i < num_changes; ++i) {
        auto* queue_ptr = param_changes.getParameterData(i);
        if (!queue_ptr) continue;
        auto& queue = *queue_ptr;

        const auto id = queue.getParameterId();

        if (id == bypass_param_id) {
            // Handle immediately
            if (queue.getPointCount() <= 0) continue;
            auto value = Steinberg::Vst::ParamValue{};
            auto offset = int32_t{};
            if (queue.getPoint(0, offset, value) != Steinberg::kResultTrue) continue;
            _bypass.set_bypassed(value >= 0.5);
            continue;
        }

        if (id >= User_params::num_params) continue; // Be defensive.

        const auto& param = User_params::param_spec(id); // To denormalize the automation values.

        const auto point_count = queue.getPointCount();

        // Block starts with an implicit point at -1 offset.
        auto previous_offset = int32_t{-1};

        for (auto point_idx = decltype(point_count){}; point_idx < point_count; ++point_idx) {
            auto value = Steinberg::Vst::ParamValue{};
            auto offset = int32_t{};
            if (queue.getPoint(point_idx, offset, value) != Steinberg::kResultTrue) continue;

            // VST3 docs mention implicit point at -1. Hard-clamp to legal range for this block.
            const auto max_offset = std::max(data.numSamples - 1, 0);
            offset = std::clamp(offset, -1, max_offset);

            // Flush blocks don't render audio, so ramp duration is zero samples.
            const auto ramp_dur = renders_audio ? std::max(offset - previous_offset, 0) : 0;

            if (_events.size() == _events.capacity()) {
                // _events vector is full!
                assert(false && "Event vector is full, increase capacity!");
            };

            // Set param
            if (ramp_dur <= 1) {
                _events.push_back({
                    .event = Set_param{
                        .address = id,
                        .value = Value_helper::knob_to_plain(value, param.semantics)
                    },
                    .offset = std::max(previous_offset, {}),
                });
            }
            // Ramp param
            else {
                _events.push_back({
                    .event = Ramp_param{
                        .address = id,
                        .target = Value_helper::knob_to_plain(value, param.semantics),
                        .dur_samples = ramp_dur
                    },
                    .offset = std::max(previous_offset, {}),
                });
            }

            previous_offset = offset;

            // Maintain host values.
            _host_values[id].store(value, std::memory_order_relaxed);
        }
    }

    // sort events.
    std::ranges::sort(_events, [](const auto& a, const auto& b) { return a.offset < b.offset; });
}

} // namespace tiny::vst3