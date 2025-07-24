#include "auv2_effect.h"

#include <AudioUnitSDK/ComponentBase.h>

Auv2_effect::Auv2_effect(AudioUnit component) : Super(component, num_inputs, num_outputs)
{
    CreateElements();

    using namespace tiny;
    const auto& tree = Param_model::build_tree();
    _clumps = tiny::auv2::tree_to_clump_map(tree);

    const auto& params = _params.get_kernel_specs();

    // Set up parameters.
    Globals()->UseIndexedParameters(User_params::num_params);
    for (const auto& param : params) {
        const auto def_val = get_host_default(param);
        Globals()->SetParameter(to_underlying(param.id), def_val);
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
    const auto max_frames = Super::GetMaxFramesPerSlice();
    _kernel->reset(sample_rate, max_frames);

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
            auto id = tiny::auv2::CFStrLocal{tiny::Plug_info::Auv2::bundle_id};
            auto* bundle = CFBundleGetBundleWithIdentifier(id.Get());
            auto* url = CFBundleCopyBundleURL(bundle);

            info->mCocoaAUViewBundleLocation = url;
            info->mCocoaAUViewClass[0] = CFStringCreateWithCString(kCFAllocatorDefault, tiny::Plug_info::Auv2::view_class, kCFStringEncodingUTF8);
            return noErr;
        }
        case kAudioUnitProperty_ParameterStringFromValue: {
            auto* data = static_cast<AudioUnitParameterStringFromValue*>(outData);

            using namespace tiny;
            const auto id = data->inParamID;
            const auto& params = _params.get_kernel_specs();
            const auto& param = params[id];
            const auto str = Host_formatter::format_string(*data->inValue, param.semantics);
            data->outString = CFStringCreateWithCString(kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);

            return noErr;
        }
        case kAudioUnitProperty_ParameterValueFromString: {
            auto* data = static_cast<AudioUnitParameterValueFromString*>(outData);

            using namespace tiny;
            const auto id = data->inParamID;
            const auto& params = _params.get_kernel_specs();
            const auto& param = params[id];

            const auto str = tiny::auv2::cf_to_std(data->inString);

            if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
                data->outValue = plain_to_host_space(*plain, param);
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
    using namespace tiny;
    outNumParameters = User_params::num_params; // Do this first.
    if (!outParameterList) return noErr;
    const auto& params = _params.get_presentation_specs();
    const auto ids = params | std::views::transform([](const auto& spec) { return to_underlying(spec.id); });
    std::ranges::copy(ids, outParameterList);

    return noErr;
}

// MARK: - GetParameterInfo

OSStatus Auv2_effect::GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID, AudioUnitParameterInfo& outParameterInfo)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    using namespace tiny;

    auto resolve_flags = [](const User_params::Spec& param, bool found_clump) {
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

    const auto& params = _params.get_kernel_specs();
    const auto& param = params[inParameterID];
    const auto* clump = tiny::auv2::find_clump_for_parameter(_clumps, to_underlying(param.id));
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
                .maxValue = static_cast<float>(l.labels.size() - 1),
                .defaultValue = static_cast<float>(l.def_val),
                .flags = resolve_flags(param, found_clump)
            };
        },
        [&](const Float_semantics& f) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? clump->id : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Generic,
                .minValue = 0,
                .maxValue = 1,
                .defaultValue = static_cast<float>(f.knob_adapter.plain_to_norm(f, f.def_val)),
                .flags = resolve_flags(param, found_clump) | (kAudioUnitParameterFlag_CanRamp | kAudioUnitParameterFlag_IsHighResolution)
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
        }
    }, param.semantics);

    return noErr;
}

// MARK: - GetParameterValueByString

OSStatus Auv2_effect::GetParameterValueStrings(AudioUnitScope inScope, AudioUnitParameterID inParameterID, CFArrayRef* outStrings)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
    if (!outStrings) return noErr;

    using namespace tiny;

    const auto& params = _params.get_kernel_specs();
    const auto& param = params[inParameterID];

    
    if (const auto* l = std::get_if<List_semantics>(&param.semantics)) {
        auto array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

        for (const auto* label : (*l).labels) {
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

    if (const auto* clump = tiny::auv2::find_clump(_clumps, inClumpID)) {
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
    const auto& params = _params.get_kernel_specs();
    const auto& param = params[inID];

    _queue.push({
        .offset = static_cast<int32_t>(inBufferOffsetInFrames),
        .event = tiny::Set_param{.id = inID, .value = tiny::host_to_plain_space(inValue, param)}
    });

    return Super::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);
}

OSStatus Auv2_effect::ScheduleParameter(const AudioUnitParameterEvent* inParameterEvent, UInt32 inNumEvents)
{
    const auto& params = _params.get_kernel_specs();
    for (auto i = decltype(inNumEvents){}; i < inNumEvents; ++i) {
        const auto& event = inParameterEvent[i];
        const auto& param = params[event.parameter];

        switch (event.eventType) {
            case kParameterEvent_Immediate: {
                const auto offset = event.eventValues.immediate.bufferOffset;
                const auto value = event.eventValues.immediate.value;
                _queue.push({
                    .offset = static_cast<int32_t>(offset),
                    .event = tiny::Set_param{
                        .id = event.parameter,
                        .value = tiny::host_to_plain_space(value, param)
                    }
                });
                Super::SetParameter(event.parameter, event.scope, event.element, value, offset);
                break;
            }
            // Do any hosts even send these?
            case kParameterEvent_Ramped: {
                const auto offset = event.eventValues.ramp.startBufferOffset;
                const auto duration = event.eventValues.ramp.durationInFrames;
                const auto initial = event.eventValues.ramp.startValue;
                const auto target = event.eventValues.ramp.endValue;
                _queue.push({
                    .offset = offset,
                    .event = tiny::Set_param{
                        .id = event.parameter,
                        .value = initial
                    }
                });
                _queue.push({
                    .offset = offset,
                    .event = tiny::Ramp_param{
                        .id = event.parameter,
                        .target = target,
                        .dur_samples = static_cast<int32_t>(duration)
                    }
                });
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

    using namespace tiny;

    _events.clear(); // Events are only valid for the current render cycle.
    auto param_event = Tagged_event{};
    while (_queue.pop(param_event)) {
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

    auto do_process = [this](size_t num_frames, size_t offset) {
        _ibuffers[0] = static_cast<const float*>(Input(0).GetBufferList().mBuffers[0].mData) + offset;
        _ibuffers[1] = static_cast<const float*>(Input(0).GetBufferList().mBuffers[1].mData) + offset;

        if constexpr (tiny::Plug_info::wants_sidechain) {
            _ibuffers[2] = static_cast<const float*>(Input(1).GetBufferList().mBuffers[0].mData) + offset;
            _ibuffers[3] = static_cast<const float*>(Input(1).GetBufferList().mBuffers[1].mData) + offset;
        }

        _obuffers[0] = static_cast<float*>(Output(0).GetBufferList().mBuffers[0].mData) + offset;
        _obuffers[1] = static_cast<float*>(Output(0).GetBufferList().mBuffers[1].mData) + offset;

        auto context = Dsp_context{
            .ibuffers = _ibuffers,
            .obuffers = _obuffers,
            .num_frames = num_frames
        };
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

    return noErr;
}

// MARK: - create_view 

void* Auv2_effect::create_view()
{
    platform_view = Platform_views::make_autoreleasing(_delegate);
    return platform_view->native_handle();
}

// MARK: - entry

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Auv2_effect);