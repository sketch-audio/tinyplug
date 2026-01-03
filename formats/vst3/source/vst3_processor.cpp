#include <algorithm>
#include <optional>
#include <ranges>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "base/source/fstreamer.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_info.h"

#include "vst3_adapters.h"
#include "vst3_processor.h"

namespace tiny {

// MARK: - initialize

Steinberg::tresult PLUGIN_API Vst3_processor::initialize(Steinberg::FUnknown* context)
{
    // Here the Plug-in will be instantiated.

    // Initialize the parent.
    Steinberg::tresult result = Steinberg::Vst::AudioEffect::initialize(context);

    if (result != Steinberg::kResultOk)
        return result;

    // Create the audio IO.

    using namespace Steinberg::Vst;

    const auto input_count = Plug_info::wants_sidechain ? 2 : 1;

    for (size_t i = 0; i < input_count; ++i) {
        const auto is_main = (i == 0);

        const auto* input_name = is_main ? u"Input" : u"Sidechain";
        const auto bus_type = is_main ? BusTypes::kMain : BusTypes::kAux;

        addAudioInput(input_name, SpeakerArr::kStereo, bus_type);
    }

    addAudioOutput(u"Output", SpeakerArr::kStereo, BusTypes::kMain);

    // Create the event IO.

    _events.reserve(128); // Want fixed size event vector.

    // Get knob defaults for automation points.
    for (const auto& param : User_params::param_specs(Param_order::Indexable)) {
        _last_points[param.address] = {.offset = -1, .value = get_knob_default(param)};
    }

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Vst3_processor::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Do not forget to call parent.
    return Steinberg::Vst::AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API Vst3_processor::setActive(Steinberg::TBool state)
{
    // Called when the Plug-in is enable/disable (On/Off).
    
    // When activating, we need to see if there is a pending latency.
    if (state) {
        const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (pending_latency) {
            _accepted_latency.store(*pending_latency, std::memory_order_release); // The kernel should manifest on the next process.
            _latency = *pending_latency;
        }
    }

    return Steinberg::Vst::AudioEffect::setActive(state);
}

Steinberg::tresult PLUGIN_API Vst3_processor::setupProcessing(Steinberg::Vst::ProcessSetup& newSetup)
{
    // Called before any processing.
    _processor->reset(newSetup.sampleRate);
    _latency = _processor->latency_samps();
    _did_reset = true;
    return Steinberg::Vst::AudioEffect::setupProcessing(newSetup);
}

Steinberg::tresult PLUGIN_API Vst3_processor::setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns, Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts)
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

Steinberg::tresult PLUGIN_API Vst3_processor::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
    // By default kSample32 is supported.
    if (symbolicSampleSize == Steinberg::Vst::kSample32)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

// MARK: - process

Steinberg::tresult PLUGIN_API Vst3_processor::process(Steinberg::Vst::ProcessData& data)
{
    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        _processor->handle_event(Accepted_latency{*accepted_latency});
        assert(_processor->latency_samps() == *accepted_latency && "Kernel must apply the accepted latency!");
    }

    // Process events in state queue.
    auto state_event = Set_param{};
    while (_queue.pop(state_event)) {
        _processor->handle_event(state_event);
    }

    _events.clear(); // Events only valid for this render cycle.
    this->normalize_input_events(data);

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

    // So we can process with an offset.
    auto do_process = [this, &data, &context](size_t num_frames, size_t offset) {
        // Assign buffer ptrs.
        assert(data.inputs[0].numChannels == static_cast<Steinberg::int32>(_ichannels));
        for (size_t i = 0; i < _ichannels; ++i) {
            _ibuffers[i] = &data.inputs[0].channelBuffers32[i][offset];
        }
        assert(data.outputs[0].numChannels == static_cast<Steinberg::int32>(_ochannels));
        for (size_t i = 0; i < _ochannels; ++i) {
            _obuffers[i] = &data.outputs[0].channelBuffers32[i][offset];
        }
        if constexpr (Plug_info::wants_sidechain) {
            assert(data.inputs[1].numChannels == static_cast<Steinberg::int32>(_schannels));
            for (size_t i = 0; i < _schannels; ++i) {
                _sbuffers[i] = &data.inputs[1].channelBuffers32[i][offset];
            }
        }

        // Resolve the musical context.
        const auto* vst_context = data.processContext;
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

        context.ibuffers = {_ibuffers.begin(), _ichannels};
        context.obuffers = {_obuffers.begin(), _ochannels};
        context.sbuffers = {_sbuffers.begin(), _schannels};
        context.num_frames = num_frames;

        _processor->process(context);
    };

    // Do process loop.
    const auto frame_count = data.numSamples;
    auto now = decltype(frame_count){};
    auto remaining = frame_count;

    while (remaining > 0) {
        if (!event) {
            const auto offset = frame_count - remaining;
            do_process(static_cast<size_t>(remaining), static_cast<size_t>(offset));
            break;
        }

        const auto frames_until_event = std::max({}, event->offset - now);

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

    auto add_output_event = [&](int32_t id, double value) {
        auto event_index = Steinberg::int32{};
        auto& queue = *data.outputParameterChanges->addParameterData(static_cast<uint32_t>(id), event_index);
        auto point_index = Steinberg::int32{};
        queue.addPoint(0, value, point_index); // offset, value, index
    };

    // Send exports as output parameter changes.
    for (size_t i = 0; i < num_meters; ++i) {
        if (context.meters[i] != _last_meters[i]) {
            // Send normalized value to UI per VST spec.
            const auto val = context.meters[i];
            const auto& spec = User_meters::meter_spec(static_cast<uint32_t>(i));
            const auto norm = plain_to_norm(val, spec.range);
            add_output_event(export_param_offset + static_cast<int32_t>(i), norm);

            _last_meters[i] = context.meters[i];
        }
    }

    // Has the kernel proposed a new latency?
    if (const auto proposed_latency = context.propose_latency; proposed_latency && *proposed_latency != _latency) {
        // Notify controller and sit on the pending latency.
        add_output_event(latency_param_id, static_cast<double>(*proposed_latency));
        _pending_latency.store(*proposed_latency, std::memory_order_release);
    }

    if (_did_reset) {
        // Force host to check latency.
        add_output_event(latency_param_id, static_cast<double>(_latency));
        _did_reset = false;
    }

    return Steinberg::kResultOk;
}

// MARK: - state load

Steinberg::tresult PLUGIN_API Vst3_processor::setState(Steinberg::IBStream* state)
{
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper. 
    auto streamer = Steinberg::IBStreamer{state};

    auto header = State_header{};
    if (!streamer.readInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    // Validate header.
    assert(header[0] == Plug_info::framework_code && "Unexpected framework code.");
    assert(header[1] == Plug_info::manufacturer_code && "Unexpected manufacturer code.");
    assert(header[2] == Plug_info::plugin_code && "Unexpected plug-in code.");

    const auto num_state_params = header[3];

    // Notify kernel (if not an interface parameter).
    auto do_notify = [this](const auto& param, auto plain_value) {
        if (param.policy != Host_policy::interface) {
            _queue.push(Set_param{param.address, plain_value});
        }
    };

    // Read processor state into temporary vector.
    auto state_params = std::vector<float>(num_state_params);
    for (auto i = decltype(num_state_params){}; i < num_state_params; ++i) {
        if (!streamer.readFloat(state_params[i])) {
            return Steinberg::kResultFalse;
        }
    }

    if (num_params <= num_state_params) {
        // Set values stored in state.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto knob_value = state_params[i];
            const auto& param = User_params::param_spec(i);
            const auto plain_value = Value_conv::knob_to_plain(knob_value, param.semantics);
            do_notify(param, plain_value);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_state_params){}; i < num_state_params; ++i) {
            const auto knob_value = state_params[i];
            const auto& param = User_params::param_spec(i);
            const auto plain_value = Value_conv::knob_to_plain(knob_value, param.semantics);
            do_notify(param, plain_value);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_state_params; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            const auto plain_value = get_plain_default(param);
            do_notify(param, plain_value);
        }
    }

    return Steinberg::kResultOk;
}

// MARK: - state save

Steinberg::tresult PLUGIN_API Vst3_processor::getState(Steinberg::IBStream* state)
{
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper. 
    auto streamer = Steinberg::IBStreamer{state};

    // Generate the header.
    auto header = State_header{
        Plug_info::framework_code, // Reserved
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_params
    };

    if (!streamer.writeInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        const auto& lpoint = _last_points[i]; // Grab value from last automation points.
        const auto knob_value = static_cast<float>(lpoint.value);
        if (!streamer.writeFloat(knob_value)) {
            return Steinberg::kResultFalse;
        }
    }

    return Steinberg::kResultOk;
}

// MARK: - latency, tail

Steinberg::uint32 PLUGIN_API Vst3_processor::getLatencySamples()
{
    return _latency;
}

Steinberg::uint32 PLUGIN_API Vst3_processor::getTailSamples()
{
    // Resolve to Steinberg's named constants.
    using namespace Steinberg::Vst;
    const auto tail = _processor->tail_samps();
    const auto inf_tail = std::numeric_limits<uint32_t>::max();
    return tail == 0 ? kNoTail : (tail == inf_tail ? kInfiniteTail : tail);
}

Steinberg::uint32 PLUGIN_API Vst3_processor::getProcessContextRequirements()
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

auto Vst3_processor::normalize_input_events(Steinberg::Vst::ProcessData& data) -> void
{
    auto& param_changes = *data.inputParameterChanges;
    const auto num_changes = param_changes.getParameterCount();

    for (auto i = decltype(num_changes){}; i < num_changes; ++i) {
        auto& queue = *param_changes.getParameterData(i);

        const auto id = queue.getParameterId();
        if (id >= User_params::num_params) continue; // Be defensive.

        const auto& param = User_params::param_spec(id); // To denormalize the automation values.

        const auto point_count = queue.getPointCount();

        auto& previous = _last_points[id];

        for (auto point_idx = decltype(point_count){}; point_idx < point_count; ++point_idx) {
            auto value = Steinberg::Vst::ParamValue{};
            auto offset = int32_t{};
            queue.getPoint(point_idx, offset, value);

            const auto ramp_dur = std::max(offset - previous.offset, 0);

            if (_events.size() == _events.capacity()) {
                // _events vector is full!
                std::cout << "Events vector full!\n";
            };

            // Set param
            if (ramp_dur <= 1) {
                _events.push_back({
                    .event = Set_param{
                        .address = id,
                        .value = Value_conv::knob_to_plain(value, param.semantics)
                    },
                    .offset = std::max(previous.offset, {}),
                });
            }
            // Ramp param
            else {
                _events.push_back({
                    .event = Ramp_param{
                        .address = id,
                        .target = Value_conv::knob_to_plain(value, param.semantics),
                        .dur_samples = ramp_dur
                    },
                    .offset = std::max(previous.offset, {}),
                });
            }

            previous.offset = offset;
            previous.value = value;
        }

        previous.offset = -1; // Next buffer starts with -1;
    }

    // sort events.
    std::ranges::sort(_events, [](const auto& a, const auto& b) { return a.offset < b.offset; });
}

} // namespace tiny