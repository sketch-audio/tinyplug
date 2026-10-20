// The web format's processor module: the plug-in's processor alone, for an AudioWorklet. A standalone wasm with no
// Emscripten JS; its memory is imported and shared, so the controller's pump reaches the rings by byte offset.
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <pthread.h>
#include <cstring>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <tiny_plugin.hpp>
#include <tiny_dsp/host_bypass.hpp>

#include "plug_info.hpp"
#include "../manifest.hpp"
#include "../wire_layout.hpp"

namespace tiny::web {
namespace {

constexpr auto max_channels = std::size_t{2};
constexpr auto max_frames = std::size_t{1024}; // AudioWorklet renders 128 today; Chrome's render size may grow.
constexpr auto max_events = std::size_t{1024};
constexpr auto num_params = User_params::num_params;


struct Processor_module {
    Inbound_ring inbound{};
    Outbound_ring outbound{};
    std::array<std::array<float, max_frames>, max_channels> inputs{};
    std::array<std::array<float, max_frames>, max_channels> sidechain{};
    std::array<std::array<float, max_frames>, max_channels> outputs{};
    std::array<double, num_params> params{};
    Wire_table table{};

    std::unique_ptr<User_processor> processor{};
    Host_bypass bypass{};
    process::Timeline_resolver timeline{};
#if TINY_HAS_METERS
    meters::Publisher<models::Resolved::Meters> meters{};
#endif
#if TINY_HAS_BLOCKS
    blocks::Publisher<models::Resolved::Blocks> block_publisher{};
    Block_stores stores{};
    std::array<Block_entry, User_blocks::num_blocks> block_entries{};
#endif
#if TINY_HAS_STATE
    state::Processor_for<State_model> state{};
    State_load_store state_load{};
    State_edit_store state_edit{};
    State_snapshot_store state_snapshot{};
    State_edit_frame edit_scratch{};
    State_snapshot_frame snapshot_scratch{}; // Staged here, not on the audio thread's stack.
    std::uint32_t edit_seen{};
    std::uint64_t load_seen{};
    State_model load_scratch{};
#endif
    process::Note_io notes{};
    bool passing{}; // A bypassed note effect forwards what comes in.
    std::vector<process::Event::Ordered> events{};
    std::optional<Transport> transport{};
    std::optional<std::uint32_t> accepted{}; // A proposal the processor gets as `Reset::Latency` next block.
    std::uint32_t latency{};
    std::int64_t frame{}; // Frames rendered since configure.
    double sr{48000};
    bool needs_hard{};
    bool was_skipped{};
    std::string manifest{};

    Processor_module()
    {
        events.reserve(max_events);
        for (auto addr = decltype(num_params){}; addr < num_params; ++addr) {
            params[addr] = params::Value_helper::default_value(User_params::param_spec(addr), params::Space::Plain);
        }
        table = Wire_table{
            .inbound = address_of(inbound),
            .inbound_capacity = Inbound_ring::capacity,
            .outbound = address_of(outbound),
            .outbound_capacity = Outbound_ring::capacity,
            .inputs = address_of(inputs),
            .sidechain = address_of(sidechain),
            .outputs = address_of(outputs),
            .max_channels = max_channels,
            .max_frames = max_frames,
            .params = address_of(params),
            .num_params = num_params,
        };
#if TINY_HAS_BLOCKS
        [this]<std::uint32_t... I>(std::integer_sequence<std::uint32_t, I...>) {
            ((block_entries[I] = Block_entry{
                .offset = address_of(std::get<I>(stores)),
                .frame_bytes = std::tuple_element_t<I, Block_stores>::frame_bytes,
                .slot_offset = std::tuple_element_t<I, Block_stores>::offset_slots,
            }), ...);
        }(std::make_integer_sequence<std::uint32_t, User_blocks::num_blocks>{});
        table.blocks = address_of(block_entries);
        table.num_blocks = User_blocks::num_blocks;
#endif
#if TINY_HAS_STATE
        table.state_edit = {address_of(state_edit), State_edit_store::frame_bytes, State_edit_store::offset_slots};
        table.state_snapshot = {address_of(state_snapshot), State_snapshot_store::frame_bytes, State_snapshot_store::offset_slots};
        table.state_load = {address_of(state_load), State_load_store::frame_bytes, State_load_store::offset_slots};
#endif
    }

    template<typename T>
    static auto address_of(const T& x) -> std::uint32_t { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&x)); }

    // Off the audio thread in spirit: the worklet calls it from its init message, before rendering starts.
    auto configure(double sample_rate) -> void
    {
        sr = sample_rate;
        processor = std::make_unique<User_processor>();
        processor->configure(process::Config{.sr = sr, .params = params});
#if TINY_HAS_WORKER
        try_bind_worker(*processor, Worker_processor_actor{
            [this](const auto& m) { return outbound.push(Kind::Worker_message, &m, static_cast<std::uint32_t>(sizeof(m))); }
        });
#endif
        notes.clear();
        latency = processor->latency_samps();
        bypass.reset(static_cast<float>(sr));
        bypass.set_max_latency(process::max_latency_of(*processor));
        bypass.set_latency(latency);
        accepted.reset();
        frame = 0;
        timeline.clear();
        outbound.push_value(Kind::Latency, Samples{.samples = latency});
    }

    auto drain_inbound() -> void
    {
        auto order = std::uint32_t{};
        const auto add = [&](process::Event::Any event, std::int32_t offset) {
            if (events.size() < events.capacity()) events.push_back({.event = event, .offset = std::max(offset, 0), .order = order++});
        };
        inbound.drain([&](Kind kind, const void* payload, std::uint32_t) {
            switch (kind) {
                case Kind::Param_set: {
                    const auto& e = *static_cast<const Param_set*>(payload);
                    if (e.address < num_params) {
                        params[e.address] = e.value;
                        add(process::Event::Param{process::Event::Set{.value = e.value, .address = e.address}}, e.offset);
                    }
                    break;
                }
                case Kind::Param_ramp: {
                    const auto& e = *static_cast<const Param_ramp*>(payload);
                    if (e.address < num_params) {
                        params[e.address] = e.value;
                        add(process::Event::Param{process::Event::Ramp{.value = e.value, .address = e.address, .samples = e.samples}}, e.offset);
                    }
                    break;
                }
                case Kind::Bypass: bypass.set_bypassed(static_cast<const Flag*>(payload)->on != 0); break;
                case Kind::Reset_hard: needs_hard = true; break;
                case Kind::Transport: transport = *static_cast<const Transport*>(payload); break;
#if TINY_HAS_NOTES_IN
                case Kind::Midi: {
                    const auto& m = *static_cast<const Midi*>(payload);
                    notes.from_midi(process::mpe_enabled(*processor), m.status, m.data1, m.data2,
                                    [&](const process::Event::Any& e) { add(e, m.offset); });
                    break;
                }
                case Kind::Editor_note: {
                    auto e = midi::Performance{};
                    std::memcpy(&e, payload, sizeof(e));
                    notes.post_from_editor(e);
                    break;
                }
#endif
#if TINY_HAS_WORKER
                case Kind::Worker_reply: {
                    auto reply = typename User_work::To_processor{};
                    std::memcpy(&reply, payload, sizeof(reply));
                    if constexpr (requires { processor->handle_worker_reply(reply); }) processor->handle_worker_reply(reply);
                    break;
                }
#endif
                default: break;
            }
        });
#if TINY_HAS_NOTES_IN
        notes.drain_editor([&](const process::Event::Any& e) { add(e, 0); }); // The editor's notes land at the top.
#endif
        std::sort(events.begin(), events.end(), process::before);
    }

    // One event to the processor; a note or control also passes through a bypassed note effect.
    auto deliver(const process::Event::Ordered& e) -> void
    {
        process::deliver(*processor, e.event);
#if TINY_HAS_NOTES_OUT
        if (passing) {
            std::visit(Inline_visitor{
                [](const process::Event::Param&) {},
                [&](const auto& x) { notes.outbox().pass(e.offset, x); },
            }, e.event);
        }
#endif
    }

    // The page's transport for this quantum: its position advances from the anchor while playing.
    auto read_timeline(std::size_t frames) -> process::Timeline
    {
        auto t = process::Timeline{};
        auto host_pos = std::optional<std::int64_t>{};
        if (transport) {
            const auto& tr = *transport;
            host_pos = tr.playing ? tr.position + (frame - tr.anchor) : tr.position;
            if (tr.tempo > 0) {
                t.tempo = tr.tempo;
                t.beat_pos = static_cast<double>(*host_pos) * tr.tempo / (60 * sr); // The page's timeline has one tempo.
            }
            if (tr.numer > 0 && tr.denom > 0) t.signature = process::Timeline::Signature{tr.numer, tr.denom};
            t.state = process::Timeline::State{.moving = tr.playing != 0};
        }
        return timeline.resolve(t, host_pos, frames, sr);
    }

    auto render(std::size_t frames) -> void
    {
        frames = std::min(frames, max_frames);
        const auto denormals = Denormal_guard{};

        if (accepted) {
            processor->reset(process::Reset::Latency{*accepted});
            bypass.set_latency(*accepted);
            assert(processor->latency_samps() == *accepted && "Kernel must apply the accepted latency!");
            latency = *accepted;
            outbound.push_value(Kind::Latency, Samples{.samples = latency});
            accepted.reset();
        }

        events.clear();
        drain_inbound();
        if (std::exchange(needs_hard, false)) {
            processor->reset(process::Reset::Hard{});
            notes.clear();
            bypass.clear();
            bypass.snap();
        }

        auto context = process::Context{};
#if TINY_HAS_METERS
        context.meters = meters.scratch();
#endif
#if TINY_HAS_BLOCKS
        context.blocks = blocks::Writer{&block_publisher};
#endif
#if TINY_HAS_STATE
        // A host load replaces the document; any edit after it applies to the loaded one.
        if (const auto seq = state_load.read(load_scratch); seq != 0 && seq != load_seen) {
            load_seen = seq;
            state.on_session_load(load_scratch);
        }
        // The controller's latest edit, if new. The processor's copy sorts out order and refusal by `seq`.
        if (const auto seq = state_edit.read(edit_scratch); seq != 0 && seq != edit_seen) {
            edit_seen = static_cast<std::uint32_t>(seq);
            state.on_edit(edit_scratch.bytes, edit_scratch.seq);
        }
        auto state_block = std::optional<state::Processor_for<State_model>::Block>{};
        state_block.emplace(&state); // Applies a staged edit now; publishes on reset, below.
        context.state = state::Access_for<State_model>{&*state_block};
#endif
#if TINY_HAS_NOTES_OUT
        const auto bypassed = notes.bypassed(bypass.is_bypassed());
        passing = bypassed && Plug_info::kind == Plugin_kind::Note_effect;
#else
        const auto bypassed = false;
#endif
        const auto block_timeline = read_timeline(frames);

        auto ins = std::array<const float*, max_channels>{};
        auto sides = std::array<const float*, max_channels>{};
        auto outs = std::array<float*, max_channels>{};
        const auto num_in = Plug_info::Wants::audio_in ? max_channels : 0;
        const auto num_side = Plug_info::wants_sidechain ? max_channels : 0;
        const auto num_out = Plug_info::Wants::audio_out ? max_channels : 0;

        const auto run = [&](std::size_t offset, std::size_t count) {
            for (auto ch = std::size_t{}; ch < max_channels; ++ch) {
                ins[ch] = inputs[ch].data() + offset;
                sides[ch] = sidechain[ch].data() + offset;
                outs[ch] = outputs[ch].data() + offset;
            }
            context.inputs = {ins.data(), num_in};
            context.sidechain = {sides.data(), num_side};
            context.outputs = {outs.data(), num_out};
            context.num_frames = count;
            context.timeline = process::Timeline_resolver::at_offset(block_timeline, offset, sr);
#if TINY_HAS_NOTES_OUT
            notes.outbox().begin_slice(static_cast<std::int64_t>(offset), static_cast<std::int64_t>(count));
            context.notes = notes.writer(bypassed);
#else
            (void)bypassed;
#endif
            processor->process(context);
        };

        const auto can_skip = bypass.can_skip_effect();
        if (was_skipped && !can_skip) processor->reset(process::Reset::Soft{});
        was_skipped = can_skip;

        auto next = events.begin();
        if (can_skip) {
            for (; next != events.end(); ++next) deliver(*next);
        }
        else {
            auto now = std::size_t{};
            while (now < frames) {
                const auto until = next == events.end() ? frames : std::min<std::size_t>(static_cast<std::size_t>(next->offset), frames);
                if (until > now) {
                    run(now, until - now);
                    now = until;
                }
                for (; next != events.end() && static_cast<std::size_t>(next->offset) <= now; ++next) deliver(*next);
                if (next == events.end() && now < frames) {
                    run(now, frames - now);
                    now = frames;
                }
            }
            for (; next != events.end(); ++next) deliver(*next); // Offsets past the quantum.
        }

        // Host bypass. Without an audio input the dry signal is silence, so bypass fades out.
        if constexpr (Plug_info::Wants::audio_out && Plug_info::kind != Plugin_kind::Note_effect) {
            static const auto silence = std::array<float, max_frames>{};
            auto dry = std::array<const float*, max_channels>{};
            auto wet = std::array<float*, max_channels>{};
            for (auto ch = std::size_t{}; ch < max_channels; ++ch) {
                dry[ch] = Plug_info::Wants::audio_in ? inputs[ch].data() : silence.data();
                wet[ch] = outputs[ch].data();
            }
            bypass.process({dry.data(), max_channels}, {wet.data(), max_channels}, frames);
        }

#if TINY_HAS_METERS
        meters.publish(false, [this](std::uint32_t address, float value) {
            return outbound.push_value(Kind::Meter, Meter{.address = address, .value = value});
        });
#endif

#if TINY_HAS_STATE
        state_block.reset(); // Publishes what the block changed, then a snapshot if it moved.
        state::pump(state, [this](std::span<const std::byte> bytes, std::uint32_t gen) {
            snapshot_scratch.gen = gen;
            std::copy_n(bytes.begin(), std::min(bytes.size(), snapshot_scratch.bytes.size()), snapshot_scratch.bytes.begin());
            state_snapshot.publish(snapshot_scratch);
            return true;
        });
#endif

#if TINY_HAS_BLOCKS
        block_publisher.transmit(false, [this](auto i, const auto& frame) {
            std::get<decltype(i)::value>(stores).publish(frame);
            return true;
        });
#endif

#if TINY_HAS_NOTES_OUT
        // What the processor sent, after all-notes-off if the stream broke.
        {
            const auto send = [this](std::int32_t at, const midi::Bytes& bytes) {
                if (bytes.size == 0) return;
                auto m = Midi_out{.frame = at, .size = std::min<std::uint32_t>(bytes.size, sizeof(Midi_out::bytes))};
                std::copy_n(bytes.data.begin(), m.size, m.bytes);
                outbound.push_value(Kind::Midi_out, m);
            };
            if (notes.take_all_off()) {
                for (auto channel = std::uint8_t{}; channel < 16; ++channel) send(0, midi::all_notes_off(channel));
            }
            auto& box = notes.outbox();
            for (const auto& entry : box.events()) {
                std::visit([&](const auto& e) { send(static_cast<std::int32_t>(entry.frame), midi::encode(e)); }, entry.event);
            }
            box.clear();
        }
#endif

        // No delay compensation to negotiate: a proposal is accepted at once, and lands at the next block boundary.
        if (const auto proposed = bypass.admit(context.propose_latency); proposed && *proposed != latency) accepted = *proposed;

        frame += static_cast<std::int64_t>(frames);
    }

    auto build_manifest() -> const char*
    {
        manifest = web::manifest();
        return manifest.c_str();
    }
};

Processor_module* module_instance{};

} // namespace
} // namespace tiny::web

extern "C" {

#if defined(__EMSCRIPTEN__)
// The wasm-workers runtime (shared memory, no pthreads) leaves libc++abi's emergency allocator wanting a mutex. This
// module only ever runs on the worklet's one thread, so there is nothing to exclude. (Natively, for the comparison
// render in tests/web, the real ones stand.)
auto pthread_mutex_lock(pthread_mutex_t*) -> int { return 0; }
auto pthread_mutex_unlock(pthread_mutex_t*) -> int { return 0; }
#endif

__attribute__((used, visibility("default"))) auto tiny_web_create() -> const tiny::web::Wire_table*
{
    if (!tiny::web::module_instance) tiny::web::module_instance = new tiny::web::Processor_module{};
    return &tiny::web::module_instance->table;
}

__attribute__((used, visibility("default"))) auto tiny_web_configure(double sample_rate) -> void
{
    tiny::web::module_instance->configure(sample_rate);
}

__attribute__((used, visibility("default"))) auto tiny_web_process(std::uint32_t frames) -> void
{
    tiny::web::module_instance->render(frames);
}

__attribute__((used, visibility("default"))) auto tiny_web_manifest() -> const char*
{
    return tiny::web::module_instance->build_manifest();
}

} // extern "C"
