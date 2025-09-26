#pragma once

#include <atomic>
#include <array>

#import <AudioToolbox/AudioToolbox.h>
#import <algorithm>
#import <vector>
#import <span>

#include "plug_processor.h"
#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_info.h"

/*
 DSPKernel
 As a non-ObjC class, this is safe to use from render thread.
 */
class DSPKernel {
public:

    ~DSPKernel() {
        // ...
    }

    void initialize(int inputChannelCount, int outputChannelCount, double inSampleRate) {
        mSampleRate = inSampleRate;
        mInputChannelCount = inputChannelCount;
        mOutputChannelCount = outputChannelCount;
        _processor->reset(mSampleRate);
        _latency = _processor->latency_samps();
    }
    
    void deInitialize() {
    }
    
    // MARK: - Bypass
    bool isBypassed() {
        return mBypassed;
    }
    
    void setBypass(bool shouldBypass) {
        mBypassed = shouldBypass;
    }
    
    // MARK: - Parameter Getter / Setter
    void setParameter(AUParameterAddress address, AUValue value) {
        if (address >= num_params) return;
        const auto addr = static_cast<uint32_t>(address);
        const auto& spec = _param_infos.param_for(addr);
        const auto plain = tiny::Value_conv::host_to_plain(value, spec.semantics);
        [[maybe_unused]] const auto success = _param_queue.push(tiny::Set_param{
            .address = addr,
            .value = plain
        });
        assert(success && "Param queue push failed. Increase queue size!");
        _hostvalues[address].store(value, std::memory_order_release);
    }
    
    AUValue getParameter(AUParameterAddress address) {
        if (address >= num_params) return 0;
        return _hostvalues[address].load(std::memory_order_acquire);
    }
    
    // MARK: - Max Frames
    AUAudioFrameCount maximumFramesToRender() const {
        return mMaxFramesToRender;
    }
    
    void setMaximumFramesToRender(const AUAudioFrameCount &maxFrames) {
        mMaxFramesToRender = maxFrames;
    }
    
    // MARK: - Musical Context
    void setMusicalContextBlock(AUHostMusicalContextBlock contextBlock) {
        mMusicalContextBlock = contextBlock;
    }
    
    void setTransportStateBlock(AUHostTransportStateBlock transportStateBlock) {
        mTransportStateBlock = transportStateBlock;
    }
    
    /**
     MARK: - Internal Process
     
     This function does the core siginal processing.
     Do your custom DSP here.
     */
    void process(std::span<float const*> inputBuffers, std::span<float const*> sidechainBuffers, std::span<float *> outputBuffers, AUEventSampleTime bufferStartTime, AUAudioFrameCount frameCount) {
        /*
         Note: For an Audio Unit with 'n' input channels to 'n' output channels, remove the assert below and
         modify the check in [Galaxy_Brain_AUAudioUnit allocateRenderResourcesAndReturnError]
         */
        assert(inputBuffers.size() == outputBuffers.size());
        
        // Handle set_param events.
        auto event = tiny::Render_event{};
        while(_param_queue.pop(event)) {
            _processor->handle_event(event);
        }
        
        const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (accepted_latency) {
            _processor->handle_event(tiny::Accepted_latency{*accepted_latency});
            assert(_processor->latency_samps() == *accepted_latency && "Kernel must apply the accepted latency!");
        }
        
        if (mBypassed) {
            // Pass the samples through
            for (UInt32 channel = 0; channel < inputBuffers.size(); ++channel) {
                std::copy_n(inputBuffers[channel], frameCount, outputBuffers[channel]);
            }
            return;
        }
        
        auto context = tiny::Dsp_context{.meters = _meters, .propose_latency = {}};
        context.musical_context = resolve_musical_context(frameCount);
        
        assert(inputBuffers.size() == static_cast<size_t>(mInputChannelCount));
        assert(outputBuffers.size() == static_cast<size_t>(mOutputChannelCount));
        
        // Already spans with size set by process helper.
        context.ibuffers = inputBuffers;
        context.obuffers = outputBuffers;
        context.sbuffers = sidechainBuffers;
        context.num_frames = frameCount;
        
        _processor->process(context);
        
        // Send exports.
        for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
            if (context.meters[i] != _last_meters[i]) {
                const auto value = context.meters[i];
                _to_editor.push(tiny::Set_meter{.address = i, .value = value});
                _last_meters[i] = value;
            }
            _meters[i] = 0; // Reset for peak meters.
        }

        // Has the kernel proposed a new latency?
        if (const auto proposed_latency = context.propose_latency; proposed_latency && *proposed_latency != _latency) {
            // Audio unit is polling. Could possibly fix.
            _pending_latency.store(*proposed_latency, std::memory_order_release);
        }

//        const auto tail = _processor->tail_samps();
//        if (tail != _tail) {
//            // tail changed
//        }
    }
    
    // Called by the process helper on the audio thread so we can send events to kernel directly.
    void handleOneEvent(AUEventSampleTime now, AURenderEvent const *event) {
        switch (event->head.eventType) {
            case AURenderEventParameter: {
                const auto address = static_cast<uint32_t>(event->parameter.parameterAddress);
                const auto& spec = _param_infos.param_for(address);
                const auto plain = tiny::Value_conv::host_to_plain(event->parameter.value, spec.semantics);
                _processor->handle_event(tiny::Set_param{.address = address, .value = plain});
                _to_editor.push(tiny::Set_param{.address = address, .value = plain});
                break;
            }
            case AURenderEventParameterRamp: {
                const auto address = static_cast<uint32_t>(event->parameter.parameterAddress);
                const auto dur_samples = static_cast<int32_t>(event->parameter.rampDurationSampleFrames);
                const auto& spec = _param_infos.param_for(address);
                const auto plain = tiny::Value_conv::host_to_plain(event->parameter.value, spec.semantics);
                _processor->handle_event(tiny::Ramp_param{.address = address, .target = plain, .dur_samples = dur_samples});
                _to_editor.push(tiny::Set_param{.address = address, .value = plain});
                break;
            }
                
            default:
                break;
        }
    }
    
    void handleParameterEvent(AUEventSampleTime now, AUParameterEvent const& parameterEvent) {
        // Implement handling incoming Parameter events as needed
    }
    
    // tiny
    auto latency_secs() -> double 
    {
        // Did we get here from a latency change notification?
        const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (pending_latency) {
            _accepted_latency.store(*pending_latency, std::memory_order_release); // The kernel should manifest on the next process.
            _latency = *pending_latency;
        }
        
        // _latency in samples
        return _latency / mSampleRate;
    }
    
    auto tail_secs() -> double
    {
        return _processor->tail_samps() / mSampleRate;
    }
    
    auto pop_event(tiny::Ui_event& event) -> bool
    {
        return _to_editor.pop(event);
    }
    
    auto onHostUpdated(AUParameterAddress address, AUValue value) -> void
    {
        _to_editor.push(tiny::Set_param{.address = static_cast<uint32_t>(address), .value = value});
    }
    
private:
    
    // MARK: Member Variables
    AUHostMusicalContextBlock mMusicalContextBlock;
    AUHostTransportStateBlock mTransportStateBlock;
    
    double mSampleRate = 48000;
    int mInputChannelCount = 2;
    int mOutputChannelCount = 2;
    
    bool mBypassed = false;
    AUAudioFrameCount mMaxFramesToRender = 1024;
    
    using User_params = tiny::Param_infos<tiny::Param_model>;
    using User_meters = tiny::Meter_infos<tiny::Meter_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{2};
    static constexpr auto max_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};
    std::array<float, num_meters> _meters{};
    
    static constexpr auto param_queue_min_size = 2 * num_params + 1;
    using Param_queue = tiny::Lock_free_queue<tiny::Render_event, param_queue_min_size>;
    Param_queue _param_queue{};
    
    static constexpr auto to_editor_size = num_params + num_meters + 1;
    using To_editor_queue = tiny::Overwrite_queue<tiny::Ui_event, to_editor_size>;
    To_editor_queue _to_editor{};
    
    User_params _param_infos{}; // We have to be able to map the values to plain space.
    
    // Values in host space.
    using Host_value = std::atomic<float>;
    using Host_values = std::array<Host_value, num_params>;
    Host_values _hostvalues{_param_infos.make_host_defaults<Host_value>()};
    
    std::array<float, num_meters> _last_meters{};
    
    std::unique_ptr<tiny::Plug_processor> _processor = std::make_unique<tiny::Plug_processor>();
    uint32_t _latency{_processor->latency_samps()};

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};
    
    tiny::Musical_context _context{};
    
    // MARK: - Musical Context Helpers
    
    auto frames_to_beats(double frames, double tempo, double sr) -> double
    {
        const auto beats_per_frame = tempo / (60 * sr);
        return frames * beats_per_frame;
    }
    
    auto resolve_musical_context(uint32_t frame_count) -> tiny::Musical_context
    {
        if (mTransportStateBlock && mMusicalContextBlock) {
            // Buid the host context.
            auto flags = AUHostTransportStateFlags{};
            auto samplePos = double{};
            auto cycleStart = double{};
            auto cycleEnd = double{};
            mTransportStateBlock(&flags, &samplePos, &cycleStart, &cycleEnd);
            
            // Resolve flags
            const auto changed = (flags & AUHostTransportStateChanged) > 0; // If there is a discontinuity, for example.
            const auto moving = (flags & AUHostTransportStateMoving) > 0;
            const auto recording = (flags & AUHostTransportStateRecording) > 0;
            const auto cycling = (flags & AUHostTransportStateCycling) > 0;
            
            _context.transport_state.moving = moving;
            _context.transport_state.recording = recording;
            _context.transport_state.cycling = cycling;
            
            _context.cycle_start = cycleStart;
            _context.cycle_end = cycleEnd;

            //
            auto tempo = double{};
            auto timeSigNumer = double{};
            auto timeSigDenom = long{};
            auto beatPos = double{};
            
            mMusicalContextBlock(&tempo,
                                 &timeSigNumer,
                                 &timeSigDenom,
                                 &beatPos,
                                 nullptr,     // sampleOffsetToNextBeat
                                 nullptr);    // currentMeasureDownbeatPosition
            
            // Grab the time signature info.
            _context.time_sig.numer = timeSigNumer;
            _context.time_sig.denom = static_cast<int32_t>(timeSigDenom);
            
            // Resolve tempo and beat time.
            if (moving) {
                // At this time, mBeatPos holds the expected beat time for the current buffer.
                const auto isDiscontinuity = fabs(beatPos - _context.beat_pos) > 1e-3;
                
                if (changed || isDiscontinuity) {
                    // Use the host tempo and beat position.
                    _context.tempo_ideal = tempo;
                    _context.tempo_real = tempo;
                    _context.beat_pos = beatPos;
                }
                else {
                    // Adjust real tempo so we can have a continuous beat position.
                    const auto idealBufferBeatDur = frames_to_beats(frame_count, tempo, mSampleRate);
                    const auto hostBeatPosEnd = beatPos + idealBufferBeatDur;
                    const auto actualBufferBeatDur = hostBeatPosEnd - _context.beat_pos;
                    const auto rateScalar = actualBufferBeatDur / idealBufferBeatDur;
                    
                    // Use the incremented beat position.
                    _context.tempo_ideal = tempo;
                    _context.tempo_real = tempo * rateScalar;
                }
            } else {
                _context.tempo_ideal = tempo;
                _context.tempo_real = tempo;
            }
        }
        
        return _context; // take a copy.
    }

};
