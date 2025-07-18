#pragma once

#include <array>

#include <AudioUnitSDK/AUEffectBase.h>

#include "plug_info.h"
#include "user_plug.h"
#include "platform/platform_view.h"

#include "auv2_adapters.h"

class Auv2_effect : public ausdk::AUEffectBase {
public:

    using Super = ausdk::AUEffectBase;
    Auv2_effect(AudioUnit component) : Super(component)
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
            Globals()->SetParameter(utils::to_underlying(param.id), param.def_val);
        }

        const auto num_inputs = Plug_info::wants_sidechain ? 2 : 1;
        SetNumberOfElements(kAudioUnitScope_Input, num_inputs);
        Inputs().SetNumberOfElements(num_inputs);
        for (size_t i = 0; i < num_inputs; ++i) {
            const auto is_main = (i == 0);
            const auto* input_name = is_main ? "Input" : "Sidechain";
            const auto str = CFStringCreateWithCString(kCFAllocatorDefault, input_name, kCFStringEncodingUTF8);
            Inputs().GetElement(i)->SetName(str);
        }

        SetNumberOfElements(kAudioUnitScope_Output, 1);
        Outputs().SetNumberOfElements(1);
        const auto str = CFStringCreateWithCString(kCFAllocatorDefault, "Output", kCFStringEncodingUTF8);
        Outputs().GetElement(0)->SetName(str);
    }

    //~Auv2_effect() override {}

    void PostConstructor() override
    {
    }


    OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable) override
    {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

        switch (inID) {
            case kAudioUnitProperty_CocoaUI: {
                outDataSize = sizeof(AudioUnitCocoaViewInfo);
                outWritable = false;
                return noErr;
            }
            // case kAudioUnitProperty_ParameterStringFromValue: {
            //     outDataSize = sizeof(AudioUnitParameterStringFromValue);
            //     outWritable = false;
            //     return noErr;
            // }
            case kAudioUnitProperty_UserPlugin: {
                outDataSize = sizeof(void*);
                outWritable = false;
                return noErr;
            }
            default: break;
        }

        return Super::GetPropertyInfo(inID, inScope, inElement, outDataSize, outWritable);
    }

    OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, void* outData) override
    {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

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
            // case kAudioUnitProperty_ParameterStringFromValue: {
            //     auto* data = static_cast<AudioUnitParameterStringFromValue*>(outData);
            //     const auto id = data->inParamID;
            //     const auto value = *data->inValue;
            //     const auto& param = _specs[id];
            //     const auto str = tiny::Param_model::format_string(value, param, _uivalues, false);
            //     data->outString = CFStringCreateWithCString(kCFAllocatorDefault, str.c_str(), kCFStringEncodingUTF8);
            //     return noErr;
            // }
            case kAudioUnitProperty_UserPlugin: {
                ((void**)outData)[0] = (void*)this;
                return noErr;
            }
            default: break;
        }

        return Super::GetProperty(inID, inScope, inElement, outData);
    }

    OSStatus GetParameterList(AudioUnitScope inScope, AudioUnitParameterID* outParameterList, UInt32& outNumParameters) override
    {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

        // This is so we can determine the presentation order of the parameters by the host.
        using namespace tiny;
        outNumParameters = Param_model::num_params; // Do this first.
        if (!outParameterList) return noErr;
        std::ranges::copy(_ids, outParameterList);

        return noErr;
    }

    OSStatus GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID, AudioUnitParameterInfo& outParameterInfo) override
    {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

        using namespace tiny;

        // 
        auto map_units = [](const Param_model::Spec& param) {
            using enum params::Units;
            switch (param.units) {
                case generic: return kAudioUnitParameterUnit_Generic;
                case boolean: return kAudioUnitParameterUnit_Boolean;
                case indexed: return kAudioUnitParameterUnit_Indexed;
                case percent: return kAudioUnitParameterUnit_Percent;
                case decibels: return kAudioUnitParameterUnit_Decibels;
                case hertz: return kAudioUnitParameterUnit_Hertz;
                default: return kAudioUnitParameterUnit_Generic;
            }
        };

        //
        auto resolve_flags = [](const Param_model::Spec& param, bool found_clump) {
            auto flags = AudioUnitParameterOptions{};

            flags |= (kAudioUnitParameterFlag_HasCFNameString | kAudioUnitParameterFlag_CFNameRelease);

            if (found_clump) {
                flags |= kAudioUnitParameterFlag_HasClump;
            }

            using enum params::Units;
            if (param.units == boolean || param.provides_labels()) {
                flags |= kAudioUnitParameterFlag_ValuesHaveStrings;
            }

            if (!param.discrete) {
                flags |= kAudioUnitParameterFlag_CanRamp;
            }

            if (param.units == hertz) {
                flags = SetAudioUnitParameterDisplayType(flags, kAudioUnitParameterFlag_DisplayLogarithmic); // Logic Pro not respecting?
            }

            if (!param.hidden) {
                flags |= (kAudioUnitParameterFlag_IsReadable | kAudioUnitParameterFlag_IsWritable);
            }

            return flags;
        };
        
        const auto& param = _specs[inParameterID];
        const auto* clump = tiny::auv2::find_clump_for_parameter(_clumps, utils::to_underlying(param.id));
        const auto found_clump = clump != nullptr;

        outParameterInfo = {
            .name = {},
            .unitName = {},
            .clumpID = found_clump ? clump->id : UInt32{},
            .cfNameString = CFStringCreateWithCString(kCFAllocatorDefault, param.name, kCFStringEncodingUTF8),
            .unit = map_units(param),
            .minValue = static_cast<float>(param.min_val),
            .maxValue = static_cast<float>(param.max_val),
            .defaultValue = static_cast<float>(param.def_val),
            .flags = resolve_flags(param, found_clump)
        };

        return noErr;
    }

    OSStatus GetParameterValueStrings(AudioUnitScope inScope, AudioUnitParameterID inParameterID, CFArrayRef* outStrings) override
    {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;
        if (!outStrings) return noErr; // ?

        using namespace tiny;
        const auto& param = _specs[inParameterID];

        auto array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);

        using enum params::Units;
        if (param.units == boolean) {
            for (const char* label : {"Off", "On"}) {
                const auto str = CFStringCreateWithCString(kCFAllocatorDefault, label, kCFStringEncodingUTF8);
                CFArrayAppendValue(array, str);
                CFRelease(str);
            }
        }
        else if (param.units == indexed) {
            for (const char* label : param.labels) {
                if (!label) continue;
                const auto str = CFStringCreateWithCString(kCFAllocatorDefault, label, kCFStringEncodingUTF8);
                CFArrayAppendValue(array, str);
                CFRelease(str);
            }
        }

        *outStrings = array;
        return noErr;
    }

    OSStatus CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 /*inDesiredNameLength*/, CFStringRef* outClumpName) override
    {
        if (inScope != kAudioUnitScope_Global) return kAudioUnitErr_InvalidScope;

        if (const auto* clump = tiny::auv2::find_clump(_clumps, inClumpID)) {
            const auto& name = clump->name;
            *outClumpName = CFStringCreateWithCString(0, name.c_str(), kCFStringEncodingUTF8);
            return noErr;
        }

        return kAudioUnitErr_InvalidProperty;
    }

    auto create_view() -> void*
    {
        platform_view = Platform_views::make_autoreleasing(_delegate);
        return platform_view->native_handle();
    }

private:

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    std::unique_ptr<Platform_view> platform_view{nullptr};

    std::vector<uint32_t> _ids{}; // So we can send to host the presentation order.
    std::vector<tiny::Param_model::Spec> _specs{};
    tiny::auv2::Clump_map _clumps{};
    //tiny::Param_model::Param_values _uivalues{};

};