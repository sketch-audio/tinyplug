#pragma once

#include <array>
#include <memory>
#include <ranges>

#include <AudioUnitSDK/AUBase.h>
#include <AudioToolbox/AudioToolbox.h>

#include "tinyplug/tinyplug.h"

#include "plug_processor.h"
#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"
#include "plug_info.h"

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

    UInt32 SupportedNumChannels(const AUChannelInfo** outInfo) override
    {
        if (cinfo.empty()) {
            cinfo.push_back({2, 2});
            if constexpr (Plug_info::can_process_mono) {
                cinfo.push_back({1, 1});
            }
        }
        if (!outInfo) return (UInt32)cinfo.size();

        *outInfo = cinfo.data();
        return (UInt32)cinfo.size();
    }

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

    OSStatus SaveState(CFPropertyListRef* outData) override;
    OSStatus RestoreState(CFPropertyListRef plist) override;

    // latency
    Float64 GetLatency() override
    {
        // Did we get here from a latency change notification?
        const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (pending_latency) {
            _accepted_latency.store(*pending_latency, std::memory_order_release); // The kernel should manifest on the next process.
            _latency = *pending_latency;
        }

        const auto format = GetStreamFormat(kAudioUnitScope_Output, 0);
        const auto sample_rate = format.mSampleRate;
        assert(sample_rate > 0 && "Invalid sample rate.");
        const auto latency_samps = static_cast<double>(_latency);
        return latency_samps / sample_rate;
    }

    Float64 GetTailTime() override
    {
        const auto tail = _processor->tail_samps();
        const auto inf_tail = std::numeric_limits<uint32_t>::max();
        const auto format = GetStreamFormat(kAudioUnitScope_Output, 0);
        const auto sample_rate = format.mSampleRate;
        assert(sample_rate > 0 && "Invalid sample rate.");
        return tail != inf_tail ? tail / sample_rate : std::numeric_limits<double>::infinity();
    }
    bool SupportsTail() override { return true; }

    OSStatus Render(AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& inTimeStamp, UInt32 nFrames) override;

    auto create_view() -> void*;

private:

    double _sr{48000};

    std::vector<AUChannelInfo> cinfo{};

    std::shared_ptr<Plug_editor> _editor = std::make_shared<Plug_editor>();

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{Plug_info::wants_sidechain ? 2 : 0};
    static constexpr auto max_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};

    std::array<float, num_meters> _meters{}; // For processor to write.
    std::array<double, num_meters> _last_meters{};

    User_params _param_infos{};
    Clump_map _clumps{};

    // 
    static constexpr auto to_processor_size = 2 * num_params + 1;
    using To_processor_queue = Lock_free_queue<Tagged_event, to_processor_size>; // mpsc?

    static constexpr auto to_editor_size = num_params + num_meters + 1;
    using To_editor_queue = Overwrite_queue<Ui_event, to_editor_size>;

    // Right now just for latency.
    using Private_queue = Lock_free_queue<Private_message, 16>;

    To_processor_queue _to_processor{};
    To_editor_queue _to_editor{};
    Private_queue _pqueue{};

    // Render
    std::vector<Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<Plug_processor> _processor = std::make_unique<Plug_processor>();
    uint32_t _latency{_processor->latency_samps()};

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // AUv2 view adapter.
    std::unique_ptr<Auv2_view> _view = std::make_unique<Auv2_view>(Ui_receiver{
        .get_knob_value = [this](auto id) {
            const auto& param = _param_infos.param_for(id);
            const auto host = Globals()->GetParameter(id);
            const auto knob = Value_conv::host_to_knob(host, param.semantics);
            return knob;
        },
        .pop_event = [this](auto& event) {
            return _to_editor.pop(event);
        },
        .action_handler = [this](auto& action) {
            std::visit(Inline_visitor{
                [&](const Action_start& a) {
                    auto event = AudioUnitEvent{};
                    event.mEventType = kAudioUnitEvent_BeginParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = a.address;
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(NULL, NULL, &event);
                },
                [&](const Set_param& a) {
                    // Notify host
                    const auto& param = _param_infos.param_for(a.address);
                    const auto plain_value = Value_conv::knob_to_plain(a.value, param.semantics);
                    const auto host_value = Value_conv::knob_to_host(a.value, param.semantics);

                    Globals()->SetParameter(a.address, static_cast<float>(host_value));
                    auto event = AudioUnitEvent{};
                    event.mEventType = kAudioUnitEvent_ParameterValueChange;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = a.address;
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(NULL, NULL, &event);
                    [[maybe_unused]] const auto success = _to_processor.push({Set_param{a.address, plain_value}, 0});
                    assert(success && "Push to processor queue failed! Increase queue size.");
                },
                [&](const Action_end& a) {
                    auto event = AudioUnitEvent{};
                    event.mEventType = kAudioUnitEvent_EndParameterChangeGesture;
                    event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                    event.mArgument.mParameter.mParameterID = a.address;
                    event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                    event.mArgument.mParameter.mElement = 0;
                    AUEventListenerNotify(NULL, NULL, &event);
                },
            }, action);
        }
    }, _editor, Main_executor{
        .on_main = [this]() {
            auto message = Private_message{};
            while (_pqueue.pop(message)) {
                switch (message.type) {
                    case Message_type::latency_changed: {
                        PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
                        break;
                    }
                }
            }
        }
    });

};

} // namespace tiny