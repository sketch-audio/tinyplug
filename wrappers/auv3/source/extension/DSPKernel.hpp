#pragma once

#include <atomic>
#include <array>
#include <limits>
#include <optional>

#import <AudioToolbox/AudioToolbox.h>
#import <algorithm>
#import <vector>
#import <span>

#include <tiny_plugin.hpp>

#include "plug_info.hpp"

#include <tiny_dsp/host_bypass.hpp>
#include <tiny_core/change_set.hpp>
#include <tiny_core/denormal_guard.hpp>

#include <tiny_core/relay.hpp>

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
        using namespace tiny;
        using namespace tiny::params;

        mSampleRate = inSampleRate;
        mInputChannelCount = inputChannelCount;
        mOutputChannelCount = outputChannelCount;

        // Come up configured for the values we already hold (`_hostvalues` is host space).
        auto config_params = std::array<double, num_params>{};
        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
            const auto& param = User_params::param_spec(addr);
            const auto host = _hostvalues[addr].load(std::memory_order_relaxed);
            const auto plain = Value_helper::host_to_plain(host, param.semantics);
            config_params[addr] = plain;
        }
        _processor->configure(tiny::process::Config{
            .sr = mSampleRate,
            .params = config_params
        });
        const auto latency = _processor->latency_samps();
        _latency.store(latency, std::memory_order_release);
        _tail.store(_processor->tail_samps(), std::memory_order_relaxed);

        // What the host is about to be told, so a kernel proposal of the same value is a
        // no-op. Any proposal still outstanding was cleared in `deInitialize`: `configure`
        // supersedes it, and left in place it would be applied against this configuration
        // and trip the assert in `process`.
        _reported_latency.store(latency, std::memory_order_relaxed);

        _bypass.reset(static_cast<float>(inSampleRate));
        _bypass.set_max_latency(tiny::process::max_latency_of(*_processor));
        _bypass.set_latency(latency);
        _silence.assign(mMaxFramesToRender, 0.f);

#if TINY_HAS_WORKER
        bind_worker_to_kernel_classes(); // The AU starts the runner.
#endif
    }
    
    void deInitialize() {
        _pending_latency.store(std::nullopt, std::memory_order_release);
        _accepted_latency.store(std::nullopt, std::memory_order_release);
        _notes.clear(); // Released downstream at the next block.
    }

    // MARK: - Notes

    auto notes() -> tiny::process::Note_io& { return _notes; }

#if TINY_HAS_NOTES_OUT
    // Captured when render resources are allocated; called on the render thread.
    void setMidiOutput(AUMIDIOutputEventBlock block) { _midi_out = block; }
#endif

    // Once per render, before any slice: the editor's notes, and the bypass the block renders with.
    void begin_block(AUEventSampleTime start) {
        _block_start = start;
#if TINY_HAS_NOTES_OUT
        const auto bypassed = _notes.bypassed(_bypass.is_bypassed());
        _passing = bypassed && tiny::Plug_info::kind == tiny::Plugin_kind::Note_effect;
        _writer = _notes.writer(bypassed);
#endif
#if TINY_HAS_NOTES_IN
        _notes.drain_editor([this](const tiny::process::Input& input) { _input(input, 0); });
#endif
    }

    // Once per render, after the last slice: what the processor sent, as MIDI.
    void end_block() {
#if TINY_HAS_NOTES_OUT
        auto& box = _notes.outbox();
        const auto all_off = _notes.take_all_off();
        if (_midi_out) {
            const auto send = [&](int32_t frame, const tiny::midi::Bytes& bytes) {
                if (bytes.size > 0) _midi_out(_block_start + frame, 0, bytes.size, bytes.data.data());
            };
            if (all_off) {
                for (auto channel = uint8_t{}; channel < 16; ++channel) send(0, tiny::midi::all_notes_off(channel));
            }
            for (const auto& entry : box.events()) {
                std::visit([&](const auto& e) { send(entry.frame, tiny::midi::encode(e)); }, entry.event);
            }
        }
        box.clear();
#endif
    }

    // AUAudioUnit's `-reset`: "Reset transitory rendering state to its initial state."
    // Deferred to `process` so it can't race a block already in flight.
    void clear() {
        _needs_clear.store(true, std::memory_order_relaxed);
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

        // Coalesces, so nothing is lost however long process() goes without running.
        _param_changes.push(tiny::process::Event::Set{.address = addr, .value = plain});

        // Maintain host values.
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
        assert(!tiny::Plug_info::Wants::audio_in || inputBuffers.size() == outputBuffers.size());

#if TINY_HAS_NOTES_OUT
        _notes.outbox().begin_slice(bufferStartTime - _block_start, frameCount);
#endif

        const auto denormals = tiny::Denormal_guard{}; // Restores the host's FP mode on the way out.

#if TINY_HAS_WORKER
        drain_worker_to_processor();
#endif

        const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (accepted_latency) {
            const auto new_latency = *accepted_latency;
            _processor->reset(tiny::process::Reset::Latency{new_latency});
            _bypass.set_latency(new_latency);
            assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
        }

        // Discontinuity requested by the host — forget history before anything this block
        // delivers lands.
        if (_needs_clear.exchange(false, std::memory_order_relaxed)) {
            _processor->reset(tiny::process::Reset::Hard{});
            _notes.clear();
            _bypass.clear();    // Its delay lines hold pre-seek dry audio.
            _bypass.snap();
        }

        // Parameter values set off the render thread since the last block.
        const auto delivered = _param_changes.consume([this](uint32_t address, double value) {
            _processor->handle(tiny::process::Event::Set{.address = address, .value = value});
        });

        // Changed while bypassed: manifest now rather than ramping through audio nobody hears,
        // so a client reading realized state (an open editor) doesn't see stale values.
        if (delivered && _bypass.is_bypassed()) {
            _processor->reset(tiny::process::Reset::Soft{});
        }

        auto context = tiny::process::Dsp_context{.propose_latency = {}};
#if TINY_HAS_STATE
        auto state_block = _state.begin_block(); // Applies a staged edit now; publishes when process returns.
        context.state = tiny::state::Access_for<tiny::models::Resolved::State>{&state_block};
#endif
#if TINY_HAS_METERS
        context.meters = _meters.scratch();
#endif
#if TINY_HAS_BLOCKS
        context.blocks = tiny::blocks::Writer{&_blocks};
#endif
        context.musical_context = resolve_musical_context(frameCount);
        context.render_mode = _offline.load(std::memory_order_relaxed)
            ? tiny::process::Render_mode::Offline
            : tiny::process::Render_mode::Realtime;

        // We need to resync on render mode edge.
        // Realtime <-> offline is a discontinuity: the audio either side is unrelated, so
        // forget history as well as manifesting values.
        if (_last_render_mode != context.render_mode) {
            _processor->reset(tiny::process::Reset::Hard{});
            _notes.clear();
            _bypass.clear();    // Its delay lines hold pre-bounce dry audio.
            _bypass.snap();
            _last_render_mode = context.render_mode;
        }
        
        assert(inputBuffers.size() == static_cast<size_t>(mInputChannelCount));
        assert(outputBuffers.size() == static_cast<size_t>(mOutputChannelCount));
        
        // Already spans with size set by process helper. A note effect's output is a silent bus
        // the processor never sees.
        context.ibuffers = inputBuffers;
        context.obuffers = tiny::Plug_info::Wants::audio_out ? outputBuffers : std::span<float*>{};
        context.sbuffers = sidechainBuffers;
        context.num_frames = frameCount;
#if TINY_HAS_NOTES_OUT
        context.notes = _writer;
#endif
        
        const auto can_skip = _bypass.can_skip_effect();

        // Resuming from a stretch where advance_rampers() didn't run (can_skip skips
        // process()) — settle before this block's own automation lands, not after.
        if (_was_skipped && !can_skip) {
            _processor->reset(tiny::process::Reset::Soft{});
        }
        _was_skipped = can_skip;

        if (!can_skip) {
            _processor->process(context);
        }
        
        if constexpr (!tiny::Plug_info::Wants::audio_out) {
            for (auto* channel : outputBuffers) std::fill(channel, channel + frameCount, 0.f);
        }
        else if constexpr (tiny::Plug_info::Wants::audio_in) {
            _bypass.process(inputBuffers, outputBuffers, frameCount);
        }
        else {
            // No audio input: the dry signal is silence, so bypass fades out.
            auto dry = std::array<const float*, max_ochannels>{};
            for (auto& channel : dry) channel = _silence.data();
            const auto channels = std::min(outputBuffers.size(), max_ochannels);
            _bypass.process({dry.data(), channels}, outputBuffers.first(channels), std::min<size_t>(frameCount, _silence.size()));
        }
        
        // Send exports. Suppressed during an offline bounce; the publisher still
        // resets peaks so a bounce cannot hoard a spike.
        const auto offline = (context.render_mode == tiny::process::Render_mode::Offline);
#if TINY_HAS_METERS
        _meters.publish(offline, [this](uint32_t address, float value) {
            _mailbox.post(address, value);
            return true; // A slot array has no capacity to refuse.
        });
#endif
#if TINY_HAS_BLOCKS
        _blocks.transmit(offline, [this](auto address, const auto& frame) {
            return _block_mailbox.post(address, frame);
        });
#endif

        // Has the kernel proposed a new latency? Only act if it actually differs from
        // what we last told the host — otherwise a kernel that re-proposes the same
        // value every block would restart the handshake every block. And stay quiet
        // during an offline bounce for the same reason the meters do: a bounce cannot
        // usefully renegotiate delay compensation, and the host recomputing it
        // mid-render interrupts playback. The pending value survives for the next
        // `latency` query.
        if (const auto proposed_latency = _bypass.admit(context.propose_latency)) {
            const auto reported = _reported_latency.load(std::memory_order_relaxed);
            if (*proposed_latency != reported) {
                // Release publishes `_pending_latency` to the thread that runs `execute`;
                // the relay's own acquire is the matching half. Don't weaken either.
                _pending_latency.store(*proposed_latency, std::memory_order_release);
                _reported_latency.store(*proposed_latency, std::memory_order_relaxed);
                if (!offline && _relay) _relay->post();
            }
        }

        _tail.store(_processor->tail_samps(), std::memory_order_relaxed); // For tailTime, on the main thread.
        
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
                _processor->handle(tiny::process::Event::Set{.address = address, .value = plain});
                _hostvalues[address].store(event->parameter.value, std::memory_order_relaxed); // Maintain host values.
                break;
            }
            case AURenderEventParameterRamp: {
                const auto address = static_cast<uint32_t>(event->parameter.parameterAddress);
                if (address >= num_params) return;
                const auto dur_samples = static_cast<int32_t>(event->parameter.rampDurationSampleFrames);
                const auto& spec = User_params::param_spec(address);
                const auto plain = tiny::params::Value_helper::host_to_plain(event->parameter.value, spec.semantics);
                _processor->handle(tiny::process::Event::Ramp{.address = address, .target = plain, .dur_samples = dur_samples});
                _hostvalues[address].store(event->parameter.value, std::memory_order_relaxed); // Maintain host values.
                break;
            }
                
            case AURenderEventMIDI: {
#if TINY_HAS_NOTES_IN
                const auto& m = event->MIDI;
                const auto offset = static_cast<int32_t>(std::max<AUEventSampleTime>(event->head.eventSampleTime - _block_start, 0));
                _notes.from_midi(tiny::process::mpe_enabled(*_processor), m.data[0], m.length > 1 ? m.data[1] : 0, m.length > 2 ? m.data[2] : 0,
                                 [&](const tiny::process::Input& input) { _input(input, offset); });
#endif
                break;
            }
            default:
                break;
        }
    }

    // One input for the processor, and for the output while a bypassed note effect forwards.
    void _input(const tiny::process::Input& input, [[maybe_unused]] int32_t offset) {
        tiny::process::deliver(*_processor, input);
#if TINY_HAS_NOTES_OUT
        if (_passing) {
            std::visit(tiny::Inline_visitor{
                [](const tiny::process::Event::Any&) {},
                [&](const auto& e) { _notes.outbox().pass(offset, e); },
            }, input);
        }
#endif
    }
    
    void handleParameterEvent(AUEventSampleTime now, AUParameterEvent const& parameterEvent) {
        // Implement handling incoming Parameter events as needed
        this->setParameter(parameterEvent.parameterAddress, parameterEvent.value);
    }
    
    // tiny
    // Scoped to the render-resource window, matching AUv2's Initialize/Cleanup: started in
    // `-allocateRenderResourcesAndReturnError:`, stopped in `-deallocateRenderResources`
    // with `-dealloc` as the backstop. Nothing can post outside that window — only the
    // render path reads `propose_latency` — and `deInitialize` clears any proposal, so a
    // wider scope would have nothing left to deliver.
    auto start_relay(tiny::Relay::Spec spec) -> void
    {
        _relay.emplace(std::move(spec));
    }

    auto stop_relay() -> void
    {
        _relay.reset();
    }

    // What our latency will be once the host reads it. Non-consuming, so the AU can decide
    // whether to post a KVO change without advancing the handshake.
    auto peek_latency_secs() const -> double
    {
        const auto pending = _pending_latency.load(std::memory_order_acquire);
        const auto samps = pending ? *pending : _latency.load(std::memory_order_acquire);
        return mSampleRate > 0 ? samps / mSampleRate : 0;
    }

    // The accept. Called from the AU's `latency` getter, because the host reading the
    // property is the only thing here that means it has adopted the value — a timer tick
    // means nothing to the host's delay compensation.
    auto accept_latency() -> void
    {
        const auto pending = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (!pending) return;

        _accepted_latency.store(*pending, std::memory_order_release); // Kernel manifests on the next process.
        _latency.store(*pending, std::memory_order_release);
    }
    
    auto tail_secs() -> double
    {
        const auto tail = _tail.load(std::memory_order_relaxed); // Never the processor: it belongs to the render thread.
        return tail == std::numeric_limits<uint32_t>::max() ? std::numeric_limits<double>::infinity() : tail / mSampleRate;
    }
    
#if TINY_HAS_METERS
    auto read_meters(std::span<float> out) -> void
    {
        _mailbox.read(out);
    }
#endif
#if TINY_HAS_STATE
    auto state() -> tiny::state::Processor_for<tiny::models::Resolved::State>& { return _state; }
#endif
#if TINY_HAS_BLOCKS
    auto read_blocks(tiny::blocks::Frames<tiny::models::Resolved::Blocks>& out) -> void
    {
        _block_mailbox.read(out);
    }
#endif
    
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

    using User_params = tiny::User_params;
    using User_meters = tiny::User_meters;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{2};
    static constexpr auto max_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};
#if TINY_HAS_METERS
    tiny::meters::Publisher<tiny::models::Resolved::Meters> _meters{}; // Owns the scratch the DSP writes.
#endif
#if TINY_HAS_BLOCKS
    tiny::blocks::Publisher<tiny::models::Resolved::Blocks> _blocks{}; // Staging frames the DSP writes.
#endif
#if TINY_HAS_STATE
    tiny::state::Processor_for<tiny::models::Resolved::State> _state{};
#endif

    // Parameter values set off the render thread (the parameter tree), to process().
    tiny::Change_set<tiny::process::Event::Set, num_params> _param_changes{};

    // Notes: identity, the editor's inbox, the outbox.
    tiny::process::Note_io _notes{};
    AUEventSampleTime _block_start{};
    std::vector<float> _silence{}; // Bypass's dry signal when there's no audio input.
#if TINY_HAS_NOTES_OUT
    AUMIDIOutputEventBlock _midi_out = nil;
    tiny::process::Note_outbox::Writer _writer{};
    bool _passing{};
#endif

    std::atomic<bool> _needs_clear{false}; // Set by clear(), consumed at the top of process().
    bool _was_skipped{}; // process()-thread only. Detects the can_skip -> processing edge.
    std::optional<tiny::process::Render_mode> _last_render_mode{}; // process()-thread only. Detects the realtime <-> offline edge.

#if TINY_HAS_METERS
    tiny::meters::Mailbox<tiny::models::Resolved::Meters> _mailbox{};
#endif
#if TINY_HAS_BLOCKS
    tiny::blocks::Mailbox<tiny::models::Resolved::Blocks> _block_mailbox{};
#endif
    
    // Values in host space.
    using Host_value = std::atomic<float>;
    using Host_values = std::array<Host_value, num_params>;
    Host_values _hostvalues{tiny::params::make_defaults<Host_value, User_params>(tiny::params::Space::Host)};
    
    
    std::unique_ptr<tiny::User_processor> _processor = std::make_unique<tiny::User_processor>();

    // Latency
    std::atomic<uint32_t> _latency{};
    std::atomic<uint32_t> _tail{}; // The processor's tail, refreshed by process and initialize.
    // Written from `process` (audio) and `initialize` (main), so it cannot be plain.
    std::atomic<uint32_t> _reported_latency{}; // Don't feedback latency changes.

    // Carries "latency moved, go read it" from the render thread to the main thread.
    std::optional<tiny::Relay> _relay{};

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // Render mode (offline/bounce). Set via setOffline off the audio thread.
    std::atomic<bool> _offline{false};

    tiny::process::Musical_context _context{};

    tiny::Host_bypass _bypass{};

#if TINY_HAS_WORKER
public:

    // Worker channel. The worker and its queues live on the AU, beside the editor and the task
    // manager, as in every other wrapper; the kernel only reaches the processor's two queues.
    using Worker_from_proc_q = tiny::Lock_free_queue<typename tiny::User_work::From_processor, tiny::User_work::inbound_capacity, tiny::Queue_concurrency::spsc>;
    using Worker_from_edit_q = tiny::Lock_free_queue<typename tiny::User_work::From_editor,    tiny::User_work::inbound_capacity, tiny::Queue_concurrency::spsc>;
    using Worker_to_proc_q   = tiny::Lock_free_queue<typename tiny::User_work::To_processor,   tiny::User_work::outbound_capacity>;
    using Worker_to_edit_q   = tiny::Lock_free_queue<typename tiny::User_work::To_editor,     tiny::User_work::outbound_capacity>;

    // Before `initialize`: it binds the processor to them.
    void bindWorkerQueues(Worker_from_proc_q* from_proc, Worker_to_proc_q* to_proc) {
        _worker_from_proc = from_proc;
        _worker_to_proc = to_proc;
    }

private:

    Worker_from_proc_q* _worker_from_proc{};
    Worker_to_proc_q* _worker_to_proc{};

    auto bind_worker_to_kernel_classes() -> void
    {
        auto* from_proc = _worker_from_proc;
        tiny::try_bind_worker(*_processor, tiny::Worker_processor_actor{
            [from_proc](const auto& m) { return from_proc->push(m); }
        });
    }

    auto drain_worker_to_processor() -> void
    {
        tiny::try_drain_worker_to_processor(*_processor, *_worker_to_proc);
    }
#endif

private:
    
    // MARK: - Musical Context Helpers
    
    auto frames_to_beats(double frames, double tempo, double sr) -> double
    {
        const auto beats_per_frame = tempo / (60 * sr);
        return frames * beats_per_frame;
    }
    
    auto resolve_musical_context(uint32_t frame_count) -> tiny::process::Musical_context
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

// An ivar of the AU, and Objective-C objects are malloc'd: over-alignment can't be honoured.
static_assert(alignof(DSPKernel) <= alignof(std::max_align_t), "DSPKernel must not be over-aligned.");
