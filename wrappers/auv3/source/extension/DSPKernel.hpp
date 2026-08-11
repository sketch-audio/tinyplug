#pragma once

#include <atomic>
#include <array>

#import <AudioToolbox/AudioToolbox.h>
#import <algorithm>
#import <vector>
#import <span>

#include "processor.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"
#include "plug_info.hpp"

#include <tiny_dsp/host_bypass.hpp>
#include <tinyplug/denormal_guard.hpp>

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

        _bypass.reset(static_cast<float>(inSampleRate));
        _bypass.set_latency(_latency);

#if TINY_HAS_WORKER
        bind_worker_to_kernel_classes();
        _worker_runner.start(inSampleRate);
#endif
    }
    
    void deInitialize() {
    }
    
    // MARK: - Bypass
    bool isBypassed() {
        return _bypass.is_bypassed();
    }
    
    void setBypass(bool shouldBypass) {
        _bypass.set_bypassed(shouldBypass);
    }

    // MARK: - Parameter Getter / Setter
    void setParameter(AUParameterAddress address, AUValue value) {
        if (address >= num_params) return;
        const auto addr = static_cast<uint32_t>(address);
        const auto& spec = User_params::param_spec(addr);
        const auto plain = tiny::params::Value_helper::host_to_plain(value, spec.semantics);

        if (_bypass.is_bypassed()) {
            // We may or may not be getting processed while bypassed.
            // process() must watch to see if a resync is necessary.
            _bypass_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        else {
            [[maybe_unused]] const auto success = _param_queue.push(tiny::Set_param{
                .address = addr,
                .value = plain
            });
            assert(success && "Param queue push failed. Increase queue size!");
            if (!success) _needs_resync.store(true, std::memory_order_relaxed); // If we can't push, resync on the next process.
        }

        // Maintain host values (the resync source of truth).
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

    // Render mode (offline/bounce). Pushed from the AU's setRenderingOffline:
    // override (off the audio thread); read on the audio thread in process.
    void setOffline(bool offline) {
        // The render-mode edge in process() resyncs; nothing was lost, so no restate.
        _offline.store(offline, std::memory_order_relaxed);
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

        const auto denormals = tiny::Denormal_guard{}; // Restores the host's FP mode on the way out.

#if TINY_HAS_WORKER
        drain_worker_to_processor();
#endif

        // Resync logic
        const auto needs_resync = _needs_resync.exchange(false, std::memory_order_relaxed); // Queue overflow.
        const auto epoch = _bypass_epoch.load(std::memory_order_relaxed);
        const auto skipped_while_bypassed = epoch != _seen_epoch;
        _seen_epoch = epoch;

        if (needs_resync || skipped_while_bypassed) {
            auto discarded = tiny::Render_event{};
            while (_param_queue.pop(discarded)) {}

            for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
                const auto host_value = _hostvalues[addr].load(std::memory_order_relaxed);
                const auto& spec = User_params::param_spec(addr);
                const auto plain = tiny::params::Value_helper::host_to_plain(host_value, spec.semantics);
                _processor->handle_event(tiny::Set_param{.address = addr, .value = plain});
            }

            // Manifest immediately — a client reading realized state (e.g. an open editor)
            // shouldn't see stale values for the whole bypassed/inactive stretch.
            _processor->handle_event(tiny::Resync_params{});
        }
        else {
            auto event = tiny::Render_event{};
            while (_param_queue.pop(event)) {
                _processor->handle_event(event);
            }
        }

        const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (accepted_latency) {
            const auto new_latency = *accepted_latency;
            _processor->handle_event(tiny::Accepted_latency{new_latency});
            _bypass.set_latency(new_latency);
            assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
        }
        
        auto context = tiny::Dsp_context{.meters = _meters, .propose_latency = {}};
        context.musical_context = resolve_musical_context(frameCount);
        context.render_mode = _offline.load(std::memory_order_relaxed)
            ? tiny::Render_mode::Offline
            : tiny::Render_mode::Realtime;

        // We need to resync on render mode edge.
        if (_last_render_mode != context.render_mode) {
            _processor->handle_event(tiny::Resync_params{});
            _last_render_mode = context.render_mode;
        }
        
        assert(inputBuffers.size() == static_cast<size_t>(mInputChannelCount));
        assert(outputBuffers.size() == static_cast<size_t>(mOutputChannelCount));
        
        // Already spans with size set by process helper.
        context.ibuffers = inputBuffers;
        context.obuffers = outputBuffers;
        context.sbuffers = sidechainBuffers;
        context.num_frames = frameCount;
        
        const auto can_skip = _bypass.can_skip_effect();

        // Resuming from a stretch where advance_rampers() didn't run (can_skip skips
        // process()) — settle before this block's own automation lands, not after.
        if (_was_skipped && !can_skip) {
            _processor->handle_event(tiny::Resync_params{});
        }
        _was_skipped = can_skip;

        if (!can_skip) {
            _processor->process(context);
        }
        
        _bypass.process(inputBuffers, outputBuffers, frameCount);
        
        // Send exports. Not during an offline bounce — editor's almost always closed then anyway.
        for (auto i = decltype(num_meters){}; i < num_meters; ++i) {
            if (context.render_mode != tiny::Render_mode::Offline && context.meters[i] != _last_meters[i]) {
                const auto value = context.meters[i];
                _meter_queue.push(tiny::Set_meter{.address = i, .value = value});
                _last_meters[i] = value;
            }
            _meters[i] = 0; // Reset for peak meters.
        }

        // Has the kernel proposed a new latency? Only act if it actually differs from
        // what we last told the host — otherwise a kernel that re-proposes the same
        // value every block would restart the handshake every block.
        if (const auto proposed_latency = context.propose_latency; proposed_latency && *proposed_latency != _reported_latency) {
            // Audio unit is polling. Could possibly fix.
            _pending_latency.store(*proposed_latency, std::memory_order_release);
            _reported_latency = *proposed_latency;
        }

//        const auto tail = _processor->tail_samps();
//        if (tail != _tail) {
//            // tail changed
//        }
        
        const auto beats_per_sample = _context.tempo_real / (60 * mSampleRate);
        _context.beat_pos += frameCount * beats_per_sample;
    }
    
    // Called by the process helper on the audio thread so we can send events to kernel directly.
    void handleOneEvent(AUEventSampleTime now, AURenderEvent const *event) {
        switch (event->head.eventType) {
            case AURenderEventParameter: {
                const auto address = static_cast<uint32_t>(event->parameter.parameterAddress);
                if (address >= num_params) return;
                const auto& spec = User_params::param_spec(address);
                const auto plain = tiny::params::Value_helper::host_to_plain(event->parameter.value, spec.semantics);
                _processor->handle_event(tiny::Set_param{.address = address, .value = plain});
                _hostvalues[address].store(event->parameter.value, std::memory_order_relaxed); // Maintain host values.
                break;
            }
            case AURenderEventParameterRamp: {
                const auto address = static_cast<uint32_t>(event->parameter.parameterAddress);
                if (address >= num_params) return;
                const auto dur_samples = static_cast<int32_t>(event->parameter.rampDurationSampleFrames);
                const auto& spec = User_params::param_spec(address);
                const auto plain = tiny::params::Value_helper::host_to_plain(event->parameter.value, spec.semantics);
                _processor->handle_event(tiny::Ramp_param{.address = address, .target = plain, .dur_samples = dur_samples});
                _hostvalues[address].store(event->parameter.value, std::memory_order_relaxed); // Maintain host values.
                break;
            }
                
            default:
                break;
        }
    }
    
    void handleParameterEvent(AUEventSampleTime now, AUParameterEvent const& parameterEvent) {
        // Implement handling incoming Parameter events as needed
        this->setParameter(parameterEvent.parameterAddress, parameterEvent.value);
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
    
    auto pop_meter(tiny::Set_meter& meter) -> bool
    {
        return _meter_queue.pop(meter);
    }
    
    auto onHostUpdated(AUParameterAddress /*address*/, AUValue /*value*/) -> void
    {
        // We're immediate in the gui so nothing to do here.
    }
    
private:
    
    // MARK: Member Variables
    AUHostMusicalContextBlock mMusicalContextBlock;
    AUHostTransportStateBlock mTransportStateBlock;
    
    double mSampleRate = 48000;
    int mInputChannelCount = 2;
    int mOutputChannelCount = 2;
    
//    bool mBypassed = false;
    AUAudioFrameCount mMaxFramesToRender = 1024;
    
    using User_params = tiny::params::Infos<tiny::models::Params>;
    using User_meters = tiny::meters::Infos<tiny::models::Meters>;

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

    static constexpr auto queue_size = []() {
        const auto state = 4 * num_params;
        const auto automation = 64 * std::bit_width(num_params); // We expect number of automated parameters to be small but we need to be able to handle a lot of flux.
        return state + automation + 1;
    }();

    //static constexpr auto param_queue_min_size = 4 * num_params + 1;
    using Param_queue = tiny::Lock_free_queue<tiny::Render_event, queue_size, tiny::Queue_concurrency::mpsc>; // I believe SetParameter can happen from multiple threads
    Param_queue _param_queue{};

    // Resync mechanism (see setParameter / setBypass / setOffline / process).
    std::atomic<bool> _needs_resync{false}; // Queue-overflow recovery only. See process().
    std::atomic<uint32_t> _bypass_epoch{};
    uint32_t _seen_epoch{}; // process()-thread only.
    bool _was_skipped{}; // process()-thread only. Detects the can_skip -> processing edge.
    std::optional<tiny::Render_mode> _last_render_mode{}; // process()-thread only. Detects the realtime <-> offline edge.

    static constexpr auto meter_size = 25 * num_meters + 1;
    using Meter_queue = tiny::Lock_free_queue<tiny::Set_meter, meter_size>;
    Meter_queue _meter_queue{};
    
    // Values in host space.
    using Host_value = std::atomic<float>;
    using Host_values = std::array<Host_value, num_params>;
    Host_values _hostvalues{tiny::params::make_defaults<Host_value, User_params>(tiny::params::Space::Host)};
    
    std::array<float, num_meters> _last_meters{};
    
    std::unique_ptr<tiny::plugin::Processor> _processor = std::make_unique<tiny::plugin::Processor>();
    uint32_t _latency{_processor->latency_samps()};
    uint32_t _reported_latency{_latency}; // Don't feedback latency changes.

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // Render mode (offline/bounce). Set via setOffline off the audio thread.
    std::atomic<bool> _offline{false};

    tiny::Musical_context _context{};

    tiny::Host_bypass _bypass{};

#if TINY_HAS_WORKER
public:

    // Worker channel.
    using Worker_from_proc_q = tiny::Lock_free_queue<typename tiny::User_worker::Model::From_processor, tiny::User_worker::Model::inbound_capacity, tiny::Queue_concurrency::spsc>;
    using Worker_from_edit_q = tiny::Lock_free_queue<typename tiny::User_worker::Model::From_editor,    tiny::User_worker::Model::inbound_capacity, tiny::Queue_concurrency::spsc>;
    using Worker_to_proc_q   = tiny::Lock_free_queue<typename tiny::User_worker::Model::To_processor,   tiny::User_worker::Model::outbound_capacity>;
    using Worker_to_edit_q   = tiny::Lock_free_queue<typename tiny::User_worker::Model::To_editor,     tiny::User_worker::Model::outbound_capacity>;

    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    Worker_to_proc_q   _worker_to_proc{};
    Worker_to_edit_q   _worker_to_edit{};

    tiny::User_worker _worker{
        tiny::Worker_replies{
            [this](const auto& m) { return _worker_to_proc.push(m); },
            [this](const auto& m) { return _worker_to_edit.push(m); }
        },
        tiny::Task_manager::Actor{}
    };

    auto bind_worker_to_kernel_classes() -> void
    {
        tiny::try_bind_worker(*_processor, tiny::Worker_processor_actor{
            [this](const auto& m) { return _worker_from_proc.push(m); }
        });
    }

    auto drain_worker_to_processor() -> void
    {
        tiny::try_drain_worker_to_processor(*_processor, _worker_to_proc);
    }

    tiny::Worker_runner<tiny::User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif

private:
    
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
            
            _context.sample_pos = static_cast<int64_t>(samplePos);
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
