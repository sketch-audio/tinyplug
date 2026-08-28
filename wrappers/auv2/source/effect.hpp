#pragma once

#include <array>
#include <memory>
#include <optional>
#include <ranges>

#include <AudioUnitSDK/AUBase.h>
#include <AudioToolbox/AudioToolbox.h>

#include "tinyplug/tinyplug.hpp"
#include "tinyplug/change_list.hpp"

#include "processor.hpp"
#include <tinyplug/meter_mailbox.hpp>
#include <tinyplug/meter_publisher.hpp>

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"
#include "plug_info.hpp"

#include "adapters.hpp"
#include "relay.hpp"
#include "view.hpp"

#include "preset_list.hpp" // Generated.

#include <tiny_dsp/host_bypass.hpp>

namespace tiny::auv2 {

class Effect : public ausdk::AUBase {
public:

    static constexpr auto num_inputs = uint32_t{Plug_info::wants_sidechain ? 2 : 1};
    static constexpr auto num_outputs = uint32_t{1};

    using Super = ausdk::AUBase;
    Effect(AudioUnit component);
    ~Effect();

    OSStatus Initialize() override;
    void Cleanup() override;

    // AU's discontinuity hook — "clear any render state".
    OSStatus Reset(AudioUnitScope inScope, AudioUnitElement inElement) override;

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
        const auto format = GetStreamFormat(kAudioUnitScope_Output, 0);
        const auto sample_rate = format.mSampleRate;
        if (sample_rate <= 0) return 0.;

        // Did we get here from a latency change notification?
        const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (pending_latency) {
            _accepted_latency.store(*pending_latency, std::memory_order_release); // The kernel should manifest on the next process.
            _latency = *pending_latency;
        }

        const auto latency_samps = static_cast<double>(_latency);
        return latency_samps / sample_rate;
    }

    Float64 GetTailTime() override
    {
        const auto format = GetStreamFormat(kAudioUnitScope_Output, 0);
        const auto sample_rate = format.mSampleRate;
        if (sample_rate <= 0) return 0.;

        const auto tail = _processor->tail_samps();
        const auto inf_tail = std::numeric_limits<uint32_t>::max();
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
        },
        .save_model = [this]() {
            const auto knob = _snapshot_knob_params();
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(knob.begin(), knob.end()),
                .editor_state = _editor ? _editor->save_state() : State_map{}
            };
        },
    }};

    std::vector<AUChannelInfo> cinfo{};

    std::optional<plugin::Editor> _editor{};
    Task_manager _tasks{};

    // Framework-owned editor window-size cache (Effect lifetime, survives view
    // recreation). Primed from persisted state in RestoreState; read by the view's
    // initial_size provider; updated on every editor-initiated resize.
    std::optional<Rect_size> _last_size{};

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    // Undo history and action queue live on the Effect (plug-in lifetime), not in the
    // view, so the editor's Edit_context (built once at construction) stays valid across
    // window open/close and host preset loads are captured with the window closed.
    // Declared before `_view` so its Deps can borrow pointers to them.
    Undo_history _undo_history{};
    Action_queue _actions{};

    // Snapshot all current param values in knob space (mirrors the receiver's get_param).
    auto _snapshot_knob_params() -> std::array<double, num_params>
    {
        auto out = std::array<double, num_params>{};
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            const auto host = Globals()->GetParameter(i);
            out[i] = params::Value_helper::host_to_knob(host, param.semantics);
        }
        return out;
    }

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{Plug_info::wants_sidechain ? 2 : 0};
    static constexpr auto max_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};

    meters::Publisher<User_meters> _meters{}; // Owns the scratch the DSP writes.

    Clump_map _clumps{};

    static constexpr auto queue_size = []() {
        const auto state = 4 * num_params;
        const auto automation = 64 * std::bit_width(num_params); // We expect number of automated parameters to be small but we need to be able to handle a lot of flux.
        return state + automation + 1;
    }();

    // Both queue and change list drain into events vector. 
    // We may want to revisit to reduce memory usage for high parameter counts.
    static constexpr auto events_size = num_params + queue_size + 1;

    // 
    using To_processor_queue = Lock_free_queue<process::Tagged_event, queue_size, Queue_concurrency::mpsc>; // I believe SetParameter can happen from a variety of threads.


    Change_list<process::Event::Set> _changes{}; // Plain space, to the audio thread.
    To_processor_queue _to_processor{};

    meters::Mailbox<User_meters> _mailbox{};

    // Render
    std::vector<process::Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<process::Processor> _processor = std::make_unique<process::Processor>();

    // Latency
    uint32_t _latency{};
    std::atomic<uint32_t> _reported_latency{}; // Don't feedback latency changes.

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // Relays latency proposal to main thread for property change notification.
    std::optional<Relay> _relay{};

    // Render mode (offline/bounce). Set off the audio thread via the
    // kAudioUnitProperty_OfflineRender property, read on the audio thread.
    std::atomic<bool> _offline{false};

    Host_bypass _bypass{};

    // Resync mechanism.
    std::atomic<bool> _needs_clear{false}; // Set by Reset(), consumed at the top of Render().
    std::atomic<bool> _needs_resync{false}; // Queue-overflow recovery only. See Render().
    std::atomic<uint32_t> _bypass_epoch{};
    uint32_t _seen_epoch{}; // Render-thread only.
    bool _was_skipped{}; // Render-thread only. Detects the can_skip -> processing edge.
    std::optional<process::Render_mode> _last_render_mode{}; // Render-thread only. Detects the realtime <-> offline edge.

#if TINY_HAS_WORKER
    // Worker channel.
    using Worker_from_proc_q = Lock_free_queue<typename User_worker::Model::From_processor, User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_worker::Model::From_editor,    User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_worker::Model::To_processor,   User_worker::Model::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_worker::Model::To_editor,     User_worker::Model::outbound_capacity>;

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
    std::unique_ptr<View> _view = std::make_unique<View>(View::Deps{
        .editor = &(*_editor),
        .receiver = {
            .get_param = [this](auto id) {
                using namespace params;
                const auto& param = User_params::param_spec(id);
                const auto host = Globals()->GetParameter(id);
                const auto knob = Value_helper::host_to_knob(host, param.semantics);
                return knob;
            },
            .read_meters = [this](std::span<meters::Sample> out) {
                _mailbox.read(out);
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
                        using namespace params;

                        // Notify host
                        const auto& param = User_params::param_spec(a.address);
                        const auto plain_value = Value_helper::knob_to_plain(a.value, param.semantics);
                        const auto host_value = Value_helper::knob_to_host(a.value, param.semantics);

                        Globals()->SetParameter(a.address, static_cast<float>(host_value));
                        auto event = AudioUnitEvent{};
                        event.mEventType = kAudioUnitEvent_ParameterValueChange;
                        event.mArgument.mParameter.mAudioUnit = GetComponentInstance();
                        event.mArgument.mParameter.mParameterID = a.address;
                        event.mArgument.mParameter.mScope = kAudioUnitScope_Global;
                        event.mArgument.mParameter.mElement = 0;
                        AUEventListenerNotify(NULL, NULL, &event);
                        _changes.push(process::Event::Set{a.address, plain_value});
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
        .undo_history = &_undo_history,
        .actions = &_actions,
        .initial_size = [this]() { return _last_size.value_or(plugin::Editor::preferred_size()); },
        .on_resized = [this](uint32_t w, uint32_t h) {
            _last_size = Rect_size{static_cast<int32_t>(w), static_cast<int32_t>(h)};
        },
#if TINY_HAS_WORKER
        .drain_worker_to_editor = [this]() { this->_drain_worker_to_editor(); }
#endif
    });

#if TINY_HAS_WORKER
    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif
};

} // namespace tiny::auv2