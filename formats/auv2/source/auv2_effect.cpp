#include "auv2_effect.h"

#include <AudioUnitSDK/ComponentBase.h>
#include <AudioUnitSDK/AUUtility.h> // Serialize

namespace tiny {

Auv2_effect::Auv2_effect(AudioUnit component) : Super{component, num_inputs, num_outputs}
{
    _editor.emplace(_tasks.actor());

#if TINY_HAS_WORKER
    try_bind_worker(*_processor, Worker_processor_actor{
        [this](const auto& m) { return _worker_from_proc.push(m); }
    });
    try_bind_worker(*_editor, Worker_editor_actor{
        [this](const auto& m) { return _worker_from_edit.push(m); }
    });
#endif

    this->_retain_presets();

    CreateElements(); // So we can create the sidechain.

    // Set up parameters.
    _clumps = tree_to_clump_map(User_params::param_tree());
    const auto& params = User_params::param_specs(Param_order::Indexable);

    Globals()->UseIndexedParameters(User_params::num_params);
    for (const auto& param : params) {
        const auto def_val = get_host_default(param);
        Globals()->SetParameter(param.address, static_cast<float>(def_val));
    }

    // 
    for (size_t i = 0; i < num_inputs; ++i) {
        const auto is_main = (i == 0);
        const auto* input_name = is_main ? "Input" : "Sidechain";
        const auto str = CFStringCreateWithCString(kCFAllocatorDefault, input_name, kCFStringEncodingUTF8);
        Inputs().GetElement(static_cast<UInt32>(i))->SetName(str);
    }

    const auto str = CFStringCreateWithCString(kCFAllocatorDefault, "Output", kCFStringEncodingUTF8);
    Outputs().GetElement(0)->SetName(str);
}

Auv2_effect::~Auv2_effect()
{
    this->_release_presets();
}

OSStatus Auv2_effect::Initialize()
{
    Super::Initialize();

    const auto format = GetStreamFormat(kAudioUnitScope_Output, 0);
    const auto sample_rate = format.mSampleRate;
    _processor->reset(sample_rate);
    _latency = _processor->latency_samps();
    _sr = sample_rate;

    _bypass.reset(static_cast<float>(sample_rate));
    _bypass.set_latency(_latency);

    _events.reserve(4 * num_params + 1);

#if TINY_HAS_WORKER
    _worker_runner.start(sample_rate);
#endif

    return noErr;
}

// MARK: - GetPropertyInfo

OSStatus Auv2_effect::GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    switch (inID) {
        case kAudioUnitProperty_BypassEffect: {
            outDataSize = sizeof(UInt32);
            outWritable = true;
            return noErr;
        }
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
    using namespace ausdk; // Serialize

    if (inScope != kAudioUnitScope_Global || !outData) return kAudioUnitErr_InvalidScope;

    switch (inID) {
        case kAudioUnitProperty_BypassEffect: {
            const auto bypass = _bypass.is_bypassed();
            Serialize<UInt32>(bypass ? 1 : 0, outData);
            return noErr;
        }
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
            const auto& params = User_params::param_specs(Param_order::Indexable);
            const auto& param = params[id];
            const auto str = Host_formatter::format_string(*data->inValue, param.semantics);
            data->outString = CFStringCreateWithCString(kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);

            return noErr;
        }
        case kAudioUnitProperty_ParameterValueFromString: {
            auto* data = static_cast<AudioUnitParameterValueFromString*>(outData);

            const auto id = data->inParamID;
            const auto& params = User_params::param_specs(Param_order::Indexable);
            const auto& param = params[id];

            const auto str = cf_to_std(data->inString);

            if (const auto plain = Host_formatter::format_value(str, param.semantics)) {
                const auto host_value = Value_conv::plain_to_host(*plain, param.semantics);
                data->outValue = static_cast<float>(host_value);
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

OSStatus Auv2_effect::SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, const void* inData, UInt32 inDataSize)
{
    using namespace ausdk; // Serialize

    if (inScope != kAudioUnitScope_Global || !inData) return kAudioUnitErr_InvalidScope;

    switch (inID) {
        case kAudioUnitProperty_BypassEffect: {
            const auto bypass = Deserialize<UInt32>(inData) != 0;
            _bypass.set_bypassed(bypass);
            return noErr;
        }
        default: break;
    }

    return Super::SetProperty(inID, inScope, inElement, inData, inDataSize);
}

// MARK: - GetParameterList

OSStatus Auv2_effect::GetParameterList(AudioUnitScope inScope, AudioUnitParameterID* outParameterList, UInt32& outNumParameters)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    // This is so we can determine the presentation order of the parameters by the host.
    outNumParameters = num_params; // Do this first.
    if (!outParameterList) return noErr;
    const auto& params = User_params::param_specs(Param_order::Presentation);
    const auto ids = params | std::views::transform([](const auto& spec) { return spec.address; });
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

        using enum Host_policy;
        switch (param.policy) {
            case automation: {
                flags |= (kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable);
                break;
            }
            case control: {
                //flags |= kAudioUnitParameterFlag_IsReadable; // Logic shows a uneditable control.
                break;
            }
            case hidden: {
                // Hidden but part of state.
                break;
            }
            case interface: {
                flags |= kAudioUnitParameterFlag_OmitFromPresets;
                break;
            }
            default:
                break;
        }

        return flags;
    };

    const auto& params = User_params::param_specs(Param_order::Indexable);
    const auto& param = params[inParameterID];
    const auto* clump = find_clump_for_parameter(_clumps, param.address);
    const auto found_clump = clump != nullptr;

    std::visit(Inline_visitor{
        [&](const Bool_semantics& b) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? static_cast<UInt32>(clump->id) : UInt32{},
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
                .clumpID = found_clump ? static_cast<UInt32>(clump->id) : UInt32{},
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
                .clumpID = found_clump ? static_cast<UInt32>(clump->id) : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Indexed,
                .minValue = static_cast<float>(i.min_val),
                .maxValue = static_cast<float>(i.max_val),
                .defaultValue = static_cast<float>(i.def_val),
                .flags = resolve_flags(param, found_clump)
            };
        },
        [&](const Fixed_semantics& f) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? static_cast<UInt32>(clump->id) : UInt32{},
                .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
                .unit = kAudioUnitParameterUnit_Generic,
                .minValue = static_cast<float>(f.min_val),
                .maxValue = static_cast<float>(f.max_val),
                .defaultValue = static_cast<float>(f.def_val),
                .flags = resolve_flags(param, found_clump)
            };
        },
        [&](const Real_semantics&) {
            outParameterInfo = {
                .name = {},
                .unitName = {},
                .clumpID = found_clump ? static_cast<UInt32>(clump->id) : UInt32{},
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

    const auto& params = User_params::param_specs(Param_order::Indexable);
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

    if (const auto* clump = find_clump(_clumps, static_cast<int32_t>(inClumpID))) {
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
    if (inID >= num_params) return kAudioUnitErr_InvalidParameter;

    const auto& params = User_params::param_specs(Param_order::Indexable);
    const auto& param = params[inID];

    const auto plain_value = Value_conv::host_to_plain(inValue, param.semantics);

    [[maybe_unused]] const auto success = _to_processor.push(Tagged_event{
        .event = Set_param{.address = inID, .value = plain_value},
        .offset = static_cast<int32_t>(inBufferOffsetInFrames),
    });
    assert(success && "Push to processor queue failed! Increase queue size.");

    return Super::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);
}

OSStatus Auv2_effect::ScheduleParameter(const AudioUnitParameterEvent* inParameterEvent, UInt32 inNumEvents)
{
    if (!inParameterEvent) return kAudioUnitErr_InvalidParameter;

    const auto& params = User_params::param_specs(Param_order::Indexable);

    for (auto i = decltype(inNumEvents){}; i < inNumEvents; ++i) {
        const auto& event = inParameterEvent[i];

        if (event.scope != kAudioUnitScope_Global || event.element != 0) continue;
        if (event.parameter >= num_params) continue; // Invalid parameter, skip.

        const auto& param = params[event.parameter];

        switch (event.eventType) {
            case kParameterEvent_Immediate: {
                const auto offset = event.eventValues.immediate.bufferOffset;
                const auto value = event.eventValues.immediate.value;
                const auto plain_value = Value_conv::host_to_plain(value, param.semantics);

                [[maybe_unused]] const auto success = _to_processor.push(Tagged_event{
                    .event = Set_param{.address = event.parameter, .value = plain_value},
                    .offset = static_cast<int32_t>(offset),
                });
                assert(success && "Push to processor queue failed! Increase queue size.");

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

                // Do we need to be sending set initial?
                [[maybe_unused]] const auto set_success = _to_processor.push(Tagged_event{
                    .event = Set_param{.address = event.parameter, .value = plain_initial},
                    .offset = offset,
                });
                assert(set_success && "Push to processor queue failed! Increase queue size.");

                [[maybe_unused]] const auto ramp_success = _to_processor.push(Tagged_event{
                    .event = Ramp_param{
                        .address = event.parameter,
                        .target = plain_target,
                        .dur_samples = static_cast<int32_t>(duration)
                    },
                    .offset = offset,
                });
                assert(ramp_success && "Push to processor queue failed! Increase queue size.");

                // Maintain host values.
                const auto off = static_cast<UInt32>(offset);
                Super::SetParameter(event.parameter, event.scope, event.element, target, off + duration);
                break;
            }
            default:
                break;
        }
    }
    return noErr;
}

auto Auv2_effect::_update_state(const Maybe_values<double>& knob_values, const State_map& editor_state) -> void
{
    // Notify kernel and view (if not an interface parameter).
    auto change_list = std::vector<Set_param>{};

    auto notify = [&](const auto& param, auto knob_value) {
        const auto can_notify = knob_value.has_value() && State_rules::is_persistent(param);
        if (can_notify) {
            const auto host = Value_conv::knob_to_host(*knob_value, param.semantics);
            const auto plain = Value_conv::knob_to_plain(*knob_value, param.semantics);
            Globals()->SetParameter(param.address, static_cast<float>(host));
            change_list.push_back(Set_param{param.address, plain}); // We'll publish as a batch.
        }
    };

    const auto num_stored_values = knob_values.size();

    if (num_params <= num_stored_values) {
        // Read as many values as we can.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& param = User_params::param_spec(static_cast<uint32_t>(i));
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
            const auto knob_value = get_knob_default(param);
            notify(param, std::optional<double>{knob_value});
        }
    }

    _changes.push_n(change_list); // Batch publish everything.

    // Editor
    _editor->load_state(editor_state);
}

// MARK: - save state

OSStatus Auv2_effect::SaveState(CFPropertyListRef* outData)
{
    const auto result = Super::SaveState(outData);
    if (result != noErr) return result;

    // Cast to mutable dictionary (yee haw).
    auto dict = const_cast<CFMutableDictionaryRef>(reinterpret_cast<CFDictionaryRef>(*outData));
    if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) return kAudioUnitErr_InvalidProperty;

    // Get the editor state.
    const auto edit_state = _editor->save_state();
    const auto num_editor_items = static_cast<int32_t>(edit_state.size());

    // Helper
    auto set_num = [&](const auto key, const auto value) {
        const auto cf_key = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        const auto cf_num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value);
        CFDictionarySetValue(dict, cf_key, cf_num);
        CFRelease(cf_num);
        CFRelease(cf_key);
    };

    // Required!
    set_num(State_rules::Auv2::num_params, static_cast<int32_t>(num_params));
    set_num(State_rules::Auv2::num_editor_items, num_editor_items);

    if (num_editor_items > 0) {
        // Serialize editor state.
        auto editor_data = ausdk::Owned<CFMutableDataRef>::from_create(CFDataCreateMutable(nullptr, 0));

        auto write_value = [&](const auto& value) {
            CFDataAppendBytes(*editor_data, reinterpret_cast<const UInt8*>(&value), sizeof(value));
        };

        auto write_container = [&](const auto& container) {
            const auto num = static_cast<uint32_t>(container.size());
            CFDataAppendBytes(*editor_data, reinterpret_cast<const UInt8*>(&num), sizeof(num));
            CFDataAppendBytes(*editor_data, reinterpret_cast<const UInt8*>(container.data()), sizeof(container[0]) * num);
        };

        for (const auto& [key, val] : edit_state) {
            write_container(key);
            const auto tag = tag_for(val);
            write_value(tag);
            switch (tag) {
                case State_tag::Bool: {
                    const auto value = std::get_if<bool>(&val);
                    if (value) write_value(*value);
                    break;
                }
                case State_tag::Int: {
                    const auto value = std::get_if<int32_t>(&val);
                    if (value) write_value(*value);
                    break;
                }
                case State_tag::Double: {
                    const auto value = std::get_if<double>(&val);
                    if (value) write_value(*value);
                    break;
                }
                case State_tag::String: {
                    const auto value = std::get_if<std::string>(&val);
                    if (value) write_container(*value);
                    break;
                }
                default:
                    break;
            }
        }

        const auto data_key = CFStringCreateWithCString(kCFAllocatorDefault, State_rules::Auv2::editor_state_map, kCFStringEncodingUTF8);
        [[maybe_unused]] const auto release_editor_key = Deferred{[&]() { if (data_key) CFRelease(data_key); }};
        CFDictionarySetValue(dict, data_key, *editor_data);
    }

    return result;
}

// MARK: - restore state

OSStatus Auv2_effect::RestoreState(CFPropertyListRef plist)
{
    const auto result = Super::RestoreState(plist); // Base class maintains Globals().
    if (result != noErr) return result;

    // Cast to dictionary.
    auto dict = reinterpret_cast<CFDictionaryRef>(plist);
    if (CFGetTypeID(dict) != CFDictionaryGetTypeID()) return result;

    // Helper
    auto read_num = [&](const auto key, auto& out) {
        const auto cf_key = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
        const auto value = CFDictionaryGetValue(dict, cf_key);
        CFRelease(cf_key);
        if (value && CFGetTypeID(value) == CFNumberGetTypeID()) {
            CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt32Type, &out);
            return true;
        }
        return false;
    };

    // --- Parameter Values ---
    auto params_val = int32_t{};
    if (read_num(State_rules::Auv2::num_params, params_val)) {
        const auto num_stored_params = static_cast<uint32_t>(params_val);

        if (num_params <= num_stored_params) {
            // The base implementation already set Globals() for us.
        }
        else {
            // Set remaining Globals() to defaults.
            for (auto i = num_stored_params; i < num_params; ++i) {
                const auto& param = User_params::param_spec(i);
                const auto host_value = get_host_default(param);
                Globals()->SetParameter(i, static_cast<float>(host_value));
            }
        }

        // Globals() now contains the full state. Notify everyone.
        // Interface parameters were omitted for us by the base implementation!
        auto change_list = std::vector<Set_param>{};

        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            const auto host_value = Globals()->GetParameter(i);
            const auto plain_value = Value_conv::host_to_plain(host_value, param.semantics);

            change_list.push_back(Set_param{param.address, plain_value});
        }

        _changes.push_n(change_list); // Batch publish everything.
    }

    // --- Editor State ---
    auto editor_val = int32_t{};
    if (read_num(State_rules::Auv2::num_editor_items, editor_val)) {
        const auto num_editor_items = static_cast<uint32_t>(editor_val);
        if (num_editor_items == 0) return result; // No editor state to read.

        // Get the data.
        const auto data_key = CFStringCreateWithCString(kCFAllocatorDefault, State_rules::Auv2::editor_state_map, kCFStringEncodingUTF8);
        [[maybe_unused]] const auto release_editor_key = Deferred{[&]() { if (data_key) CFRelease(data_key); }};

        const auto* editor_data = static_cast<CFDataRef>(CFDictionaryGetValue(dict, data_key));
        if (!editor_data || CFGetTypeID(editor_data) != CFDataGetTypeID()) return result;

        auto offset = CFIndex{}; // Keep track of where we are.

        auto read_value = [&](auto& value) {
            const auto size = static_cast<CFIndex>(sizeof(value));
            if (offset + size <= CFDataGetLength(editor_data)) {
                CFDataGetBytes(editor_data, CFRange{offset, size}, reinterpret_cast<UInt8*>(&value));
                offset += size;
                return true;
            }
            return false;
        };

        auto read_container = [&](auto& container) {
            using Element = typename std::decay<decltype(container)>::type::value_type; // ...
            auto num = uint32_t{};
            if (read_value(num)) {
                const auto size = static_cast<CFIndex>(sizeof(Element) * num);
                if (offset + size <= CFDataGetLength(editor_data)) {
                    container.resize(num);
                    CFDataGetBytes(editor_data, CFRange{offset, size}, reinterpret_cast<UInt8*>(container.data()));
                    offset += size;
                    return true;
                }
            }
            return false;
        };

        auto edit_state = State_map{};
        for (auto i = decltype(num_editor_items){}; i < num_editor_items; ++i) {
            auto key = std::string{};
            if (!read_container(key)) break;

            auto tag = State_tag{};
            if (!read_value(tag)) break;

            auto value = State_item{};
            switch (tag) {
                case State_tag::Bool: {
                    auto v = bool{};
                    if (read_value(v)) {
                        value = v;
                    }
                    break;
                }
                case State_tag::Int: {
                    auto v = int32_t{};
                    if (read_value(v)) {
                        value = v;
                    }
                    break;
                }
                case State_tag::Double: {
                    auto v = double{};
                    if (read_value(v)) {
                        value = v;
                    }
                    break;
                }
                case State_tag::String: {
                    auto v = std::string{};
                    if (read_container(v)) {
                        value = std::move(v);
                    }
                    break;
                }
                default:
                    break;
            }

            edit_state.emplace(std::move(key), std::move(value));
        }

        _editor->load_state(edit_state);
    }
    
    return result;
}

// MARK: - Presets

OSStatus Auv2_effect::GetPresets(CFArrayRef* outData) const
{
    if (!outData) return noErr;

    auto mutable_arr = CFArrayCreateMutable(kCFAllocatorDefault, Preset_list::num_presets, nullptr);
    for (auto i = size_t{0}; i < Preset_list::num_presets; ++i) {
        CFArrayAppendValue(mutable_arr, &_au_presets[i]);
    }

    *outData = (CFArrayRef)mutable_arr;
    return noErr;
}

OSStatus Auv2_effect::NewFactoryPresetSet(const AUPreset& inNewFactoryPreset)
{
    const auto preset_number = static_cast<size_t>(inNewFactoryPreset.presetNumber);
    if (preset_number >= Preset_list::num_presets) return kAudioUnitErr_InvalidPropertyValue;

    // The preset is a file in the bundle resources with the native extension.
    const auto preset_name = CFStringCreateWithCString(kCFAllocatorDefault, Preset_list::names[preset_number], kCFStringEncodingUTF8);
    [[maybe_unused]] const auto release_preset_name = Deferred{[&]() { if (preset_name) CFRelease(preset_name); }};

    const auto preset_ext = CFStringCreateWithCString(kCFAllocatorDefault, Plug_info::Presets::extension, kCFStringEncodingUTF8);
    [[maybe_unused]] const auto release_preset_ext = Deferred{[&]() { if (preset_ext) CFRelease(preset_ext); }};

    const auto bundle_id = CFStringCreateWithCString(kCFAllocatorDefault, Plug_info::Auv2::bundle_id, kCFStringEncodingUTF8);
    [[maybe_unused]] const auto release_bundle_id = Deferred{[&]() { if (bundle_id) CFRelease(bundle_id); }};

    auto bundle = CFBundleGetBundleWithIdentifier(bundle_id);
    if (!bundle) return kAudioUnitErr_InvalidPropertyValue;

    auto preset_url = CFBundleCopyResourceURL(bundle, preset_name, preset_ext, nullptr);
    if (!preset_url) return kAudioUnitErr_InvalidPropertyValue;
    [[maybe_unused]] const auto release_preset_url = Deferred{[&]() { if (preset_url) CFRelease(preset_url); }};

    if (!preset_url) return kAudioUnitErr_InvalidPropertyValue;

    auto json_data = CFDataRef{nullptr};
    [[maybe_unused]] const auto release_json_data = Deferred{[&]() { if (json_data) CFRelease(json_data); }};

    // CFURLCreateDataAndPropertiesFromResource is deprecated, we need to use CFReadStream to get the json_data.
    CFReadStreamRef stream = CFReadStreamCreateWithFile(kCFAllocatorDefault, preset_url);
    if (!stream) return kAudioUnitErr_InvalidPropertyValue;
    [[maybe_unused]] const auto release_stream = Deferred{[&]() { if (stream) CFRelease(stream); }};

    if (CFReadStreamOpen(stream)) {
        auto data = CFDataCreateMutable(kCFAllocatorDefault, 0); // We're moving this out later.

        auto buffer = std::vector<UInt8>(4096);
        auto bytes_read = CFIndex{0};
        do {
            bytes_read = CFReadStreamRead(stream, buffer.data(), static_cast<CFIndex>(buffer.size()));
            if (bytes_read > 0) {
                CFDataAppendBytes(data, buffer.data(), bytes_read);
            }
        } while (bytes_read > 0);
        json_data = data;
        CFReadStreamClose(stream);
    }

    if (!json_data) return kAudioUnitErr_InvalidPropertyValue;

    try {
        const auto* raw_bytes = reinterpret_cast<const char*>(CFDataGetBytePtr(json_data));
        const auto size = static_cast<size_t>(CFDataGetLength(json_data));
        auto json = nlohmann::json::parse(raw_bytes, raw_bytes + size);

        const auto param_values = _state_adapter.param_values(json);
        const auto editor_state = _state_adapter.editor_state(json);
        this->_update_state(param_values, editor_state);
    }
    catch (...) {
        return kAudioUnitErr_InvalidPropertyValue;
    }
    
    return noErr;
}

// MARK: - Render

OSStatus Auv2_effect::Render(AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& inTimeStamp, UInt32 nFrames)
{
    this->_drain_worker_to_processor();

    // Pull inputs.
    for (auto i = decltype(num_inputs){}; i < num_inputs; ++i) {
        Input(i).PullInput(ioActionFlags, inTimeStamp, i, nFrames);
    }

    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        const auto new_latency = static_cast<uint32_t>(*accepted_latency);
        _processor->handle_event(Accepted_latency{new_latency});
        _bypass.set_latency(new_latency);
        assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
    }

    _events.clear(); // Events are only valid for the current render cycle.

    _changes.consume([&](auto address, auto value) {
        assert(_events.size() < _events.capacity() && "Events vector full!");
        _events.push_back(Tagged_event{
            .event = Set_param{.address = address, .value = value},
            .offset = 0,
        });
    });


    auto param_event = Tagged_event{};
    while (_to_processor.pop(param_event)) {
        assert(_events.size() < _events.capacity() && "Events vector full!");
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
    auto context = Dsp_context{.meters = _meters};

    auto do_process = [this, &context, &host_data](size_t num_frames, size_t offset) {
        const auto num_ichannels = Input(0).NumberChannels();
        [[maybe_unused]] const auto num_ibuffers = Input(0).GetBufferList().mNumberBuffers;
        assert(num_ichannels == num_ibuffers && "Channel mismatch!");
        for (size_t i = 0; i < num_ichannels; ++i) {
            _ibuffers[i] = static_cast<const float*>(Input(0).GetFloat32ChannelData(static_cast<UInt32>(i))) + offset;
        }

        const auto num_ochannels = Output(0).NumberChannels();
        [[maybe_unused]] const auto num_obuffers = Output(0).GetBufferList().mNumberBuffers;
        assert(num_ochannels == num_obuffers && "Channel mismatch!");
        for (size_t i = 0; i < num_ochannels; ++i) {
            _obuffers[i] = static_cast<float*>(Output(0).GetFloat32ChannelData(static_cast<UInt32>(i))) + offset;
        }

        auto num_schannels = size_t{};
        if (Plug_info::wants_sidechain && HasInput(1)) {
            num_schannels = Input(1).NumberChannels();
            [[maybe_unused]] const auto num_sbuffers = Input(1).GetBufferList().mNumberBuffers;
            assert(num_schannels == num_sbuffers && "Channel mismatch!");
            for (size_t i = 0; i < num_schannels; ++i) {
                _sbuffers[i] = static_cast<const float*>(Input(1).GetFloat32ChannelData(static_cast<UInt32>(i))) + offset;
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
        const auto doff = static_cast<double>(offset);

        context.musical_context = {
            .sample_pos = static_cast<int64_t>(sample_pos + doff),
            .beat_pos = beat_pos + frames_to_beats(static_cast<int64_t>(offset), tempo, _sr),
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

        context.ibuffers = {_ibuffers.begin(), num_ichannels};
        context.obuffers = {_obuffers.begin(), num_ochannels};
        context.sbuffers = {_sbuffers.begin(), num_schannels};
        context.num_frames = num_frames;
        _processor->process(context);
    };

    const auto frame_count = nFrames;
    auto now = decltype(frame_count){};
    auto remaining = frame_count;

    const auto can_skip = _bypass.can_skip_effect();

    if (can_skip) {
        // Manifest events until end of block.
        while (event) {
            _processor->handle_event(event->event);
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

            // Events with negative offsets should start now.
            const auto event_offset = std::max(int32_t{}, event->offset);
            const auto frames_until_event = std::max({}, static_cast<uint32_t>(event_offset) - now);

            if (frames_until_event > 0) {
                const auto offset = frame_count - remaining;
                do_process(frames_until_event, offset);

                const auto advance = std::min(frames_until_event, remaining);
                remaining -= advance;
                now += advance;
            }

            do {
                _processor->handle_event(event->event);
                next_event();
            } while (event && event->offset <= static_cast<int32_t>(now));
        }
    }

    auto in_buffers = [&]() {
        auto arr = std::array<const float*, max_ichannels>{};
        for (size_t i = 0; i < Input(0).NumberChannels(); ++i) {
            arr[i] = static_cast<const float*>(Input(0).GetFloat32ChannelData(static_cast<UInt32>(i)));
        }
        return arr;
    }();

    auto out_buffers = [&]() {
        auto arr = std::array<float*, max_ochannels>{};
        for (size_t i = 0; i < Output(0).NumberChannels(); ++i) {
            arr[i] = static_cast<float*>(Output(0).GetFloat32ChannelData(static_cast<UInt32>(i)));
        }
        return arr;
    }();

    const auto min_channels = std::min(Input(0).NumberChannels(), Output(0).NumberChannels());
    const auto num_channels = static_cast<size_t>(min_channels);
    _bypass.process({in_buffers.begin(), num_channels}, {out_buffers.begin(), num_channels}, frame_count);

    // Send exports.
    for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
        if (context.meters[i] != _last_meters[i]) {
            // Send an output event.
            const auto value = context.meters[i];
            _meter_queue.push(Set_meter{.address = i, .value = value});

            // Cache for next time.
            _last_meters[i] = value;
        }

        _meters[i] = 0; // Reset for peak meters.
    }

    // Has the kernel proposed a new latency?
    if (const auto proposed_latency = context.propose_latency; proposed_latency/* && *proposed_latency != _latency*/) {
        // Notify controller and sit on the pending latency.
        _pqueue.push(Private_message{.type = Message_type::latency_changed});
        _pending_latency.store(*proposed_latency, std::memory_order_release);
    }

    return noErr;
}

// MARK: - create_view 

auto Auv2_effect::create_view() -> void*
{
    return _view->create_view();
}

auto Auv2_effect::_retain_presets() const -> void
{
    for (auto i = size_t{0}; i < Preset_list::num_presets; ++i) {
        const auto name = Preset_list::names[i];
        const auto str = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
        _au_presets[i] = {
            .presetNumber = static_cast<SInt32>(i),
            .presetName = str
        };
    }
}

auto Auv2_effect::_release_presets() const -> void
{
    for (auto& preset : _au_presets) {
        if (preset.presetName) {
            CFRelease(preset.presetName);
            preset.presetName = nullptr;
        }
    }
}

// MARK: - entry

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Auv2_effect);

} // namespace tiny