#pragma once

#include <array>
#include <memory>
#include <ranges>

#include <AudioUnitSDK/AUBase.h>
#include <AudioToolbox/AudioToolbox.h>

#include "tinyplug/tinyplug.h"

#include "plug_info.h"
#include "user/param_model.h"
#include "user/dsp_kernel.h"

#include "auv2_adapters.h"
#include "auv2_view.h"

namespace tiny {

class Auv2_effect : public ausdk::AUBase {
public:

    static constexpr auto num_inputs = uint32_t{Plug_info::wants_sidechain ? 2 : 1};
    static constexpr auto num_outputs = uint32_t{1};

    using Super = ausdk::AUBase;
    Auv2_effect(AudioUnit component);
    ~Auv2_effect() = default;

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

    double _sr{48000};

    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{2};
    static constexpr auto num_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_schannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    User_params _params{};
    Clump_map _clumps{};

    std::array<double, num_exports> _lexports{};

    // SetParameter -> Render
    // TODO: - Use a heuristic for size.
    using Event_queue = Lock_free_queue<Tagged_event, 256>; // mpsc?
    using To_ui_queue = Lock_free_queue<Ui_event, 256, Queue_concurrency::mpsc>;
    Event_queue _iqueue{};
    To_ui_queue _oqueue{};

    // Render
    std::vector<Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<Dsp_kernel> _kernel = std::make_unique<Dsp_kernel>();

    // AUv2 view adapter.
    std::unique_ptr<Auv2_view> _view = std::make_unique<Auv2_view>(Ui_receiver{
        .get_knob_value = [this](auto id) {
            const auto& params = _params.kernel_specs();
            const auto& param = params[id];
            const auto host = Globals()->GetParameter(id);
            const auto knob = Value_conv::host_to_knob(host, param.semantics);
            return knob;
        },
        .pop_event = [this](auto& event) { return _oqueue.pop(event); },
        .action_handler = [this](auto& action) {
            std::visit(Inline_visitor{
                [&](const Action_start& a) {
                    auto event = AudioUnitEvent{};
                    event.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = a.id;
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(NULL, NULL, &event);
                },
                [&](const Set_param& a) {
                    // Notify host
                    const auto& params = _params.kernel_specs();
                    const auto& param = params[a.id];
                    const auto plain_value = Value_conv::knob_to_plain(a.value, param.semantics);
                    const auto host_value = Value_conv::knob_to_host(a.value, param.semantics);

                    Globals()->SetParameter(a.id, host_value);
                    auto event = AudioUnitEvent{};
                    event.mEventType = kAudioUnitEvent_ParameterValueChange;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = a.id;
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(NULL, NULL, &event);
                    _iqueue.push({Set_param{a.id, plain_value}, 0});
                },
                [&](const Action_end& a) {
                    auto event = AudioUnitEvent{};
                    event.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = a.id;
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(NULL, NULL, &event);
                },
            }, action);
        }
    });

};

} // namespace tiny