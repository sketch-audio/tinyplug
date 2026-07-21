#include "alg_proc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>

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

// Fold newly delivered coefficient segments into Set_param events.
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
                st.processor.handle_event(Set_param{.address = address, .value = value});
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

auto read_musical_context(const Alg_context* ctx, bool recording) -> Musical_context
{
    auto out = Musical_context{};
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

    [[maybe_unused]] auto result = AAX_Result{AAX_SUCCESS};
    result = transport->GetCurrentTempo(&tempo);
    result = transport->GetCurrentMeter(&ts_numer, &ts_denom);
    result = transport->IsTransportPlaying(&is_playing);
    result = transport->GetCurrentTickPosition(&tick_pos);
    result = transport->GetCurrentLoopPosition(&is_looping, &loop_start_tick, &loop_end_tick);
    result = transport->GetCurrentNativeSampleLocation(&sample_pos);
    result = transport->GetCurrentTicksPerBeat(&ticks_per_beat);

    if (ticks_per_beat == 0) ticks_per_beat = 1; // Defensive: avoid a divide by zero.
    const auto per_beat = static_cast<double>(ticks_per_beat);

    out.sample_pos = sample_pos;
    out.beat_pos = static_cast<double>(tick_pos) / per_beat;
    out.cycle_start = static_cast<double>(loop_start_tick) / per_beat;
    out.cycle_end = static_cast<double>(loop_end_tick) / per_beat;
    out.tempo_ideal = tempo;
    out.tempo_real = tempo;
    out.time_sig = {ts_numer, ts_denom};
    out.transport_state = {
        .moving = is_playing,
        .cycling = is_looping,
        .recording = recording
    };
    return out;
}

template<int32_t num_channels>
auto render_instance(Alg_context* ctx) -> void
{
    auto* st = ctx->state;
    if (st == nullptr) return;

    const auto num_frames = ctx->num_frames != nullptr
        ? static_cast<size_t>(*ctx->num_frames)
        : size_t{};

    if (ctx->audio_in == nullptr || ctx->audio_out == nullptr) return;

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
        st->processor.handle_event(Accepted_latency{runtime.accepted_latency});
        st->bypass.set_latency(runtime.accepted_latency);
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
    if constexpr (Plug_info::wants_sidechain) {
        if (ctx->sidechain_index != nullptr) {
            const auto sc = static_cast<size_t>(*ctx->sidechain_index);
            for (auto i = size_t{}; i < max_schannels; ++i) {
                st->sbuffers[i] = ctx->audio_in[sc + i];
            }
        }
    }

    auto context = Dsp_context{
        .musical_context = read_musical_context(ctx, runtime.recording != 0),
        .ibuffers = {st->ibuffers.begin(), channels},
        .sbuffers = {st->sbuffers.begin(), Plug_info::wants_sidechain ? max_schannels : 0},
        .obuffers = {st->obuffers.begin(), channels},
        .num_frames = num_frames,
        .meters = st->meters
    };
    context.render_mode = runtime.offline != 0 ? Render_mode::Offline : Render_mode::Realtime;

    if (!st->bypass.can_skip_effect()) {
        st->processor.process(context);
    }
    st->bypass.process({st->ibuffers.begin(), channels}, {st->obuffers.begin(), channels}, num_frames);

    // Meters out. Only changes are sent; the accumulator resets each callback so
    // peak meters report the maximum over the buffer.
    for (auto i = size_t{}; i < num_meters; ++i) {
        const auto value = static_cast<double>(context.meters[i]);
        if (value != st->last_meters[i] && ctx->returns != nullptr) {
            const auto sent = ctx->returns->push_value(Ring_kind::Meter, Ring_meter{
                .address = static_cast<uint32_t>(i),
                .pad = 0,
                .value = value
            });
            // Only advance the shadow on a successful send, so an update dropped by a
            // full ring is retried on the next callback rather than lost until the
            // meter happens to move again.
            if (sent) st->last_meters[i] = value;
        }
        st->meters[i] = 0;
    }

    // Latency proposal out. The data model turns this into SetSignalLatency, the host
    // answers with a notification, and the accepted value comes back in Runtime_packet.
    if (const auto proposed = context.propose_latency; proposed && runtime.delay_comp != 0) {
        if (ctx->returns != nullptr) {
            ctx->returns->push_value(Ring_kind::Propose_latency, Ring_latency{
                .samples = *proposed,
                .pad = 0
            });
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

    // Packets are delivered before this callback runs (step 4 of the documented
    // algorithm initialization order), so the config packet is already in the
    // context and the sample rate is known here — which is what lets the kernel's
    // allocating reset() happen off the real-time thread.
    const auto sample_rate = [&]() -> double {
        if (context->config != nullptr && context->config->sample_rate > 0) {
            return context->config->sample_rate;
        }
        if (context->sample_rate != nullptr && *context->sample_rate > 0) {
            return static_cast<double>(*context->sample_rate);
        }
        return 44100.;
    }();

    switch (action) {
        case AAX_eComponentInstanceInitAction_AddingNewInstance: {
            // Placement new into the host-allocated private data block. Deliberately
            // not done in ResetFieldData: that block is copied into the algorithm's
            // memory pool, which would require Alg_state to be trivially relocatable.
            st = new (static_cast<void*>(st)) Alg_state{};

            // The rings arrive zeroed (the default ResetFieldData), which happens to be
            // their correct initial state — but start their lifetimes properly rather
            // than reading objects that were never constructed.
            if (context->returns != nullptr) new (static_cast<void*>(context->returns)) Return_ring{};
            if (context->inbound != nullptr) new (static_cast<void*>(context->inbound)) Inbound_ring{};

            st->shadow.fill(std::numeric_limits<double>::quiet_NaN());
            st->processor.reset(sample_rate);
            st->bypass.reset(static_cast<float>(sample_rate));

            // Seed the host's latency from the kernel through the normal proposal
            // path: the data model turns this into SetSignalLatency on the first
            // Direct Data wakeup, and the accepted value comes back in Runtime_packet.
            const auto initial_latency = st->processor.latency_samps();
            st->bypass.set_latency(initial_latency);
            st->accepted_latency = initial_latency;
            if (auto* returns = context->returns) {
                returns->push_value(Ring_kind::Propose_latency, Ring_latency{
                    .samples = initial_latency,
                    .pad = 0
                });
            }

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
            return 0;
        }
        case AAX_eComponentInstanceInitAction_ResetInstance: {
            if (st->constructed) {
                st->processor.reset(sample_rate);
                st->bypass.reset(static_cast<float>(sample_rate));
                st->shadow.fill(std::numeric_limits<double>::quiet_NaN());
                st->shadow_seq.fill(0);
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
