#include "alg_proc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <span>

#include <tinyplug/denormal_guard.hpp>

#include "AAX_ITransport.h"

namespace tiny::aax {

namespace {

// The worker-reply handler is concept-detected on the user's processor. As with the
// queue-draining helpers in tiny_worker.hpp, the check has to live inside a template
// so `if constexpr` discards the branch in a dependent context.
template<typename P, typename Msg>
auto try_handle_worker_reply([[maybe_unused]] P& processor, [[maybe_unused]] const Msg& msg) -> void
{
    if constexpr (has_worker && Receives_worker_reply_to_processor<P>) {
        processor.handle_worker_reply(msg);
    }
}

// Fold newly delivered coefficient segments into process::Event::Set events.
//
// A segment whose `seq` is unchanged is skipped outright; otherwise each of its
// (at most 15) values is compared against the shadow. The shadow is seeded with NaN
// so that the very first delivery reports every address as changed and the kernel
// receives a complete initial state.
auto apply_coefs(const Alg_context* ctx, Alg_state& st) -> void
{
    for (auto seg = size_t{}; seg < num_segments; ++seg) {
        const auto* segment = ctx->coefs[seg];
        if (segment == nullptr) continue;
        if (segment->seq == st.shadow_seq[seg]) continue;
        st.shadow_seq[seg] = segment->seq;

        const auto base = seg * coefs_per_segment;
        const auto count = std::min(coefs_per_segment, num_coefs - base);

        for (auto i = size_t{}; i < count; ++i) {
            const auto value = segment->value[i];
            if (value == st.shadow[base + i]) continue;
            st.shadow[base + i] = value;

            const auto address = static_cast<uint32_t>(base + i);
            if (address == bypass_address) {
                st.bypass.set_bypassed(value >= 0.5);
            }
            else {
                st.processor.handle(process::Event::Set{.address = address, .value = value});
            }
        }
    }
}

auto drain_inbound([[maybe_unused]] const Alg_context* ctx, [[maybe_unused]] Alg_state& st) -> void
{
#if TINY_HAS_WORKER
    if (ctx->inbound == nullptr) return;

    using To_processor = typename User_worker::Model::To_processor;

    ctx->inbound->drain([&st](Ring_kind kind, const void* payload, uint32_t bytes) {
        if (kind != Ring_kind::Worker_to_processor) return;
        if (bytes != sizeof(To_processor)) return;

        auto msg = To_processor{};
        std::memcpy(&msg, payload, sizeof(msg));
        try_handle_worker_reply(st.processor, msg);
    });
#endif
}

// Adopt the reset-time snapshot: render mode, the master bypass, and the parameter
// values the processor should come up holding.
//
// Pro Tools wipes private data at every reset, so the processor arrives default-
// constructed with every value lost. Without this it comes up at defaults and ramps to
// the real values as coefficient packets land after the first block.
//
// Values leave through `params_out` (plain space, indexed by address) and reach the
// processor as `process::Config::params`, not as events replayed before it is configured. That is
// what removes the ordering constraint this function used to carry — AAX was the one
// format where `handle` preceded `reset`, an unenforceable rule with a silent,
// format-specific failure mode. `params_out` arrives pre-filled with defaults, so an
// absent snapshot leaves every address at its default.
auto adopt_reset_state(const Alg_context* context, Alg_state& st, std::span<double> params_out) -> void
{
    const auto* reset = context->reset_state;
    if (reset == nullptr) return;

    st.render_mode = reset->runtime.offline != 0 ? process::Render_mode::Offline : process::Render_mode::Realtime;

    // The latency the host has already accepted. `latency_seq == 0` is the documented
    // sentinel for "the host has not accepted anything yet", so a zero-initialised packet
    // cannot read as "the host accepted zero".
    //
    // Seeding all three matters. `latency_seq` stops block 1 of every reconstruct from
    // re-firing the seq guard in render_instance. `accepted_latency` makes the host's
    // value the truth rather than the kernel's own opinion. And `reported_latency` would
    // otherwise come up 0, so the kernel's first proposal of its actual latency would pass
    // the dedupe guard and ask Pro Tools to renegotiate delay compensation at every reset.
    if (reset->runtime.latency_seq != 0) {
        st.latency_seq = reset->runtime.latency_seq;
        st.accepted_latency = reset->runtime.accepted_latency;
        st.reported_latency = reset->runtime.accepted_latency;
    }

    // Applied unconditionally, unlike apply_coefs: the processor is default-constructed,
    // so every address is new. Seeding the shadow and `seq` here means the next posted
    // packet still diffs correctly and does not re-send what we just applied.
    for (auto seg = size_t{}; seg < num_segments; ++seg) {
        const auto& segment = reset->coefs[seg];
        st.shadow_seq[seg] = segment.seq;

        const auto base = seg * coefs_per_segment;
        const auto count = std::min(coefs_per_segment, num_coefs - base);

        for (auto i = size_t{}; i < count; ++i) {
            const auto value = segment.value[i];
            st.shadow[base + i] = value;

            const auto address = static_cast<uint32_t>(base + i);
            if (address == bypass_address) {
                st.bypass.set_bypassed(value >= 0.5);
            }
            else if (address < params_out.size()) {
                params_out[address] = value;
            }
        }
    }
}

// Everything a `configure` owes, on both paths that can run one. The reset branch in
// alg_init reuses a surviving Alg_state rather than placement-newing a fresh one, but it
// owes the host exactly the same latency bookkeeping — until this was factored out it
// silently skipped all of it.
//
// Latency is separated not by `adding_new` but by whether the value actually moved.
// Re-proposing on every reset would make the host renegotiate delay compensation each
// time, including the two Pro Tools issues per offline bounce while it is rebuilding its
// mixer graph — but staying silent unconditionally would hide a genuine change, since
// `configure` can come up at a different latency than the host holds (a preset moved a
// structural parameter while the block was wiped). `adopt_reset_state` seeds
// `reported_latency` from the snapshot, so the comparison below is silent on an ordinary
// reset and speaks up exactly when the configuration moved.
auto configure_instance(const Alg_context* context, Alg_state& st, double sample_rate) -> void
{
    // Before adopt_reset_state, which seeds both from the snapshot so the next posted
    // packet still diffs correctly.
    st.shadow.fill(std::numeric_limits<double>::quiet_NaN());
    st.shadow_seq.fill(0);

    // The values we come up holding travel in the configuration itself, so the kernel
    // sizes for the mode and latency we are actually in rather than switching into it
    // under audio. `configure` implies clear and snap, so nothing follows it here.
    auto config_params = params::make_defaults<double, User_params>(params::Space::Plain);
    adopt_reset_state(context, st, config_params);

    st.processor.configure(process::Config{.sr = sample_rate, .params = config_params});

    st.bypass.reset(static_cast<float>(sample_rate));

    const auto latency = st.processor.latency_samps();
    st.bypass.set_latency(latency); // Compensates our own delay, so always the kernel's value.

    if (st.latency_seq == 0) {
        // No host-accepted value exists yet — a genuine add, or a host that has never
        // answered. The kernel's own number is the only truth there is.
        st.accepted_latency = latency;
    }

    // Tell the host through the normal proposal path: the data model turns this into
    // SetSignalLatency on the next Direct Data wakeup and the accepted value comes back in
    // the Runtime_packet. Silent when nothing moved — see the note above.
    if (latency != st.reported_latency) {
        if (auto* returns = context->returns) {
            const auto sent = returns->push_value(Ring_kind::Propose_latency, Ring_latency{
                .samples = latency,
                .pad = 0
            });
            if (sent) st.reported_latency = latency;
        }
    }
}

// Bring an algorithm instance up in the host-provided private data block.
//
// Called both when the host adds a new instance and when it resets one whose block it
// has wiped — see the ResetInstance case in alg_init for why the second path exists.
//
// `adding_new` separates the two. Rebuilding DSP state is safe at any time, but the
// host-facing half of instance creation is not, and a reconstruct has no business
// repeating it:
//
//   - Re-initialising the return/inbound rings would reset their read/write positions
//     underneath the data model's timer thread, which reads and writes those same fields
//     through AAX_IPrivateDataAccess. At a genuine add the model is not yet pumping them,
//     so there is nothing to race.
//
// Everything from `configure` onwards is common to both paths and lives in
// `configure_instance` above.
auto construct_instance(const Alg_context* context, Alg_state* st, double sample_rate, bool adding_new) -> void
{
    // Placement new into the host-allocated private data block. Deliberately
    // not done in ResetFieldData: that block is copied into the algorithm's
    // memory pool, which would require Alg_state to be trivially relocatable.
    st = new (static_cast<void*>(st)) Alg_state{};

    // The rings arrive zeroed (the default ResetFieldData), which happens to be
    // their correct initial state — but start their lifetimes properly rather
    // than reading objects that were never constructed. Only ever on a genuine add:
    // the data model is not yet pumping them then, so there is nothing to race.
    if (adding_new) {
        if (context->returns != nullptr) new (static_cast<void*>(context->returns)) Return_ring{};
        if (context->inbound != nullptr) new (static_cast<void*>(context->inbound)) Inbound_ring{};
    }

    configure_instance(context, *st, sample_rate);

#if TINY_HAS_WORKER
    if (auto* returns = context->returns) {
        try_bind_worker(st->processor, Worker_processor_actor{
            [returns](const auto& m) {
                return returns->push(Ring_kind::Worker_from_processor, &m, static_cast<uint32_t>(sizeof(m)));
            }
        });
    }
#endif
    st->constructed = true;
}

auto read_musical_context(const Alg_context* ctx, bool recording) -> process::Musical_context
{
    auto out = process::Musical_context{};
    if (ctx->transport_node == nullptr) return out;

    auto* transport = ctx->transport_node->GetTransport();
    if (transport == nullptr) return out;

    auto tempo = double{};
    auto ts_numer = int32_t{};
    auto ts_denom = int32_t{};
    auto is_playing = bool{};
    auto tick_pos = int64_t{};
    auto is_looping = bool{};
    auto loop_start_tick = int64_t{};
    auto loop_end_tick = int64_t{};
    auto sample_pos = int64_t{};
    auto ticks_per_beat = uint32_t{};

    // A failed query leaves its local zero-initialised, so each field keeps the
    // free-running default rather than reporting 0 bpm / 0/0 as if it were real.
    const auto ok = [](AAX_Result r) { return r == AAX_SUCCESS; };

    if (ok(transport->GetCurrentTempo(&tempo))) {
        out.tempo_ideal = tempo;
        out.tempo_real = tempo;
    }
    if (ok(transport->GetCurrentMeter(&ts_numer, &ts_denom)) && ts_denom != 0) {
        out.time_sig = {ts_numer, ts_denom};
    }
    if (ok(transport->GetCurrentNativeSampleLocation(&sample_pos))) {
        out.sample_pos = sample_pos;
    }

    auto per_beat = double{1};
    if (ok(transport->GetCurrentTicksPerBeat(&ticks_per_beat)) && ticks_per_beat != 0) {
        per_beat = static_cast<double>(ticks_per_beat);
    }

    if (ok(transport->GetCurrentTickPosition(&tick_pos))) {
        out.beat_pos = static_cast<double>(tick_pos) / per_beat;
    }
    if (ok(transport->GetCurrentLoopPosition(&is_looping, &loop_start_tick, &loop_end_tick))) {
        out.cycle_start = static_cast<double>(loop_start_tick) / per_beat;
        out.cycle_end = static_cast<double>(loop_end_tick) / per_beat;
        out.transport_state.cycling = is_looping;
    }
    if (ok(transport->IsTransportPlaying(&is_playing))) {
        out.transport_state.moving = is_playing;
    }
    out.transport_state.recording = recording;

    return out;
}

template<int32_t num_channels>
auto render_instance(Alg_context* ctx) -> void
{
    const auto denormals = Denormal_guard{}; // Restores the host's FP mode on the way out.

    auto* st = ctx->state;
    if (st == nullptr) {
        return;
    }

    const auto num_frames = ctx->num_frames != nullptr
        ? static_cast<size_t>(*ctx->num_frames)
        : size_t{};

    if (ctx->audio_in == nullptr || ctx->audio_out == nullptr) {
        return;
    }

    // The instance-init callback should always have run by now. If it somehow did
    // not, still honour the AAX contract that every output sample is written.
    if (!st->constructed) {
        for (auto ch = int32_t{}; ch < num_channels; ++ch) {
            if (auto* out = ctx->audio_out[ch]) {
                std::fill_n(out, num_frames, 0.f);
            }
        }
        return;
    }

    static constexpr auto no_runtime = Runtime_packet{
        .latency_seq = 0, .accepted_latency = 0,
        .offline = 0, .recording = 0, .delay_comp = 1, .pad = 0
    };
    const auto runtime = ctx->runtime != nullptr ? *ctx->runtime : no_runtime;

    drain_inbound(ctx, *st);

    // Latency the host has accepted; the kernel must match it immediately. Gated on
    // the sequence rather than the value, so a not-yet-populated packet cannot be read
    // as "the host accepted zero".
    if (runtime.latency_seq != 0 && runtime.latency_seq != st->latency_seq) {
        st->latency_seq = runtime.latency_seq;
        st->accepted_latency = runtime.accepted_latency;
        st->processor.reset(process::Reset::Latency{runtime.accepted_latency});
        st->bypass.set_latency(runtime.accepted_latency);
        // AAX owns the value and may clamp it, so this is the format where a mismatch is a
        // live possibility rather than a kernel bug.
        assert(st->processor.latency_samps() == runtime.accepted_latency
            && "Kernel must apply the accepted latency!");
    }

    apply_coefs(ctx, *st);

    // Buffer pointers.
    const auto channels = static_cast<size_t>(num_channels);
    for (auto ch = size_t{}; ch < channels; ++ch) {
        st->ibuffers[ch] = ctx->audio_in[ch];
        st->obuffers[ch] = ctx->audio_out[ch];
    }
#if TINY_WANTS_SIDECHAIN
    if (ctx->sidechain_index != nullptr) {
        const auto sc = static_cast<size_t>(*ctx->sidechain_index);
        for (auto i = size_t{}; i < max_schannels; ++i) {
            st->sbuffers[i] = ctx->audio_in[sc + i];
        }
    }
#endif

    auto context = process::Dsp_context{
        .musical_context = read_musical_context(ctx, runtime.recording != 0),
        .ibuffers = {st->ibuffers.begin(), channels},
        .sbuffers = {st->sbuffers.begin(), Plug_info::wants_sidechain ? max_schannels : 0},
        .obuffers = {st->obuffers.begin(), channels},
        .num_frames = num_frames,
        .meters = st->meters.scratch()
    };
    // Latched at the last reset, never mid-render. The kernel therefore only ever sees
    // this change across a reset — a point at which it has already been cleared and
    // snapped — so a late-arriving flag can no longer wipe history under flowing audio.
    context.render_mode = st->render_mode;

    const auto can_skip = st->bypass.can_skip_effect();

    // Resuming from a stretch where advance_rampers() didn't run (can_skip skips
    // process()) — settle before this block's own automation lands, not after.
    if (st->was_skipped && !can_skip) {
        st->processor.reset(process::Reset::Soft{});
    }
    st->was_skipped = can_skip;

    if (!can_skip) {
        st->processor.process(context);
    }
    st->bypass.process({st->ibuffers.begin(), channels}, {st->obuffers.begin(), channels}, num_frames);

    // Meters out. Suppressed during an offline bounce; the publisher still resets
    // peaks so a bounce cannot hoard a spike. A ring that refuses the value leaves
    // the shadow alone, so the update is retried rather than lost until the meter
    // happens to move again.
    const auto offline = (context.render_mode == process::Render_mode::Offline);
    st->meters.publish(offline, [ctx](uint32_t address, float value) {
        if (ctx->returns == nullptr) return false;
        return ctx->returns->push_value(Ring_kind::Meter, Ring_meter{
            .address = address,
            .pad = 0,
            .value = static_cast<double>(value)
        });
    });

    // Latency proposal out. The data model turns this into SetSignalLatency, the host
    // answers with a notification, and the accepted value comes back in Runtime_packet.
    // Only act if it actually differs from what we last told the host — otherwise a
    // kernel that re-proposes the same value every block would restart the handshake
    // every block.
    if (const auto proposed = context.propose_latency) {
        // A steady intention restated every block is not a new proposal. PDC disabled by
        // the host drops it outright — known unfixed bug: nothing re-proposes if it is
        // switched back on.
        const auto wants_push = *proposed != st->reported_latency && runtime.delay_comp != 0;
        if (wants_push && ctx->returns != nullptr) {
            // Only advance on a successful push, like the meters above — a proposal
            // dropped by a full ring is retried on the next callback rather than lost
            // until the kernel happens to propose again.
            const auto sent = ctx->returns->push_value(Ring_kind::Propose_latency, Ring_latency{
                .samples = *proposed,
                .pad = 0
            });
            if (sent) st->reported_latency = *proposed;
        }
    }
}

template<int32_t num_channels>
auto render_batch(Alg_context* const begin[], const void* end) -> void
{
    for (auto* const* walk = begin; walk < end; ++walk) {
        render_instance<num_channels>(*walk);
    }
}

} // namespace

// MARK: - entrypoints

void AAX_CALLBACK alg_render_mono(Alg_context* const instances_begin[], const void* instances_end)
{
    render_batch<1>(instances_begin, instances_end);
}

void AAX_CALLBACK alg_render_stereo(Alg_context* const instances_begin[], const void* instances_end)
{
    render_batch<2>(instances_begin, instances_end);
}

int32_t AAX_CALLBACK alg_init(const Alg_context* context, AAX_EComponentInstanceInitAction action)
{
    if (context == nullptr || context->state == nullptr) return 0;
    auto* st = context->state;

    // The AddSampleRate context field, which the host populates directly — the sanctioned
    // way for an algorithm to read the rate (AAX_IComponentDescriptor.h: "host-provided
    // information in the algorithm's context structure"; DemoGain_Smoothed reads
    // `*instance->mSampleRate`). Measured live here at both init actions, which is what
    // lets the kernel's allocating reset() happen off the real-time thread.
    //
    // Deliberately not relayed through a data port of our own: a port is only refreshed by
    // GenerateCoefficients, which the host does not run at a reset, so it would be
    // retained-but-stale at exactly the moment we read it — see Reset_state.
    const auto sample_rate = context->sample_rate != nullptr && *context->sample_rate > 0
        ? static_cast<double>(*context->sample_rate)
        : 44100.;

    switch (action) {
        case AAX_eComponentInstanceInitAction_AddingNewInstance: {
            construct_instance(context, st, sample_rate, /*adding_new=*/true);
            return 0;
        }
        case AAX_eComponentInstanceInitAction_ResetInstance: {
            if (st->constructed) {
                configure_instance(context, *st, sample_rate);
            }
            else {
                construct_instance(context, st, sample_rate, /*adding_new=*/false);
            }
            return 0;
        }
        case AAX_eComponentInstanceInitAction_RemovingInstance: {
            if (st->constructed) {
                st->constructed = false;
                st->~Alg_state();
            }
            return 0;
        }
        default:
            return 0;
    }
}

} // namespace tiny::aax
