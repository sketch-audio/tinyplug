#include "audio_effect.hpp"

#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <tiny_core/denormal_guard.hpp>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "base/source/fstreamer.h"

#include <tiny_plugin.hpp>
#include "plug_info.hpp"

#include "adapters.hpp"
#include "messaging.hpp"

namespace tiny::vst3 {

// MARK: - worker

#if TINY_HAS_WORKER

constexpr auto k_worker_from_processor_id = "tiny/worker/from_processor";
constexpr auto k_worker_to_processor_id   = "tiny/worker/to_processor";

auto Audio_effect::_setup_worker() -> void
{
    // Realtime-safe push from the audio thread: lock-free SPSC push, no allocation. The
    // worker relay forwards over IMessage (`_send_worker_messages`).
    try_bind_worker(*_processor, Worker_processor_actor{
        [this](const auto& m) -> bool { return _worker_outbound.push(m); }
    });

    // Worker → processor replies arrive via IMessage on notify().
    _router.register_handler(k_worker_to_processor_id, [this](std::span<const std::byte> bytes, uint32_t tag) {
        using To_proc = typename User_work::To_processor;
        _worker_to_proc_inbox.push(vst3::reconstruct_variant<To_proc>(bytes, tag));
    });
}

// IConnectionPoint::notify is [UI-thread & Connected]: a host proxy may drop a send from any
// other thread, so worker traffic leaves through a relay like everything else the processor sends.
// A Relay delivers on the UI thread on every platform, which is also why the drain below is bounded.
auto Audio_effect::_send_worker_messages() -> void
{
    // At most one queue's worth per delivery: this runs on the UI thread, and a `while (pop)`
    // against an audio thread that keeps pushing need never end. The rest goes next tick.
    auto m = typename User_work::From_processor{};
    for (auto n = size_t{}; n < User_work::inbound_capacity && _worker_outbound.pop(m); ++n) {
        _to_ctrl.send_variant(k_worker_from_processor_id, m);
    }
}

#endif // TINY_HAS_WORKER

#if TINY_HAS_STATE
auto Audio_effect::_setup_state() -> void
{
    _router.register_handler(k_state_edit_id, [this](std::span<const std::byte> bytes, uint32_t seq) {
        _state.on_edit(bytes, seq);
    });
}
#endif

Steinberg::tresult PLUGIN_API Audio_effect::notify(Steinberg::Vst::IMessage* message)
{
    if (_router.dispatch(message)) return Steinberg::kResultOk;
    return Super::notify(message);
}

// MARK: - latency notification

// Tells the controller the host's latency number went stale. Never called from the audio
// thread: `sendMessage` allocates. Correctness may not depend on this landing — a host
// with no connected controller gets kResultFalse and still reads the right value from
// `getLatencySamples`.
auto Audio_effect::_send_latency(uint32_t latency) -> void
{
    _to_ctrl.send_pod(k_latency_changed_id, latency);
}

#if TINY_HAS_BLOCKS
auto Audio_effect::_send_blocks() -> void
{
    _block_outbox.read(_block_scratch);
    blocks::for_each_address<models::Resolved::Blocks>([this](auto i) {
        if (const auto* frame = _block_scratch.fresh<decltype(i)::value>()) {
            _to_ctrl.send_pod(k_blocks_id, *frame, decltype(i)::value);
        }
    });
}
#endif

auto Audio_effect::_drain_worker_to_processor() -> void
{
#if TINY_HAS_WORKER
    try_drain_worker_to_processor(*_processor, _worker_to_proc_inbox);
#endif
}


// MARK: - initialize

Steinberg::tresult PLUGIN_API Audio_effect::initialize(Steinberg::FUnknown* context)
{
    // Here the Plug-in will be instantiated.

    // Initialize the parent.
    auto result = Super::initialize(context);

    if (result != Steinberg::kResultOk) {
        return result;
    }

    // Create the audio IO.

    using namespace Steinberg::Vst; // SpeakerArr, BusTypes

    if constexpr (Plug_info::Wants::audio_in) addAudioInput(u"Input", SpeakerArr::kStereo, BusTypes::kMain);
    if constexpr (Plug_info::wants_sidechain) addAudioInput(u"Sidechain", SpeakerArr::kStereo, BusTypes::kAux);
    if constexpr (Plug_info::Wants::audio_out) addAudioOutput(u"Output", SpeakerArr::kStereo, BusTypes::kMain);
    if constexpr (Plug_info::Wants::notes_in) addEventInput(u"Notes In", 16);
    if constexpr (Plug_info::Wants::notes_out) addEventOutput(u"Notes Out", 16);

    return Steinberg::kResultOk;
}

Steinberg::tresult PLUGIN_API Audio_effect::terminate()
{
    // Here the Plug-in will be de-instantiated, last possibility to remove some memory!

    // Backstop: hosts skip this far less often than they skip `setActive(false)`.
    _relay.reset();
#if TINY_HAS_STATE
    _state_relay.reset();
#endif
#if TINY_HAS_BLOCKS
    _block_relay.reset();
#endif
#if TINY_HAS_WORKER
    _worker_relay.reset();
#endif

    // Do not forget to call parent.
    return Steinberg::Vst::AudioEffect::terminate();
}

Steinberg::tresult PLUGIN_API Audio_effect::setupProcessing(Steinberg::Vst::ProcessSetup& newSetup)
{
    using namespace params;
    using namespace process;

    // Clear handshake state for reconfigure.
    _pending_latency.store(std::nullopt, std::memory_order_release);
    _accepted_latency.store(std::nullopt, std::memory_order_release);
    _did_peek.store(false, std::memory_order_relaxed);

    // Get the initial state for this configuration.
    auto config_values = std::array<double, num_params>{};
    for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
        // Convert to plain space.
        const auto& param = User_params::param_spec(addr);
        const auto knob = _host_values[addr].load(std::memory_order_relaxed);
        const auto plain = Value_helper::knob_to_plain(knob, param.semantics);
        config_values[addr] = plain;
    }

    _processor->configure(Config{
        .sr = newSetup.sampleRate,
        .params = config_values
    });
    _latency = _processor->latency_samps();
    _tail.store(_processor->tail_samps(), std::memory_order_relaxed);

    // The host re-queries `getLatencySamples` after setup ([UI-thread & Setup Done]), so a
    // reconfigure is not a change to announce — but a host that does not re-query still
    // needs telling, and this is the one place we can tell it with no render in flight.
    // The exchange syncs the shadow whether or not the send lands, so a reconfigure can
    // never be mistaken for a mid-render change and notified from `process` instead.
    const auto latency = _latency.load(std::memory_order_relaxed);
    if (latency != _reported_latency.exchange(latency, std::memory_order_relaxed)) {
        _send_latency(latency);
    }

    _bypass.reset(static_cast<float>(newSetup.sampleRate));
    _bypass.set_max_latency(max_latency_of(*_processor));
    _bypass.set_latency(_latency);

    const auto max_samples = static_cast<size_t>(newSetup.maxSamplesPerBlock);
    for (auto& channel : _input_data) {
        channel.resize(max_samples);
        std::fill(channel.begin(), channel.end(), 0.f);
    }

    // Create the event IO.
    static constexpr auto events_size = [](auto samples) {
        const auto state = 4 * num_params;
        const auto scale = std::max(samples / 256, size_t{1});
        const auto automation = scale * 64 * std::bit_width(num_params); // We expect number of automated parameters to be small but we need to be able to handle a lot of flux.
        return state + automation + 1;
    };
    _events.reserve(events_size(max_samples) + (Plug_info::Wants::notes_in ? 1024 : 0)); // Want fixed size event vector.
#if TINY_HAS_NOTES_IN
    _staged.reserve(1024);
#endif

    return Steinberg::Vst::AudioEffect::setupProcessing(newSetup);
}

Steinberg::tresult PLUGIN_API Audio_effect::setActive(Steinberg::TBool state)
{
    // Called when the Plug-in is enable/disable (On/Off).

    // When activating, we need to see if there is a pending latency.
    if (state) {
        // Host accepted.
        const auto pending = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
        if (pending.has_value()) {
            _accepted_latency.store(*pending, std::memory_order_release);
            _latency.store(*pending, std::memory_order_relaxed);
            _send_latency(*pending); // Don't rely on the host having noticed the proposal.
        }

        // Scoped to the active window, not to object lifetime: `setActive` is
        // [UI-thread & Setup Done] by spec, so the "stop runs on the main thread"
        // precondition stops being an assumption about host behaviour.
        _relay.emplace(Relay::Spec{
            .execute = [this]() {
                const auto proposed = _pending_latency.load(std::memory_order_acquire);
                if (proposed.has_value()) _send_latency(*proposed);
            },
            .interval = 0.05
        });
#if TINY_HAS_BLOCKS
        _block_relay.emplace(Relay::Spec{
            .execute = [this]() { _send_blocks(); },
            .interval = 1. / 60.
        });
#endif
#if TINY_HAS_STATE
        if constexpr (state::Processor_for<models::Resolved::State>::processor_writes) {
            _state_relay.emplace(Relay::Spec{
                .execute = [this]() {
                    const auto lock = std::lock_guard{_state_mutex};
                    state::pump(_state, [this](std::span<const std::byte> bytes, uint32_t gen) {
                        return _to_ctrl.send(k_state_snapshot_id, bytes, gen);
                    });
                },
                .interval = 1. / 60.
            });
        }
#endif

#if TINY_HAS_WORKER
        _worker_relay.emplace(Relay::Spec{
            .execute = [this]() { _send_worker_messages(); },
            .interval = std::chrono::duration<double>(User_work::update_period).count()
        });
#endif
    }
    else {
        _notes.clear(); // Released downstream at the next block.
        _relay.reset();
#if TINY_HAS_BLOCKS
        _block_relay.reset();
#endif
#if TINY_HAS_STATE
        _state_relay.reset();
#endif
#if TINY_HAS_WORKER
        _worker_relay.reset();
#endif
    }

    return Steinberg::Vst::AudioEffect::setActive(state);
}

// The stream is discontinuous on both edges of this call. Deferred to `process` so it
// can't race a block already in flight.
Steinberg::tresult PLUGIN_API Audio_effect::setProcessing(Steinberg::TBool state)
{
    _needs_clear.store(true, std::memory_order_relaxed);
    return Steinberg::Vst::AudioEffect::setProcessing(state);
}

Steinberg::tresult PLUGIN_API Audio_effect::setBusArrangements(Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns, Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts)
{
    using namespace Steinberg::Vst;

    const auto expected_ins = (Plug_info::Wants::audio_in ? 1 : 0) + (Plug_info::wants_sidechain ? 1 : 0);
    const auto expected_outs = Plug_info::Wants::audio_out ? 1 : 0;

    if (numIns != expected_ins || numOuts != expected_outs) return Steinberg::kResultFalse;
    if ((numIns > 0 && !inputs) || (numOuts > 0 && !outputs)) return Steinberg::kResultFalse;
    if constexpr (!Plug_info::Wants::audio_out) return Steinberg::kResultTrue; // A note effect: no audio to arrange.

    // What does the host want to do? An instrument has no main input: only its output counts.
    auto& output_arr = outputs[0];
    const auto out_count = SpeakerArr::getChannelCount(output_arr);
    const auto in_count = Plug_info::Wants::audio_in ? SpeakerArr::getChannelCount(inputs[0]) : out_count;
    const auto wants_mono = in_count == 1 && out_count == 1;
    const auto wants_stereo = in_count == 2 && out_count == 2;

    // We will accept either mono or stereo sidechain.
    auto accept_sidechain = [&]() {
        if constexpr (Plug_info::wants_sidechain) {
            auto& sidechain_arr = inputs[sidechain_bus];
            getAudioInput(sidechain_bus)->setArrangement(sidechain_arr);
            _schannels = static_cast<size_t>(SpeakerArr::getChannelCount(sidechain_arr));
        }
    };

    const auto accept = [&](size_t channels) {
        if constexpr (Plug_info::Wants::audio_in) getAudioInput(0)->setArrangement(inputs[0]);
        getAudioOutput(0)->setArrangement(output_arr);
        _ichannels = Plug_info::Wants::audio_in ? channels : 0;
        _ochannels = channels;
        accept_sidechain();
        return Steinberg::kResultTrue;
    };

    if (wants_mono && Plug_info::can_process_mono) return accept(1);
    if (wants_stereo) return accept(2);
    return Steinberg::kResultFalse;
}

Steinberg::tresult PLUGIN_API Audio_effect::canProcessSampleSize(Steinberg::int32 symbolicSampleSize)
{
    // By default kSample32 is supported.
    if (symbolicSampleSize == Steinberg::Vst::kSample32)
        return Steinberg::kResultTrue;

    return Steinberg::kResultFalse;
}

// MARK: - process

Steinberg::tresult PLUGIN_API Audio_effect::process(Steinberg::Vst::ProcessData& data)
{
    using namespace process;

    const auto denormals = Denormal_guard{}; // Restores the host's FP mode on the way out.
    this->_drain_worker_to_processor();

    // Latency fallback completes handshake on first `started` after a proposal.
    const auto [playing, started] = [&]() {
        auto context = data.processContext;
        if (!context) return std::pair{false, false};

        const auto state = context->state;
        const auto has_flag = [](auto x, auto f) { return (x & f) > 0; };

        const auto p = has_flag(state, Steinberg::Vst::ProcessContext::kPlaying);
        const auto s = !_was_moving && p;
        return std::pair{p, s};
    }();
    _was_moving = playing;

    // Fallback complete handshake now in case of non-conforming host..
    if (started && _did_peek.load(std::memory_order_relaxed)) {
        if (const auto pending = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel)) {
            _accepted_latency.store(*pending, std::memory_order_release); // The kernel should manifest on the next process.
            _latency.store(*pending, std::memory_order_relaxed); // The non-conforming path.
        }
        _did_peek.store(false, std::memory_order_relaxed);
    }

    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        const auto new_latency = static_cast<uint32_t>(*accepted_latency);
        _processor->reset(Reset::Latency{new_latency});
        _bypass.set_latency(new_latency);
        assert(_processor->latency_samps() == new_latency && "Kernel must apply the accepted latency!");
    }

    // Discontinuity requested by the host — forget history before anything this block
    // delivers lands.
    if (_needs_clear.exchange(false, std::memory_order_relaxed)) {
        _processor->reset(Reset::Hard{});
        _notes.clear();
        _bypass.clear();    // Its delay lines hold pre-seek dry audio.
        _bypass.snap();
    }

    // Values from a state load.
    _loaded.consume([this](uint32_t address, double value) {
        _processor->handle(process::Event::Set{address, value});
    });

    // Validate shape up front. Bus indices follow what the plug-in carries: an instrument's
    // first input is its sidechain.
    const auto has_inputs = data.numInputs > 0 && data.inputs && Plug_info::Wants::audio_in;
    const auto has_sidechain = data.numInputs > sidechain_bus && data.inputs && Plug_info::wants_sidechain;
    const auto has_outputs = data.numOutputs > 0 && data.outputs && Plug_info::Wants::audio_out;

    const auto required_in_channels = static_cast<Steinberg::int32>(_ichannels);
    const auto required_out_channels = static_cast<Steinberg::int32>(_ochannels);
    const auto required_sc_channels = static_cast<Steinberg::int32>(_schannels);

    const auto inputs_shape_ok = !has_inputs
        || (data.inputs[0].channelBuffers32 != nullptr && data.inputs[0].numChannels >= required_in_channels);
    const auto outputs_shape_ok = !has_outputs
        || (data.outputs[0].channelBuffers32 != nullptr && data.outputs[0].numChannels >= required_out_channels);
    const auto sidechain_shape_ok = !has_sidechain
        || (data.inputs[sidechain_bus].channelBuffers32 != nullptr && data.inputs[sidechain_bus].numChannels >= required_sc_channels);

    const auto shape_ok = inputs_shape_ok && outputs_shape_ok && sidechain_shape_ok;

    // Guarded on `shape_ok`: a null `channelBuffers32` is exactly what the shape check
    // above rejects, and indexing it to look for null *channels* would dereference it.
    auto main_input_pointers_ok = true;
    if (shape_ok && has_inputs) {
        for (size_t i = 0; i < _ichannels; ++i) {
            if (data.inputs[0].channelBuffers32[i] == nullptr) {
                main_input_pointers_ok = false;
                break;
            }
        }
    }

    auto main_output_pointers_ok = true;
    if (shape_ok && has_outputs) {
        for (size_t i = 0; i < _ochannels; ++i) {
            if (data.outputs[0].channelBuffers32[i] == nullptr) {
                main_output_pointers_ok = false;
                break;
            }
        }
    }

    auto sidechain_pointers_ok = true;
    if (shape_ok && has_sidechain) {
        for (size_t i = 0; i < _schannels; ++i) {
            if (data.inputs[sidechain_bus].channelBuffers32[i] == nullptr) {
                sidechain_pointers_ok = false;
                break;
            }
        }
    }

    const auto pointers_ok = main_input_pointers_ok && main_output_pointers_ok && sidechain_pointers_ok;

    // Well-formed *and* actually carrying samples on both the main input and the main
    // output. Everything else is a flush.
    const auto renders_audio = shape_ok && pointers_ok && data.numSamples > 0
        && (has_inputs || !Plug_info::Wants::audio_in) && (has_outputs || !Plug_info::Wants::audio_out);

    // A bypassed note effect forwards its input instead of running; an edge releases what was sounding.
#if TINY_HAS_NOTES_OUT
    const auto bypassed = _notes.bypassed(_bypass.is_bypassed());
    _passing = bypassed && Plug_info::kind == Plugin_kind::Note_effect;
#else
    const auto bypassed = false;
#endif

#if TINY_HAS_NOTES_IN
    // The editor's notes land at the top of the block.
    _notes.drain_editor([this](const process::Input& input) { _input(input, 0); });
#endif

    _events.clear(); // Events only valid for this render cycle.
    this->normalize_input_events(data, renders_audio);

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

    // Create the context. The scratch is NOT blanket-cleared here: what survives a
    // block is per-policy and the publisher decides it at the end (levels persist,
    // peaks and events do not).
    auto context = Dsp_context{};
#if TINY_HAS_STATE
    // Scoped by hand: the block must publish before the relay is told there is something to send.
    auto state_block = std::optional<state::Processor_for<models::Resolved::State>::Block>{};
    state_block.emplace(&_state);
    context.state = state::Access_for<models::Resolved::State>{&*state_block};
#endif
#if TINY_HAS_METERS
    context.meters = _meters.scratch();
#endif
#if TINY_HAS_BLOCKS
    context.blocks = blocks::Writer{&_blocks};
#endif

    // kPrefetch (sampler pre-roll / variable-rate playback) is not a bounce → realtime.
    // `ProcessSetup::processMode` is the canonical field; some hosts leave the per-block
    // `data.processMode` at 0, so either one saying offline is enough.
    const auto is_offline_bounce = (data.processMode == Steinberg::Vst::kOffline)
                                || (processSetup.processMode == Steinberg::Vst::kOffline);
    context.render_mode = is_offline_bounce ? Render_mode::Offline : Render_mode::Realtime;

    // A processMode transition (entering/leaving an offline bounce) is a discontinuity:
    // the audio either side is unrelated, so forget history as well as manifesting values
    // rather than gliding in.
    if (data.processMode != _last_process_mode) {
        _processor->reset(Reset::Hard{});
        _notes.clear();
        _bypass.clear();    // Its delay lines hold pre-bounce dry audio.
        _bypass.snap();
        _last_process_mode = data.processMode;
    }

    // Copy main input to internal buffers in case of in-place processing.
    if (renders_audio) {
        for (size_t i = 0; i < _ichannels; ++i) {
            const auto* in = data.inputs[0].channelBuffers32[i];
            auto& channel = _input_data[i];
            assert(channel.size() >= static_cast<size_t>(data.numSamples) && "Input buffer too small!");
            std::copy(in, in + data.numSamples, channel.begin());
        }
    }

    // So we can process with an offset.
    auto do_process = [this, &data, &context, has_inputs, has_outputs, has_sidechain, bypassed](size_t num_frames, size_t offset) {
#if TINY_HAS_NOTES_OUT
        _notes.outbox().begin_slice(static_cast<int64_t>(offset), static_cast<int64_t>(num_frames));
        context.notes = _notes.writer(bypassed);
#else
        (void)bypassed;
#endif
        assert(offset + num_frames <= static_cast<size_t>(data.numSamples) && "Offset + num_frames exceeds data.numSamples!");

        // Assign buffer ptrs.
        if (has_inputs) {
            assert(data.inputs[0].numChannels >= static_cast<Steinberg::int32>(_ichannels));
            for (size_t i = 0; i < _ichannels; ++i) {
                _ibuffers[i] = &_input_data[i][offset];
            }
        }
        if (has_outputs) {
            assert(data.outputs[0].numChannels >= static_cast<Steinberg::int32>(_ochannels));
            for (size_t i = 0; i < _ochannels; ++i) {
                _obuffers[i] = &data.outputs[0].channelBuffers32[i][offset];
            }
        }
        if (has_sidechain) {
            assert(data.inputs[sidechain_bus].numChannels >= static_cast<Steinberg::int32>(_schannels));
            for (size_t i = 0; i < _schannels; ++i) {
                _sbuffers[i] = &data.inputs[sidechain_bus].channelBuffers32[i][offset]; // Assume sidechain not "in-place"
            }
        }

        // Resolve the musical context.
        context.musical_context = Musical_context{}; // Default in case processContext is null.
        if (const auto* vst_context = data.processContext; vst_context) {
            const auto sample_pos = vst_context->projectTimeSamples;
            const auto beat_pos = vst_context->projectTimeMusic;
            const auto cycle_start = vst_context->cycleStartMusic;
            const auto cycle_end = vst_context->cycleEndMusic;
            const auto tempo = vst_context->tempo;
            const auto sr = vst_context->sampleRate;
            const auto ts_numer = vst_context->timeSigNumerator;
            const auto ts_denom = vst_context->timeSigDenominator;

            using enum Steinberg::Vst::ProcessContext::StatesAndFlags;
            const auto transport_state = vst_context->state;
            const auto has_flag = [](auto x, auto f) { return (x & f) > 0; };

            context.musical_context = {
                .sample_pos = sample_pos + static_cast<int64_t>(offset),
                .beat_pos = beat_pos + frames_to_beats(static_cast<int64_t>(offset), tempo, sr),
                .cycle_start = cycle_start,
                .cycle_end = cycle_end,
                .tempo_ideal = tempo,
                .time_sig = {ts_numer, ts_denom},
                .transport_state = {
                    .moving = has_flag(transport_state, kPlaying),
                    .cycling = has_flag(transport_state, kCycleActive),
                    .recording = has_flag(transport_state, kRecording)
                }
            };
        }

        using In = std::span<const float*>;
        using Out = std::span<float*>;
        context.ibuffers = has_inputs ? In{_ibuffers.begin(), _ichannels} : In{};
        context.obuffers = has_outputs ? Out{_obuffers.begin(), _ochannels} : Out{};
        context.sbuffers = has_sidechain ? In{_sbuffers.begin(), _schannels} : In{};
        context.num_frames = num_frames;

        _processor->process(context);
    };

    // Do process loop.
    const auto frame_count = data.numSamples;
    auto now = decltype(frame_count){};
    auto remaining = frame_count;

    const auto can_skip = _bypass.can_skip_effect();

    // Resuming from a stretch where advance_rampers() didn't run (can_skip skips
    // process()) — settle before this block's own automation lands, not after.
    if (_was_skipped && !can_skip) {
        _processor->reset(Reset::Soft{});
    }
    _was_skipped = can_skip;

    if (can_skip || !renders_audio) {
        // No kernel run to interleave the events with, so deliver them all.
        auto delivered = false;
        while (event) {
            _input(event->event, event->offset);
            next_event();
            delivered = true;
        }

        // Resync on flush block.
        if (!renders_audio && delivered) {
            _processor->reset(Reset::Soft{});
        }
    }
    else {
        while (remaining > 0) {
            if (!event) {
                const auto offset = frame_count - remaining; // remaining strictly <= frame_count.
                do_process(static_cast<size_t>(remaining), static_cast<size_t>(offset));
                break;
            }

            // Clamp.
            // Compute in 64-bit to avoid signed overflow on pathological offsets.
            const auto delta64 = static_cast<int64_t>(event->offset) - static_cast<int64_t>(now);
            const auto clamped64 = std::clamp<int64_t>(delta64, 0, static_cast<int64_t>(remaining));
            const auto frames_until_event = static_cast<decltype(remaining)>(clamped64);

            if (frames_until_event > 0) {
                const auto offset = frame_count - remaining;
                do_process(static_cast<size_t>(frames_until_event), static_cast<size_t>(offset));
                remaining -= frames_until_event;
                now += frames_until_event;
            }

            do {
                _input(event->event, event->offset);
                next_event();
            } while (event && event->offset <= now);
        }
    }

    // Host bypass. Without an audio input the dry signal is `_input_data`'s silence, so bypass fades out.
    if (renders_audio && has_outputs) {
        auto in_buffers = [&]() {
            auto arr = std::array<const float*, max_ichannels>{};
            for (size_t i = 0; i < std::min(_ochannels, max_ichannels); ++i) {
                arr[i] = &_input_data[i][0];
            }
            return arr;
        }();

        auto out_buffers = [&]() {
            auto arr = std::array<float*, max_ochannels>{};
            for (size_t i = 0; i < _ochannels; ++i) {
                arr[i] = &data.outputs[0].channelBuffers32[i][0];
            }
            return arr;
        }();

        const auto min_channels = std::min({
            has_inputs ? data.inputs[0].numChannels : data.outputs[0].numChannels,
            data.outputs[0].numChannels,
            static_cast<Steinberg::int32>(max_ichannels),
            static_cast<Steinberg::int32>(max_ochannels)
        });
        const auto num_channels = static_cast<size_t>(min_channels);
        _bypass.process(
            {in_buffers.begin(), num_channels},
            {out_buffers.begin(), num_channels},
            static_cast<size_t>(data.numSamples)
        );
    }

#if TINY_HAS_NOTES_OUT
    _send_notes(data);
#endif

#if TINY_HAS_METERS
    auto add_output_event = [&](int32_t id, double value) -> bool {
        auto event_index = Steinberg::int32{};
        if (!data.outputParameterChanges) return false;
        auto* queue = data.outputParameterChanges->addParameterData(static_cast<uint32_t>(id), event_index);
        if (!queue) return false;
        auto point_index = Steinberg::int32{};
        return queue->addPoint(0, value, point_index) == Steinberg::kResultOk; // offset, value, index
    };

    // Two reasons to stay quiet. Live can crash ingesting output-parameter meters
    // during an offline bounce. And a flush block (numSamples == 0, or a buffer
    // shape we refused) ran no audio, so it has measured nothing — publishing there
    // would report a peak of zero, which asserts silence we never observed and
    // reads as a one-frame drop-out in the editor.
    _meters.publish(is_offline_bounce || !renders_audio, [&](uint32_t address, float value) {
        // Normalized on the wire per the VST spec; note the range clamps, so a meter
        // whose plain value can exceed its declared Range is reported at the limit.
        const auto& spec = User_meters::spec(address);
        const auto norm = plain_to_norm(value, spec.range);
        return add_output_event(export_param_offset + static_cast<int32_t>(address), norm);
    });
#endif
#if TINY_HAS_BLOCKS
    auto posted_blocks = false;
    _blocks.transmit(is_offline_bounce, [&](auto address, const auto& frame) {
        posted_blocks = true;
        return _block_outbox.post(address, frame);
    });
    if (posted_blocks && _block_relay) _block_relay->post();
#endif
#if TINY_HAS_STATE
    state_block.reset();
    if (_state_relay) _state_relay->post(); // Cheap; the relay sends only if a block published.
#endif
#if TINY_HAS_WORKER
    if (_worker_relay) _worker_relay->post(); // Cheap; the relay drains whatever the processor pushed.
#endif

    // Latency notifications, now only when actually changed. The configure-time path is
    // handled directly in `setupProcessing`; this is only a runtime proposal, and it goes
    // out through the relay because `sendMessage` allocates.
    //
    // Silent during an offline bounce, for the same reason the meters are: a bounce cannot
    // usefully renegotiate delay compensation, and the host's response to being told
    // (`restartComponent(kLatencyChanged)`) interrupts playback mid-render. The pending
    // value survives, and `getLatencySamples` still completes the handshake whenever the
    // host next asks.
    const auto reported = _reported_latency.load(std::memory_order_relaxed);
    if (const auto proposed = _bypass.admit(context.propose_latency); proposed.has_value() && *proposed != reported) {
        // Set pending, mark reported, & notify.
        _pending_latency.store(*proposed, std::memory_order_release);
        _did_peek.store(false, std::memory_order_relaxed);
        _reported_latency.store(*proposed, std::memory_order_relaxed);
        if (!is_offline_bounce && _relay) _relay->post();
    }

    _tail.store(_processor->tail_samps(), std::memory_order_relaxed); // For getTailSamples, off this thread.

    return Steinberg::kResultOk;
}

// MARK: - state load

Steinberg::tresult PLUGIN_API Audio_effect::setState(Steinberg::IBStream* state)
{
    using namespace params;
    using namespace process;

    if (!state) {
        return Steinberg::kResultFalse;
    }
    auto full = vst3::Full_read_stream{state}; // Hosts may return short reads.
    state = &full;

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    auto header = State_rules::Vst3::Header{};
    if (!streamer.readInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    // Validate for real, not just in debug: hosts hand us chunks from other plug-ins
    // and truncated session files, and every count below is untrusted until checked.
    if (header[0] != Plug_info::framework_code) return Steinberg::kResultFalse;
    if (header[1] != Plug_info::manufacturer_code) return Steinberg::kResultFalse;
    if (header[2] != Plug_info::plugin_code) return Steinberg::kResultFalse;

    const auto num_stored_values = header[3];

    auto loaded = std::vector<process::Event::Set>{};
    loaded.reserve(num_params);

    auto notify = [&](const auto& spec, float knob_value) {
        if (!State_rules::is_persistent(spec)) return;

        const auto address = spec.identity.address;
        const auto plain_value = Value_helper::knob_to_plain(knob_value, spec.semantics);
        loaded.push_back(process::Event::Set{address, plain_value});

        // Maintain host values.
        _host_values[address].store(knob_value, std::memory_order_relaxed);
    };

    auto read_and_notify = [&](const auto& knob_values, auto index) {
        // Do we have a real value?
        if (const auto knob_value = knob_values[index]; knob_value != State_rules::no_value) {
            notify(User_params::param_spec(index), knob_value);
        }
    };

    // Sized by what we can use, not by what the chunk claims; the stream is still read
    // in full so the bypass float below stays aligned.
    const auto usable_values = std::min<size_t>(num_stored_values, num_params);
    auto stored_values = std::vector<float>(usable_values);
    for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
        auto value = float{};
        if (!streamer.readFloat(value)) {
            return Steinberg::kResultFalse;
        }
        if (i < num_params) stored_values[i] = value;
    }

    if (num_params <= num_stored_values) {
        // Set values stored in state.
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            read_and_notify(stored_values, i);
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_stored_values){}; i < num_stored_values; ++i) {
            read_and_notify(stored_values, i);
        }

        // Set remaining parameters to defaults.
        for (auto i = num_stored_values; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            notify(param, static_cast<float>(Value_helper::default_value(param, Space::Knob)));
        }
    }

    _loaded.push_n(loaded); // One batch: the processor never runs a block on half a load.

    // Try to read bypass state. A preset exporter writes `no_value`: it has no opinion.
    auto bypass_value = float{};
    const auto has_bypass = streamer.readFloat(bypass_value);
    if (has_bypass && bypass_value != State_rules::no_value) {
        _bypass.set_bypassed(bypass_value >= 0.5f);
    }

#if TINY_HAS_STATE
    // The state record follows the bypass; a session without one loads the default document.
    auto record = std::vector<std::byte>{};
    if (has_bypass) {
        record = state::read_record([&](std::byte* out, size_t size) {
            auto got = Steinberg::int32{};
            return state->read(out, static_cast<Steinberg::int32>(size), &got) == Steinberg::kResultOk
                && static_cast<size_t>(got) == size;
        });
    }
    const auto doc = state::decode_record_or_default<models::Resolved::State>(record);
    {
        const auto lock = std::lock_guard{_state_mutex};
        _state.on_session_load(doc);
    }
#endif

    return Steinberg::kResultOk;
}

// MARK: - state save

Steinberg::tresult PLUGIN_API Audio_effect::getState(Steinberg::IBStream* state)
{
    if (!state) {
        return Steinberg::kResultFalse;
    }

    // Streamer convenience wrapper.
    auto streamer = Steinberg::IBStreamer{state};

    // Generate the header.
    auto header = State_rules::Vst3::Header{
        Plug_info::framework_code, // Reserved
        Plug_info::manufacturer_code,
        Plug_info::plugin_code,
        num_params
    };

    if (!streamer.writeInt32uArray(header.data(), static_cast<int32_t>(header.size()))) {
        return Steinberg::kResultFalse;
    }

    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        // Grab state from host values.
        const auto knob_value = static_cast<float>(_host_values[i].load(std::memory_order_relaxed));

        const auto& spec = User_params::param_spec(i);
        const auto to_write = State_rules::is_persistent(spec) ? knob_value : State_rules::no_value;

        if (!streamer.writeFloat(to_write)) {
            return Steinberg::kResultFalse;
        }
    }

    // Write bypass.
    const auto bypass_value = _bypass.is_bypassed() ? 1.f : 0.f;
    if (!streamer.writeFloat(bypass_value)) {
        return Steinberg::kResultFalse;
    }

#if TINY_HAS_STATE
    // The state record, last so older builds stop before it.
    auto doc = models::Resolved::State{};
    {
        const auto lock = std::lock_guard{_state_mutex};
        _state.snapshot(doc);
    }
    const auto record = state::encode_record(doc);
    auto written = Steinberg::int32{};
    if (state->write(const_cast<std::byte*>(record.data()), static_cast<Steinberg::int32>(record.size()), &written) != Steinberg::kResultOk
        || static_cast<size_t>(written) != record.size()) {
        return Steinberg::kResultFalse;
    }
#endif

    return Steinberg::kResultOk;
}

// MARK: - latency, tail

Steinberg::uint32 PLUGIN_API Audio_effect::getLatencySamples()
{
    // Peek pending latency (host got notified already).
    if (const auto pending = _pending_latency.load(std::memory_order_acquire)) {
        _did_peek.store(true, std::memory_order_relaxed); // Fallback for non-conforming hosts.
        return *pending;
    }

    return _latency.load(std::memory_order_relaxed);
}

Steinberg::uint32 PLUGIN_API Audio_effect::getTailSamples()
{
    // Resolve to Steinberg's named constants.
    using namespace Steinberg::Vst;
    // The copy `process` keeps: the host asks on its UI thread, and the processor belongs to the
    // audio thread.
    const auto tail = _tail.load(std::memory_order_relaxed);
    const auto inf_tail = std::numeric_limits<uint32_t>::max();
    return tail == 0 ? kNoTail : (tail == inf_tail ? kInfiniteTail : tail);
}

Steinberg::uint32 PLUGIN_API Audio_effect::getProcessContextRequirements()
{
    auto requirements = Steinberg::Vst::ProcessContextRequirements{};
    requirements.needProjectTimeMusic();
    requirements.needCycleMusic();
    requirements.needTempo();
    requirements.needTimeSignature();
    requirements.needTransportState();
    return requirements.flags;
}

// MARK: - private

auto Audio_effect::normalize_input_events(Steinberg::Vst::ProcessData& data, bool renders_audio) -> void
{
    using namespace params;
    using namespace process;

    if (!data.inputParameterChanges) {
#if TINY_HAS_NOTES_IN
        _collect_notes(data);
        for (auto i = size_t{}; i < _events.size(); ++i) _events[i].order = static_cast<uint32_t>(i);
        std::ranges::sort(_events, process::before);
#endif
        return;
    }
    auto& param_changes = *data.inputParameterChanges;
    const auto num_changes = param_changes.getParameterCount();

    for (auto i = decltype(num_changes){}; i < num_changes; ++i) {
        auto* queue_ptr = param_changes.getParameterData(i);
        if (!queue_ptr) continue;
        auto& queue = *queue_ptr;

        const auto id = queue.getParameterId();

        if (id == bypass_param_id) {
            // Handle immediately
            if (queue.getPointCount() <= 0) continue;
            auto value = Steinberg::Vst::ParamValue{};
            auto offset = int32_t{};
            if (queue.getPoint(0, offset, value) != Steinberg::kResultTrue) continue;
            _bypass.set_bypassed(value >= 0.5);
            continue;
        }

#if TINY_HAS_NOTES_IN
        // A player control, mapped by the controller's `IMidiMapping`.
        if (id >= static_cast<Steinberg::Vst::ParamID>(control_param_offset) && id < static_cast<Steinberg::Vst::ParamID>(control_param_offset + num_control_params)) {
            const auto index = static_cast<int32_t>(id) - control_param_offset;
            const auto channel = static_cast<uint8_t>(index / controls_per_channel);
            const auto which = index % controls_per_channel;
            for (auto point = int32_t{}; point < queue.getPointCount(); ++point) {
                auto value = Steinberg::Vst::ParamValue{};
                auto offset = int32_t{};
                if (queue.getPoint(point, offset, value) != Steinberg::kResultTrue) continue;
                const auto control = which == 0 ? Control::Any{Control::Bend{channel, value * 2. - 1.}}
                                   : which == 1 ? Control::Any{Control::Pressure{channel, value}}
                                   : Control::Any{Control::Pedal{channel, static_cast<Control::Pedal::Kind>(which - 2), value}};
                _stage({.event = midi::encode(control), .offset = std::clamp(offset, 0, std::max(data.numSamples - 1, 0))});
            }
            continue;
        }
        if (id >= static_cast<Steinberg::Vst::ParamID>(timbre_param_offset) && id < static_cast<Steinberg::Vst::ParamID>(timbre_param_offset + num_timbre_params)) {
            const auto channel = static_cast<uint8_t>(static_cast<int32_t>(id) - timbre_param_offset);
            for (auto point = int32_t{}; point < queue.getPointCount(); ++point) {
                auto value = Steinberg::Vst::ParamValue{};
                auto offset = int32_t{};
                if (queue.getPoint(point, offset, value) != Steinberg::kResultTrue) continue;
                const auto cc = midi::Bytes{{static_cast<uint8_t>(0xb0 | channel), 74, midi::detail::seven(value)}, 3};
                _stage({.event = cc, .offset = std::clamp(offset, 0, std::max(data.numSamples - 1, 0))});
            }
            continue;
        }
#endif

        if (id >= User_params::num_params) continue; // Be defensive.

        const auto& param = User_params::param_spec(id); // To denormalize the automation values.

        const auto point_count = queue.getPointCount();

        // Block starts with an implicit point at -1 offset.
        auto previous_offset = int32_t{-1};

        for (auto point_idx = decltype(point_count){}; point_idx < point_count; ++point_idx) {
            auto value = Steinberg::Vst::ParamValue{};
            auto offset = int32_t{};
            if (queue.getPoint(point_idx, offset, value) != Steinberg::kResultTrue) continue;

            // VST3 docs mention implicit point at -1. Hard-clamp to legal range for this block.
            const auto max_offset = std::max(data.numSamples - 1, 0);
            offset = std::clamp(offset, -1, max_offset);

            // Flush blocks don't render audio, so ramp duration is zero samples.
            const auto ramp_dur = renders_audio ? std::max(offset - previous_offset, 0) : 0;

            if (_events.size() == _events.capacity()) {
                // _events vector is full!
                assert(false && "Event vector is full, increase capacity!");
            };

            // Set param
            if (ramp_dur <= 1) {
                _events.push_back({
                    .event = process::Event::Set{
                        .address = id,
                        .value = Value_helper::knob_to_plain(value, param.semantics)
                    },
                    .offset = std::max(previous_offset, {}),
                });
            }
            // Ramp param
            else {
                _events.push_back({
                    .event = process::Event::Ramp{
                        .address = id,
                        .target = Value_helper::knob_to_plain(value, param.semantics),
                        .dur_samples = ramp_dur
                    },
                    .offset = std::max(previous_offset, {}),
                });
            }

            previous_offset = offset;

            // Maintain host values.
            _host_values[id].store(value, std::memory_order_relaxed);
        }
    }

#if TINY_HAS_NOTES_IN
    _collect_notes(data);
#endif

    // Sort by offset, keeping arrival order at equal offsets.
    for (auto i = size_t{}; i < _events.size(); ++i) _events[i].order = static_cast<uint32_t>(i);
    std::ranges::sort(_events, process::before);
}

// MARK: - notes

// [audio] One input for the processor, and for the output while a bypassed note effect forwards.
auto Audio_effect::_input(const process::Input& input, [[maybe_unused]] int32_t offset) -> void
{
    process::deliver(*_processor, input);
#if TINY_HAS_NOTES_OUT
    if (_passing) {
        std::visit(Inline_visitor{
            [](const process::Event::Any&) {},
            [&](const auto& e) { _notes.outbox().pass(offset, e); },
        }, input);
    }
#endif
}

// [audio] The event bus, staged for `_name_notes`. Controls arrive separately, as mapped parameters.
auto Audio_effect::_collect_notes([[maybe_unused]] Steinberg::Vst::ProcessData& data) -> void
{
#if TINY_HAS_NOTES_IN
    using namespace process;
    using Vst_event = Steinberg::Vst::Event;
    auto* list = data.inputEvents;
    if (!list) return _name_notes();

    const auto last = std::max(data.numSamples - 1, 0);
    const auto key_of = [](int16_t pitch) { return static_cast<uint8_t>(pitch); };

    const auto count = list->getEventCount();
    for (auto i = decltype(count){}; i < count; ++i) {
        auto e = Vst_event{};
        if (list->getEvent(i, e) != Steinberg::kResultOk) continue;

        switch (e.type) {
            case Vst_event::kNoteOnEvent: {
                const auto& on = e.noteOn;
                if (on.pitch < 0 || on.pitch > 127) break;
                const auto id = Note::Id{0, static_cast<uint8_t>(on.channel), key_of(on.pitch)};
                auto note = on.velocity > 0.f ? Note::Any{Note::On{id, on.velocity}} : Note::Any{Note::Off{id, 0.f}}; // 0 is an off, as in MIDI.
                _stage({.event = note, .local = on.noteId, .offset = std::clamp(e.sampleOffset, 0, last)});
                break;
            }
            case Vst_event::kNoteOffEvent: {
                const auto& off = e.noteOff;
                if (off.pitch < 0 || off.pitch > 127) break;
                auto note = Note::Any{Note::Off{{0, static_cast<uint8_t>(off.channel), key_of(off.pitch)}, off.velocity}};
                _stage({.event = note, .local = off.noteId, .offset = std::clamp(e.sampleOffset, 0, last)});
                break;
            }
            case Vst_event::kPolyPressureEvent: {
                const auto& pp = e.polyPressure;
                if (pp.pitch < 0 || pp.pitch > 127) break;
                auto note = Note::Any{Note::Expression{{0, static_cast<uint8_t>(pp.channel), key_of(pp.pitch)}, Note::Expression::Kind::Pressure, pp.pressure}};
                _stage({.event = note, .local = pp.noteId, .offset = std::clamp(e.sampleOffset, 0, last)});
                break;
            }
            case Vst_event::kNoteExpressionValueEvent: {
                const auto& x = e.noteExpressionValue;
                using Kind = Note::Expression::Kind;
                auto kind = std::optional<Kind>{};
                auto value = x.value;
                switch (x.typeId) {
                    case Steinberg::Vst::kVolumeTypeID: kind = Kind::Volume; value *= 4.; break; // 0.25 is unity.
                    case Steinberg::Vst::kPanTypeID: kind = Kind::Pan; break;
                    case Steinberg::Vst::kTuningTypeID: kind = Kind::Tuning; value = (value - 0.5) * 240.; break; // ±120 semitones.
                    case Steinberg::Vst::kVibratoTypeID: kind = Kind::Vibrato; break;
                    case Steinberg::Vst::kBrightnessTypeID: kind = Kind::Brightness; break;
                    case pressure_expression_id: kind = Kind::Pressure; break;
                    default: break;
                }
                if (!kind || x.noteId < 0) break;
                // Named by id alone: key 255 matches nothing if the id is unknown.
                auto note = Note::Any{Note::Expression{{0, 0, 255}, *kind, value}};
                _stage({.event = note, .local = x.noteId, .offset = std::clamp(e.sampleOffset, 0, last)});
                break;
            }
            default:
                break;
        }
    }
    _name_notes();
#endif
}

#if TINY_HAS_NOTES_IN
// [audio] Staged notes and controls named in time order, into `_events`. At one offset, controls
// go first: an MPE sender sets a channel's values before its note starts.
auto Audio_effect::_name_notes() -> void
{
    using namespace process;
    for (auto i = size_t{}; i < _staged.size(); ++i) _staged[i].order = static_cast<uint32_t>(i);
    std::ranges::sort(_staged, [](const Staged& a, const Staged& b) {
        if (a.offset != b.offset) return a.offset < b.offset;
        const auto a_note = std::holds_alternative<Note::Any>(a.event);
        if (a_note != std::holds_alternative<Note::Any>(b.event)) return !a_note;
        return a.order < b.order;
    });

    const auto mpe = process::mpe_enabled(*_processor);
    for (const auto& staged : _staged) {
        const auto emit = [&](const Input& input) {
            if (_events.size() < _events.capacity()) _events.push_back({.event = input, .offset = staged.offset});
        };
        std::visit(Inline_visitor{
            [&](const Note::Any& note) { _notes.from_host(mpe, staged.local, note, emit); },
            [&](const midi::Bytes& bytes) { _notes.from_midi(mpe, bytes.data[0], bytes.data[1], bytes.data[2], emit); },
        }, staged.event);
    }
    _staged.clear();
}
#endif

// [audio] What the processor sent this block, after all-notes-off if the stream broke.
auto Audio_effect::_send_notes([[maybe_unused]] Steinberg::Vst::ProcessData& data) -> void
{
#if TINY_HAS_NOTES_OUT
    using namespace process;
    using Vst_event = Steinberg::Vst::Event;
    auto& box = _notes.outbox();
    auto* out = data.outputEvents;
    if (!out) {
        box.clear();
        return;
    }

    const auto id_of = [](const Note::Id& n) { return static_cast<int32_t>(n.id & 0x7fffffff); };
    const auto cc = [&](int32_t offset, int16_t channel, uint8_t number, uint8_t value, uint8_t value2 = 0) {
        auto e = Vst_event{};
        e.busIndex = 0;
        e.sampleOffset = offset;
        e.type = Vst_event::kLegacyMIDICCOutEvent;
        e.midiCCOut.controlNumber = number;
        e.midiCCOut.channel = static_cast<int8_t>(channel);
        e.midiCCOut.value = static_cast<int8_t>(value);
        e.midiCCOut.value2 = static_cast<int8_t>(value2);
        out->addEvent(e);
    };

    if (_notes.take_all_off()) {
        for (auto channel = int16_t{}; channel < 16; ++channel) cc(0, channel, midi::all_notes_off_number, 0);
    }

    for (const auto& entry : box.events()) {
        std::visit(Inline_visitor{
            [&](const Note::Any& note) {
                auto e = Vst_event{};
                e.busIndex = 0;
                e.sampleOffset = entry.frame;
                std::visit(Inline_visitor{
                    [&](const Note::On& n) {
                        e.type = Vst_event::kNoteOnEvent;
                        e.noteOn.channel = n.note.channel;
                        e.noteOn.pitch = n.note.key;
                        e.noteOn.velocity = n.velocity;
                        e.noteOn.noteId = id_of(n.note);
                    },
                    [&](const Note::Off& n) {
                        e.type = Vst_event::kNoteOffEvent;
                        e.noteOff.channel = n.note.channel;
                        e.noteOff.pitch = n.note.key;
                        e.noteOff.velocity = n.velocity;
                        e.noteOff.noteId = id_of(n.note);
                    },
                    [&](const Note::Choke& n) {
                        e.type = Vst_event::kNoteOffEvent;
                        e.noteOff.channel = n.note.channel;
                        e.noteOff.pitch = n.note.key;
                        e.noteOff.noteId = id_of(n.note);
                    },
                    [&](const Note::Expression& n) {
                        using Kind = Note::Expression::Kind;
                        if (n.kind == Kind::Pressure) {
                            e.type = Vst_event::kPolyPressureEvent;
                            e.polyPressure.channel = n.note.channel;
                            e.polyPressure.pitch = n.note.key;
                            e.polyPressure.pressure = static_cast<float>(n.value);
                            e.polyPressure.noteId = id_of(n.note);
                            return;
                        }
                        static constexpr auto types = std::array<Steinberg::Vst::NoteExpressionTypeID, 5>{
                            Steinberg::Vst::kVolumeTypeID, Steinberg::Vst::kPanTypeID, Steinberg::Vst::kTuningTypeID,
                            Steinberg::Vst::kVibratoTypeID, Steinberg::Vst::kBrightnessTypeID};
                        auto value = n.value;
                        if (n.kind == Kind::Volume) value /= 4.;
                        if (n.kind == Kind::Tuning) value = value / 240. + 0.5;
                        e.type = Vst_event::kNoteExpressionValueEvent;
                        e.noteExpressionValue.typeId = types[static_cast<size_t>(n.kind)];
                        e.noteExpressionValue.noteId = id_of(n.note);
                        e.noteExpressionValue.value = std::clamp(value, 0., 1.);
                    },
                }, note);
                out->addEvent(e);
            },
            [&](const auto& message) { // Control::Any or midi::Raw, by status byte.
                const auto bytes = midi::encode(message);
                if (bytes.size == 0) return;
                const auto channel = static_cast<int16_t>(bytes.data[0] & 0x0f);
                const auto note = [&](Vst_event::EventTypes type) {
                    auto e = Vst_event{};
                    e.busIndex = 0;
                    e.sampleOffset = entry.frame;
                    e.type = type;
                    if (type == Vst_event::kNoteOnEvent) {
                        e.noteOn.channel = channel;
                        e.noteOn.pitch = bytes.data[1];
                        e.noteOn.velocity = static_cast<float>(bytes.data[2]) / 127.f;
                        e.noteOn.noteId = -1;
                    }
                    else if (type == Vst_event::kNoteOffEvent) {
                        e.noteOff.channel = channel;
                        e.noteOff.pitch = bytes.data[1];
                        e.noteOff.velocity = static_cast<float>(bytes.data[2]) / 127.f;
                        e.noteOff.noteId = -1;
                    }
                    else {
                        e.polyPressure.channel = channel;
                        e.polyPressure.pitch = bytes.data[1];
                        e.polyPressure.pressure = static_cast<float>(bytes.data[2]) / 127.f;
                        e.polyPressure.noteId = -1;
                    }
                    out->addEvent(e);
                };
                switch (bytes.data[0] & 0xf0) {
                    case 0x80: note(Vst_event::kNoteOffEvent); break;
                    case 0x90: note(bytes.data[2] == 0 ? Vst_event::kNoteOffEvent : Vst_event::kNoteOnEvent); break;
                    case 0xa0: note(Vst_event::kPolyPressureEvent); break;
                    case 0xb0: cc(entry.frame, channel, bytes.data[1], bytes.data[2]); break;
                    case 0xc0: cc(entry.frame, channel, Steinberg::Vst::kCtrlProgramChange, bytes.data[1]); break;
                    case 0xd0: cc(entry.frame, channel, Steinberg::Vst::kAfterTouch, bytes.data[1]); break;
                    case 0xe0: cc(entry.frame, channel, Steinberg::Vst::kPitchBend, bytes.data[1], bytes.data[2]); break;
                    default: break;
                }
            },
        }, entry.event);
    }
    box.clear();
#endif
}

} // namespace tiny::vst3