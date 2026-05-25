#pragma once

#include <array>
#include <memory>
#include <ranges>

#include <AudioUnitSDK/AUBase.h>
#include <AudioToolbox/AudioToolbox.h>

#include "tinyplug/tinyplug.h"
#include "tinyplug/change_list.hpp"

#include "plug_processor.h"
#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"
#include "plug_info.h"

#include "auv2_adapters.h"
#include "auv2_view.h"

#include "auv2_preset_list.h" // Generated.

#include "dsp/host_bypass.hpp"

namespace tiny {

class Auv2_effect : public ausdk::AUBase {
public:

    static constexpr auto num_inputs = uint32_t{Plug_info::wants_sidechain ? 2 : 1};
    static constexpr auto num_outputs = uint32_t{1};

    using Super = ausdk::AUBase;
    Auv2_effect(AudioUnit component);
    ~Auv2_effect();

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
    OSStatus SetProperty(AudioUnitPropertyID inID, AudioUnitScope inScope, AudioUnitElement inElement, const void* inData, UInt32 inDataSize) override;

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

    OSStatus GetPresets(CFArrayRef* outData) const override;
    OSStatus NewFactoryPresetSet(const AUPreset& inNewFactoryPreset) override;

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

    auto _update_state(const Maybe_values<double>& knob_values, const State_map& editor_state) -> void;

    double _sr{48000};

    mutable std::array<AUPreset, Preset_list::num_presets> _au_presets{}; // Strings will need released.
    //mutable CFArrayRef _presets_array{nullptr};
    auto _retain_presets() const -> void;
    auto _release_presets() const -> void;

    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params,
            };
        }
    }};

    std::vector<AUChannelInfo> cinfo{};

    std::optional<Plug_editor> _editor{};
    Task_manager _tasks{};

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

    Clump_map _clumps{};

    static constexpr auto queue_size = []() {
        const auto state = 4 * num_params;
        const auto automation = 64 * std::bit_width(num_params); // We expect number of automated parameters to be small but we need to be able to handle a lot of flux.
        return state + automation + 1;
    }();

    // 
    //static constexpr auto to_processor_size = 4 * num_params + 1;
    using To_processor_queue = Lock_free_queue<Tagged_event, queue_size, Queue_concurrency::mpsc>; // I believe SetParameter can happen from a variety of threads.

    static constexpr auto meter_size = 25 * num_meters + 1; // Approx number of 32 sample buffers between UI updates at 60fps (25).
    using Meter_queue = Lock_free_queue<Set_meter, meter_size>;
    using Private_queue = Lock_free_queue<Private_message, 24>; // Right now just to send latency change notifications.

    Change_list _changes{}; // State, UI updates (not for SetParameter).
    To_processor_queue _to_processor{};

    Meter_queue _meter_queue{};
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

    Host_bypass _bypass{};

#if TINY_HAS_WORKER
    // Worker channel.
    using Worker_from_proc_q = Lock_free_queue<typename User_worker::From_processor, User_worker::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_worker::From_editor,    User_worker::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_worker::To_processor,   User_worker::reply_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_worker::To_editor,     User_worker::reply_capacity>;

    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    Worker_to_proc_q   _worker_to_proc{};
    Worker_to_edit_q   _worker_to_edit{};

    User_worker _worker{
        Worker_replies{
            [this](const auto& m) { return _worker_to_proc.push(m); },
            [this](const auto& m) { return _worker_to_edit.push(m); }
        },
        _tasks.actor()
    };
#endif

    auto _drain_worker_to_processor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_processor(*_processor, _worker_to_proc);
#endif
    }

    auto _drain_worker_to_editor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
    }

    // AUv2 view adapter.
    std::unique_ptr<Auv2_view> _view = std::make_unique<Auv2_view>(Auv2_view::Deps{
        .editor = &(*_editor),
        .executor = {[this]() {
            auto message = Private_message{};
            while (_pqueue.pop(message)) {
                switch (message.type) {
                    case Message_type::latency_changed: {
                        PropertyChanged(kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0);
                        break;
                    }
                    default:
                        break;
                }
            }
        }},
        .receiver = {
            .get_param = [this](auto id) {
                const auto& param = User_params::param_spec(id);
                const auto host = Globals()->GetParameter(id);
                const auto knob = Value_conv::host_to_knob(host, param.semantics);
                return knob;
            },
            .pop_meter = [this](auto& event) {
                return _meter_queue.pop(event);
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
                        const auto& param = User_params::param_spec(a.address);
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
                        _changes.push(Set_param{a.address, plain_value});
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
                    [](const auto&) {}
                }, action);
            }
        },
        .tasks = &_tasks,
#if TINY_HAS_WORKER
        .drain_worker_to_editor = [this]() { this->_drain_worker_to_editor(); }
#endif
    });

#if TINY_HAS_WORKER
    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif
};

} // namespace tiny