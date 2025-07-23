#pragma once

#include <array>
#include <memory>
#include <ranges>

#include <AudioUnitSDK/AUBase.h>

#include "tinyplug/tinyplug.h"

#include "plug_info.h"
#include "platform/platform_view.h"
#include "user/param_model.h"
#include "user/dsp_kernel.h"

#include "auv2_adapters.h"

class Auv2_effect : public ausdk::AUBase {
public:

    static constexpr auto num_inputs = uint32_t{tiny::Plug_info::wants_sidechain ? 2 : 1};
    static constexpr auto num_outputs = uint32_t{1};

    using Super = ausdk::AUBase;
    Auv2_effect(AudioUnit component);
    //~Auv2_effect() override {}

    OSStatus Initialize() override;

    OSStatus GetPropertyInfo(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, UInt32& outDataSize, bool& outWritable) override;
    OSStatus GetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, void* outData) override;

    OSStatus GetParameterList(AudioUnitScope inScope, AudioUnitParameterID* outParameterList, UInt32& outNumParameters) override;
    OSStatus GetParameterInfo(AudioUnitScope inScope, AudioUnitParameterID inParameterID, AudioUnitParameterInfo& outParameterInfo) override;
    OSStatus GetParameterValueStrings(AudioUnitScope inScope, AudioUnitParameterID inParameterID, CFArrayRef* outStrings) override;
    OSStatus CopyClumpName(AudioUnitScope inScope, UInt32 inClumpID, UInt32 inDesiredNameLength, CFStringRef* outClumpName) override;

    //
    bool CanScheduleParameters() const override { return true; }
    bool StreamFormatWritable(AudioUnitScope /*scope*/, AudioUnitElement /*element*/) override { return !IsInitialized(); }

    OSStatus GetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement, AudioUnitParameterValue& outValue) override;
    OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement, AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames) override;
    OSStatus ScheduleParameter(const AudioUnitParameterEvent* inParameterEvent, UInt32 inNumEvents) override;

    OSStatus Render(AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& inTimeStamp, UInt32 nFrames) override;

    auto create_view() -> void*;

private:

    static constexpr auto num_ichannels = size_t{2 + (tiny::Plug_info::wants_sidechain ? 2 : 0)};
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<float*, num_ochannels> _obuffers{};

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    std::unique_ptr<Platform_view> platform_view{nullptr};

    std::vector<uint32_t> _ids{}; // So we can send to host the presentation order.
    std::vector<tiny::Param_model::Spec> _specs{};
    tiny::auv2::Clump_map _clumps{};
    tiny::Param_model::Param_values _uivalues{};

    // SetParameter -> Render
    lark::Lock_free_queue<tiny::Tagged_event, 256, lark::On_full_behavior::overwrite> _queue{}; // TODO: - Use a heuristic.

    // Render
    std::vector<tiny::Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<tiny::Dsp_kernel> _kernel = std::make_unique<tiny::Dsp_kernel>();

};