#include "auv2_effect.h"

#include <AudioUnitSDK/ComponentBase.h>

namespace tiny {

Auv2_effect::Auv2_effect(AudioUnit component) : Super(component, num_inputs, num_outputs)
{
    CreateElements(); // So we can create the sidechain.

    // Set up parameters.
    const auto& params = _params.kernel_specs();

    Globals()->UseIndexedParameters(User_params::num_params);
    for (const auto& param : params) {
        const auto def_val = get_host_default(param);
        Globals()->SetParameter(param.id, def_val);
    }

    // 
    for (size_t i = 0; i < num_inputs; ++i) {
        const auto is_main = (i == 0);
        const auto* input_name = is_main ? "Input" : "Sidechain";
        const auto str = CFStringCreateWithCString(kCFAllocatorDefault, input_name, kCFStringEncodingUTF8);
        Inputs().GetElement(i)->SetName(str);
    }

    const auto str = CFStringCreateWithCString(kCFAllocatorDefault, "Output", kCFStringEncodingUTF8);
    Outputs().GetElement(0)->SetName(str);
}

OSStatus Auv2_effect::Initialize()
{
    Super::Initialize();

    const auto format = GetStreamFormat(kAudioUnitScope_Output, 0);
    const auto sample_rate = format.mSampleRate;
    _kernel->reset(sample_rate);
    _latency = _kernel->latency_samps();
    _sr = sample_rate;

    _events.reserve(128);

    return noErr;
}

// MARK: - GetPropertyInfo

OSStatus Auv2_effect::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    switch (inID) {
        case kAudioUnitProperty_CocoaUI: {
            outDataSize = sizeof(AudioUnitCocoaViewInfo);
            outWritable = false;
            return noErr;
        }
        case kAudioUnitProperty_ParameterStringFromValue: {
            outDataSize = sizeof(AudioUnitParameterStringFromValue);
            outWritable = false;
            return noErr;
        }
        case kAudioUnitProperty_ParameterValueFromString: {
            outDataSize = sizeof(AudioUnitParameterValueFromString);
            outWritable = false;
            return noErr;
        }
        case kAudioUnitProperty_UserPlugin: {
            outDataSize = sizeof(void*);
            outWritable = false;
            return noErr;
        }
        default: break;
    }

    return Super::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
}

// MARK: - GetProperty

OSStatus Auv2_effect::GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, void* outData)
{
    if (inScope != kAudioUnitScope_Global || !outData) return kAudioUnitErr_InvalidScope;

    switch (inID) {
        case kAudioUnitProperty_CocoaUI: {
            auto* info = static_cast<AudioUnitCocoaViewInfo*>(outData);

            // Bundle
            auto id = CFStrLocal{Plug_info::Auv2::bundle_id};
            auto* bundle = CFBundleGetBundleWithIdentifier(id.Get());
            auto* url = CFBundleCopyBundleURL(bundle);

            info->mCocoaAUViewBundleLocation = url;
            info->mCocoaAUViewClass[0] = CFStringCreateWithCString(kCFAllocatorDefault, Plug_info::Auv2::view_class, kCFStringEncodingUTF8);
            return noErr;
        }
        case kAudioUnitProperty_ParameterStringFromValue: {
            auto* data = static_cast<AudioUnitParameterStringFromValue*>(outData);

            const auto id = data->inParamID;
            const auto& params = _params.kernel_specs();
            const auto& param = params[id];
            const auto str = Host_formatter::format_string(*data->inValue, param.semantics);
            data->outString = CFStringCreateWithCString(kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);

            return noErr;
        }
        case kAudioUnitProperty_ParameterValueFromString: {
            auto* data = static_cast<AudioUnitParameterValueFromString*>(outData);

            const auto id = data->inParamID;
            const auto& params = _params.kernel_specs();
            const auto& param = params[id];

            const auto str = cf_to_std(data->inString);

            if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
                data->outValue = Value_conv::plain_to_host(*plain, param.semantics);
                return noErr;
            }

            return kAudioUnitErr_InvalidPropertyValue;
        }
        case kAudioUnitProperty_UserPlugin: {
            ((void**)outData)[0] = (void*)this;
            return noErr;
        }
        default: break;
    }

    return Super::GetProperty(inID, inScope, inElement, outData);
}

// MARK: - GetParameterList

OSStatus Auv2_effect::GetParameterList(AudioUnitScope inScope, AudioUnitParameterID* outParameterList, UInt32& outNumParameters)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    // This is so we can determine the presentation order of the parameters by the host.
    outNumParameters = num_params; // Do this first.
    if (!outParameterList) return noErr;
    const auto& params = _params.presentation_specs();
    const auto ids = params | std::views::transform([](const auto& spec) { return spec.id; });
    std::ranges::copy(ids, outParameterList);

    return noErr;
}

// MARK: - GetParameterInfo

OSStatus Auv2_effect::GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID, AudioUnitParameterInfo& outParameterInfo)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    auto resolve_flags = [](const Param_spec& param, bool found_clump) {
        auto flags = AudioUnitParameterOptions{};

        flags |= (kAudioUnitParameterFlag_HasCFNameString | kAudioUnitParameterFlag_CFNameRelease);
        flags |= kAudioUnitParameterFlag_ValuesHaveStrings;

        if (found_clump) {
            flags |= kAudioUnitParameterFlag_HasClump;
        }

        if (!param.hidden) {
            flags |= (kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable);
        }

        return flags;
    };

    const auto& params = _params.kernel_specs();
    const auto& param = params[inParameterID];
    const auto* clump = find_clump_for_parameter(_clumps, param.id);
    const auto found_clump = clump != nullptr;

    std::visit(Inline_visitor{
        [&](const Bool_semantics& b) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? clump->id : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Boolean,
                .minValue = 0,
                .maxValue = 1,
                .defaultValue = static_cast<float>(b.def_val ? 1 : 0),
                .flags = resolve_flags(param, found_clump)
            };
        },
        [&](const List_semantics& l) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? clump->id : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Indexed,
                .minValue = 0,
                .maxValue = static_cast<float>(l.items.size() - 1),
                .defaultValue = static_cast<float>(l.def_val),
                .flags = resolve_flags(param, found_clump)
            };
        },
        [&](const Int_semantics& i) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? clump->id : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Indexed,
                .minValue = static_cast<float>(i.min_val),
                .maxValue = static_cast<float>(i.max_val),
                .defaultValue = static_cast<float>(i.def_val),
                .flags = resolve_flags(param, found_clump)
            };
        },
        [&](const Real_semantics&) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? clump->id : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Generic,
                .minValue = 0,
                .maxValue = 1,
                .defaultValue = static_cast<float>(get_host_default(param)),
                .flags = resolve_flags(param, found_clump) | (kAudioUnitParameterFlag_CanRamp | kAudioUnitParameterFlag_IsHighResolution)
            };
        },
    }, param.semantics);

    return noErr;
}

// MARK: - GetParameterValueByString

OSStatus Auv2_effect::GetParameterValueStrings(AudioUnitScope inScope, AudioUnitParameterID inParameterID, CFArrayRef* outStrings)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    if (!outStrings) return noErr;

    const auto& params = _params.kernel_specs();
    const auto& param = params[inParameterID];


    if (const auto* l = std::get_if<List_semantics>(&param.semantics)) {
        auto array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

        for (const auto* label : (*l).items) {
            if (!label) continue;
            const auto str = CFStringCreateWithCString(kCFAllocatorDefault, label, kCFStringEncodingUTF8);
            CFArrayAppendValue(array, str);
            CFRelease(str);
        }

        *outStrings = array;
        return noErr;
    }

    return kAudioUnitErr_InvalidPropertyValue;
}

// MARK: - CopyClumpName

OSStatus Auv2_effect::CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 /*inDesiredNameLength*/, CFStringRef* outClumpName)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    if (const auto* clump = find_clump(_clumps, inClumpID)) {
        const auto& name = clump->name;
        *outClumpName = CFStringCreateWithCString(0, name.c_str(), kCFStringEncodingUTF8);
        return noErr;
    }

    return kAudioUnitErr_InvalidPropertyValue;
}

// MARK: - GetParameter

OSStatus Auv2_effect::GetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement, AudioUnitParameterValue& outValue)
{
    return Super::GetParameter(inID, inScope, inElement, outValue);
}

// MARK: - SetParameter

OSStatus Auv2_effect::SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement, AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames)
{
    const auto& params = _params.kernel_specs();
    const auto& param = params[inID];

    const auto plain_value = Value_conv::host_to_plain(inValue, param.semantics);
    const auto knob_value = Value_conv::host_to_knob(inValue, param.semantics);

    _iqueue.push({
        .offset = static_cast<int32_t>(inBufferOffsetInFrames),
        .event = Set_param{.id = inID, .value = plain_value}
    });
    _oqueue.push(Set_param{.id = inID, .value = knob_value});

    return Super::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);
}

OSStatus Auv2_effect::ScheduleParameter(const AudioUnitParameterEvent* inParameterEvent, UInt32 inNumEvents)
{
    const auto& params = _params.kernel_specs();

    for (auto i = decltype(inNumEvents){}; i < inNumEvents; ++i) {
        const auto& event = inParameterEvent[i];
        const auto& param = params[event.parameter];

        switch (event.eventType) {
            case kParameterEvent_Immediate: {
                const auto offset = event.eventValues.immediate.bufferOffset;
                const auto value = event.eventValues.immediate.value;
                const auto plain_value = Value_conv::host_to_plain(value, param.semantics);
                const auto knob_value = Value_conv::host_to_knob(value, param.semantics);

                _iqueue.push({
                    .offset = static_cast<int32_t>(offset),
                    .event = Set_param{.id = event.parameter, .value = plain_value}
                });
                _oqueue.push(Set_param{.id = event.parameter, .value = knob_value});

                // Maintain host values.
                Super::SetParameter(event.parameter, event.scope, event.element, value, offset);
                break;
            }
            // Do any hosts even send these?
            case kParameterEvent_Ramped: {
                const auto offset = event.eventValues.ramp.startBufferOffset;
                const auto duration = event.eventValues.ramp.durationInFrames;
                const auto initial = event.eventValues.ramp.startValue;
                const auto target = event.eventValues.ramp.endValue;

                const auto plain_initial = Value_conv::host_to_plain(initial, param.semantics);
                const auto plain_target = Value_conv::host_to_plain(target, param.semantics);
                const auto knob_target = Value_conv::host_to_knob(target, param.semantics);

                // Do we need to be sending set initial?
                _iqueue.push({
                    .offset = offset,
                    .event = Set_param{.id = event.parameter, .value = plain_initial}
                });
                _iqueue.push({
                    .offset = offset,
                    .event = Ramp_param{
                        .id = event.parameter,
                        .target = plain_target,
                        .dur_samples = static_cast<int32_t>(duration)
                    }
                });
                _oqueue.push(Set_param{.id = event.parameter, .value = knob_target});

                // Maintain host values.
                Super::SetParameter(event.parameter, event.scope, event.element, target, offset + duration);
                break;
            }
        }
    }
    return noErr;
}

// MARK: - Render

OSStatus Auv2_effect::Render(AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& inTimeStamp, UInt32 nFrames)
{
    // Pull inputs.
    for (auto i = decltype(num_inputs){}; i < num_inputs; ++i) {
        Input(i).PullInput(ioActionFlags, inTimeStamp, i, nFrames);
    }

    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        _kernel->handle_event(Accepted_latency{*accepted_latency});
        assert(_kernel->latency_samps() == *accepted_latency && "Kernel must apply the accepted latency!");
    }

    _events.clear(); // Events are only valid for the current render cycle.
    auto param_event = Tagged_event{};
    while (_iqueue.pop(param_event)) {
        if (_events.size() == _events.capacity()) {
            std::cout << "Events vector full!\n";
        }
        _events.push_back(param_event);
    }

    // Sort the events such that Set_params precedes Ramp_params with the same buffer offset.
    std::ranges::sort(_events, [](const auto& a, const auto& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        return std::holds_alternative<Set_param>(a.event) && std::holds_alternative<Ramp_param>(b.event);
    });

    const auto event_count = _events.size();
    size_t event_index = 0;
    const auto* event = event_count > 0 ? &_events[event_index] : nullptr;

    auto next_event = [&]() {
        ++event_index;
        event = (event_index < event_count) ? &_events[event_index] : nullptr;
    };

    // Get host data.
    struct {
        Boolean is_playing{};
        Boolean is_recording{};
        Float64 sample_pos{};
        Boolean is_cycling{};
        Float64 cycle_start{};
        Float64 cycle_end{};
        Float64 beat_pos{};
        Float64 tempo{};
        Float32 time_sig_numer{};
        UInt32 time_sig_denom{};
    } host_data{};

    [[maybe_unused]] auto result = OSStatus{noErr};
    result = [this, &host_data]() {
        auto info = GetHostCallbackInfo();
        if (info.transportStateProc2) {
            return (*info.transportStateProc2)(
                info.hostUserData,
                &host_data.is_playing,
                &host_data.is_recording,
                nullptr, // transport_state_changed
                &host_data.sample_pos,
                &host_data.is_cycling,
                &host_data.cycle_start,
                &host_data.cycle_end
            );
        }
        else {
            return CallHostTransportState(
                &host_data.is_playing,
                nullptr, // transport_state_changed
                &host_data.sample_pos,
                &host_data.is_cycling,
                &host_data.cycle_start,
                &host_data.cycle_end
            );
        }
    }();

    result = CallHostBeatAndTempo(
        &host_data.beat_pos,
        &host_data.tempo
    );

    result = CallHostMusicalTimeLocation(
        nullptr, // sample_offset_to_next_beat
        &host_data.time_sig_numer,
        &host_data.time_sig_denom,
        nullptr // current_measure_downbeat
    );

    // Create the context.
    auto context = Dsp_context{.exports = _exports};

    auto do_process = [this, &context, &host_data](size_t num_frames, size_t offset) {
        for (size_t i = 0; i < num_ichannels; ++i) {
            _ibuffers[i] = static_cast<const float*>(Input(0).GetBufferList().mBuffers[i].mData) + offset;
        }
        for (size_t i = 0; i < num_ochannels; ++i) {
            _obuffers[i] = static_cast<float*>(Output(0).GetBufferList().mBuffers[i].mData) + offset;
        }
        if constexpr (Plug_info::wants_sidechain) {
            for (size_t i = 0; i < num_schannels; ++i) {
                _sbuffers[i] = static_cast<const float*>(Input(1).GetBufferList().mBuffers[i].mData) + offset;
            }
        }

        const auto sample_pos = host_data.sample_pos;
        const auto beat_pos = host_data.beat_pos;
        const auto cycle_start = host_data.cycle_start;
        const auto cycle_end = host_data.cycle_end;
        const auto tempo = host_data.tempo;
        const auto ts_numer = static_cast<int32_t>(host_data.time_sig_numer);
        const auto ts_denom = static_cast<int32_t>(host_data.time_sig_denom);

        const auto to_bool = [](auto a) { return a != 0; };

        context.musical_context = {
            .sample_pos = static_cast<int64_t>(sample_pos + offset),
            .beat_pos = beat_pos + frames_to_beats(offset, tempo, _sr),
            .cycle_start = cycle_start,
            .cycle_end = cycle_end,
            .tempo_ideal = tempo,
            .time_sig = {ts_numer, ts_denom},
            .transport_state = {
                .moving = to_bool(host_data.is_playing),
                .cycling = to_bool(host_data.is_cycling),
                .recording = to_bool(host_data.is_recording)
            }
        };

        context.ibuffers = _ibuffers;
        context.obuffers = _obuffers;
        context.sbuffers = _sbuffers;
        context.num_frames = num_frames;
        _kernel->process(context);
    };

    const auto frame_count = nFrames;
    auto now = decltype(frame_count){};
    auto remaining = frame_count;

    while (remaining > 0) {
        if (!event) {
            const auto offset = frame_count - remaining;
            do_process(remaining, offset);
            break;
        }

        const auto frames_until_event = std::max({}, event->offset - now);

        if (frames_until_event > 0) {
            const auto offset = frame_count - remaining;
            do_process(frames_until_event, offset);
            remaining -= frames_until_event;
            now += frames_until_event;
        }

        do {
            _kernel->handle_event(event->event);
            next_event();
        } while (event && static_cast<uint32_t>(event->offset) <= now);
    }

    // Send exports.
    for (auto i = decltype(num_exports){}; i < num_exports; ++i) {
        if (context.exports[i] != _lexports[i]) {
            // Send an output event.
            const auto value = context.exports[i];
            _oqueue.push(Set_export{.id = i, .value = value});

            // Cache for next time.
            _lexports[i] = value;
        }

        _exports[i] = 0; // Reset for peak meters.
    }

    // Has the kernel proposed a new latency?
    if (const auto proposed_latency = context.propose_latency; proposed_latency && *proposed_latency != _latency) {
        // Notify controller and sit on the pending latency.
        _pqueue.push({.type = Message_type::latency_changed});
        _pending_latency.store(*proposed_latency, std::memory_order_release);
    }

    return noErr;
}

// MARK: - create_view 

void* Auv2_effect::create_view()
{
    return _view->create_view();
}

// MARK: - entry

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Auv2_effect);

} // namespace tiny