#pragma once

#include <array>
#include <memory>
#include <ranges>

#include <AudioUnitSDK/AUBase.h>
//#include <AudioUnitSDK/AUEffectBase.h>

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

    OSStatus GetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement, AudioUnitParameterValue& outValue) override
    {
        return Super::GetParameter(inID, inScope, inElement, outValue);
    }

    OSStatus SetParameter(AudioUnitParameterID inID, AudioUnitScope inScope, AudioUnitElement inElement, AudioUnitParameterValue inValue, UInt32 inBufferOffsetInFrames) override {
        //
        const auto& param = _specs[inID];

        _queue.push({
            .offset = static_cast<int32_t>(inBufferOffsetInFrames),
            .event = tiny::Set_param{.id = inID, .value = tiny::params::host_to_plain_space(inValue, param)}
        });
        return Super::SetParameter(inID, inScope, inElement, inValue, inBufferOffsetInFrames);
    }

    // MARK: - process

    OSStatus Render(AudioUnitRenderActionFlags& ioActionFlags, const AudioTimeStamp& inTimeStamp, UInt32 nFrames) override
    {
        // Pull inputs
        for (auto i = decltype(num_inputs){}; i < num_inputs; ++i) {
            Input(i).PullInput(ioActionFlags, inTimeStamp, i, nFrames);
        }

        auto check_events_full = [this]() {
            if (_events.size() == _events.capacity()) {
                // _events vector is full!
                std::cout << "Events vector full!\n";
            };
        };

        _events.clear(); // Start with no events.

        // Push received parameter events
        auto param_event = Tagged_event{};
        while (_queue.pop(param_event)) {
            check_events_full();
            _events.push_back(param_event);
        }

        // Push scheduled events
        auto& events = GetParamEventList(); // Studio One only?

        for (auto& event : events) {
            const auto& param = _specs[event.parameter]; // Get spec.
            switch (event.eventType) {
                case kParameterEvent_Immediate: {
                    const auto offset = event.eventValues.immediate.bufferOffset;
                    const auto value = event.eventValues.immediate.value;
                    check_events_full();
                    _events.push_back({
                        .offset = static_cast<int32_t>(offset),
                        .event = tiny::Set_param{
                            .id = event.parameter,
                            .value = tiny::params::host_to_plain_space(value, param)
                        }
                    });
                    break;
                }
                // It seems no hosts actually do this.
                case kParameterEvent_Ramped: {
                    const auto offset = event.eventValues.ramp.startBufferOffset;
                    const auto duration = event.eventValues.ramp.durationInFrames;
                    const auto initial = event.eventValues.ramp.startValue;
                    const auto target = event.eventValues.ramp.endValue;

                    if (offset + duration > nFrames) {
                        // Ramp is not wholly contained in buffer!
                        assert(false);
                    }
                    else {
                        // Interpret ramp start as Set_param?
                        check_events_full();
                        _events.push_back({
                            .offset = offset,
                            .event = tiny::Set_param{
                                .id = event.parameter,
                                .value = initial
                            }
                        });
                        check_events_full();
                        _events.push_back({
                            .offset = offset,
                            .event = tiny::Ramp_param{
                                .id = event.parameter,
                                .target = target,
                                .dur_samples = static_cast<int32_t>(duration)
                            }
                        });
                    }
                    break;
                }
            }
        }

        // sort events.
        std::ranges::sort(_events, [](const auto& a, const auto& b) {
            if (a.offset != b.offset) {
                return a.offset < b.offset;
            }
            // Place Set_param events before Ramp_param events.
            return std::holds_alternative<tiny::Set_param>(a.event) && std::holds_alternative<tiny::Ramp_param>(b.event);
        });

        // Now we have the events organized how we want.
        const auto event_count = _events.size();
        auto event_index = size_t{};
        const auto* event = event_count > 0 ? &_events[event_index] : nullptr;

        auto next_event = [&]() {
            ++event_index;
            if (event_index >= event_count) {
                event = nullptr;
            }
            else {
                event = &_events[event_index];
            }
        };

        auto do_process = [this](size_t num_frames, size_t offset) {
            // Assign buffer ptrs.
            _ibuffers[0] = static_cast<const float*>(Input(0).GetBufferList().mBuffers[0].mData) + offset;
            _ibuffers[1] = static_cast<const float*>(Input(0).GetBufferList().mBuffers[1].mData) + offset;

            if constexpr (tiny::Plug_info::wants_sidechain) {
                _ibuffers[2] = static_cast<const float*>(Input(1).GetBufferList().mBuffers[0].mData) + offset;
                _ibuffers[3] = static_cast<const float*>(Input(1).GetBufferList().mBuffers[1].mData) + offset;
            }

            _obuffers[0] = static_cast<float*>(Output(0).GetBufferList().mBuffers[0].mData) + offset;
            _obuffers[1] = static_cast<float*>(Output(0).GetBufferList().mBuffers[1].mData) + offset;

            // Process kernel.
            auto context = tiny::Dsp_context{
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

    auto create_view() -> void*;

protected:

    // Override to return your custom element
    std::unique_ptr<ausdk::AUElement> CreateElement(AudioUnitScope scope, AudioUnitElement element) override
    {
        if (scope == kAudioUnitScope_Global) {
            return std::make_unique<tiny::auv2::MyScheduledAUElement>(*this);
        }
        // Fall back to default behavior for other scopes (input/output)
        return Super::CreateElement(scope, element);
    }

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

    struct Tagged_event {
        int32_t offset{std::numeric_limits<int32_t>::max()};
        tiny::Event event{};
    };

    // SetParameter -> Render
    lark::Lock_free_queue<Tagged_event, 256, lark::On_full_behavior::overwrite> _queue{}; // TODO: - Use a heuristic.

    // Render
    std::vector<Tagged_event> _events{}; // Some fixed size thing.

    std::unique_ptr<tiny::Dsp_kernel> _kernel = std::make_unique<tiny::Dsp_kernel>();

    //
    static constexpr bool ParameterEventListSortPredicate(const AudioUnitParameterEvent& ev1, const AudioUnitParameterEvent& ev2) noexcept
    {
        constexpr auto bufferOffset = [](const AudioUnitParameterEvent& event) {
            // ramp.startBufferOffset is signed
            return (event.eventType == kParameterEvent_Immediate)
                    ? static_cast<SInt32>(event.eventValues.immediate.bufferOffset) // NOLINT union
                    : event.eventValues.ramp.startBufferOffset;                     // NOLINT union
        };

        return bufferOffset(ev1) < bufferOffset(ev2);
    }

};