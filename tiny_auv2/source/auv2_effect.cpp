#include "auv2_effect.h"

#include <AudioUnitSDK/ComponentBase.h>

Auv2_effect::Auv2_effect(AudioUnit component) : Super(component, num_inputs, num_outputs)
{
    CreateElements();

    using namespace tiny;
    const auto tree = Param_model::build_tree();
    _ids = tiny::auv2::flatten_tree_ids(tree); // Ids in presentation order!
    _specs = params::flatten_tree(tree);
    params::sort_param_specs_by_id(_specs);

    _clumps = tiny::auv2::build_clump_map(tree);

    // Set up parameters.
    Globals()->UseIndexedParameters(tiny::Param_model::num_params);
    for (const auto& param : _specs) {
        const auto def_val = params::get_host_default(param);
        Globals()->SetParameter(utils::to_underlying(param.id), def_val);
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
            const auto& param = _specs[id];
            const auto str = Param_model::format_string(*data->inValue, param, _uivalues);
            data->outString = CFStringCreateWithCString(kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);

            return noErr;
        }
        case kAudioUnitProperty_ParameterValueFromString: {
            auto* data = static_cast<AudioUnitParameterValueFromString*>(outData);

            using namespace tiny;
            const auto id = data->inParamID;
            const auto& param = _specs[id];

            const auto str = tiny::auv2::cf_to_std(data->inString);

            if (const auto plain = Param_model::format_value(str, param)) {
                data->outValue = params::plain_to_host_space(*plain, param);
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
    outNumParameters = Param_model::num_params; // Do this first.
    if (!outParameterList) return noErr;
    std::ranges::copy(_ids, outParameterList);

    return noErr;
}

// MARK: - GetParameterInfo

OSStatus Auv2_effect::GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID, AudioUnitParameterInfo& outParameterInfo)
{
    if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

    using namespace tiny;
    using namespace params;

    auto resolve_flags = [](const Param_model::Spec& param, bool found_clump) {
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

    const auto& param = _specs[inParameterID];
    const auto* clump = tiny::auv2::find_clump_for_parameter(_clumps, utils::to_underlying(param.id));
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
    using namespace params;
    
    const auto& param = _specs[inParameterID];

    
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

// MARK: - create_view 

void* Auv2_effect::create_view()
{
    platform_view = Platform_views::make_autoreleasing(_delegate);
    return platform_view->native_handle();
}

// MARK: - entry

AUSDK_COMPONENT_ENTRY(ausdk::AUBaseFactory, Auv2_effect);