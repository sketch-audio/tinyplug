#pragma once

#include <cassert>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>

#include "tiny_events.hpp"
#include "tiny_utils.hpp"

namespace tiny::process {

struct Event {
    struct Set {
        uint32_t address{};
        double value{}; // Plain space.
    };

    struct Ramp {
        uint32_t address{};
        double target{}; // Plain space.
        int32_t dur_samples{};
    };

    using Any = std::variant<Set, Ramp>;
};

struct Tagged_event {
    Event::Any event{};
    int32_t offset{std::numeric_limits<decltype(offset)>::max()}; // Frame offset in current buffer.
};

inline auto frames_to_beats(int64_t frames, double tempo, double sample_rate) noexcept -> double
{
    assert(sample_rate > 0 && "Sample rate must be greater than zero.");
    return static_cast<double>(frames) * tempo / (60 * sample_rate);
}

struct Transport_state {
    bool moving{};
    bool cycling{};
    bool recording{};
};

struct Time_sig {
    int32_t numer{4};
    int32_t denom{4};
};

// TODO: consider whether fields like beat_pos, cycle_start/end, tempo should be optional for hosts that don't provide them.
struct Musical_context {
    int64_t sample_pos{};
    double beat_pos{};
    double cycle_start{}; // cycle start, end in beats
    double cycle_end{};
    double tempo_ideal{120};
    double tempo_real{tempo_ideal};
    Time_sig time_sig{};
    Transport_state transport_state{};
};

// What a wrapper read from the host for one buffer, turned into a full `Musical_context`:
// every field is always supplied. A missing field is `nullopt`; invalid values count as missing.
struct Timeline_resolver {

    struct Report {
        std::optional<int64_t> sample_pos{};
        std::optional<double> beat_pos{};
        std::optional<double> tempo{};
        std::optional<Time_sig> time_sig{};
        std::optional<double> cycle_start{};
        std::optional<double> cycle_end{};
        Transport_state transport_state{};
    };

    // Once per host buffer, at its first frame.
    auto resolve(const Report& r, size_t num_frames, double sr) -> Musical_context
    {
        const auto defaults = Musical_context{};
        const auto finite = [](const std::optional<double>& x) { return x && std::isfinite(*x); };

        auto mc = Musical_context{};
        mc.transport_state = r.transport_state;

        const auto host_tempo = finite(r.tempo) && *r.tempo > 0;
        mc.tempo_ideal = host_tempo ? *r.tempo : defaults.tempo_ideal;
        mc.tempo_real = mc.tempo_ideal;

        const auto valid_sig = r.time_sig && r.time_sig->numer > 0 && r.time_sig->denom > 0;
        mc.time_sig = valid_sig ? *r.time_sig : defaults.time_sig;

        // Position: the host's, else from its beats, else free-running (and not moving).
        if (r.sample_pos) {
            mc.sample_pos = *r.sample_pos;
        }
        else if (finite(r.beat_pos) && host_tempo) {
            mc.sample_pos = std::llround(*r.beat_pos * 60 * sr / mc.tempo_ideal);
        }
        else {
            mc.sample_pos = _free_pos;
            mc.transport_state.moving = false;
        }
        _free_pos = mc.sample_pos + static_cast<int64_t>(num_frames);

        // Beats: the host's, else from the position.
        mc.beat_pos = finite(r.beat_pos) ? *r.beat_pos : frames_to_beats(mc.sample_pos, mc.tempo_ideal, sr);

        if (finite(r.cycle_start) && finite(r.cycle_end)) {
            mc.cycle_start = *r.cycle_start;
            mc.cycle_end = *r.cycle_end;
        }
        else {
            mc.transport_state.cycling = false;
        }
        return mc;
    }

    // A host position reported in samples as a double; non-finite is missing.
    static auto samples(double x) -> std::optional<int64_t>
    {
        if (!std::isfinite(x)) return std::nullopt;
        return static_cast<int64_t>(std::llround(x));
    }

    // The same buffer's context `offset` frames in, for a sub-block.
    static auto at_offset(Musical_context mc, size_t offset, double sr) -> Musical_context
    {
        mc.sample_pos += static_cast<int64_t>(offset);
        mc.beat_pos += frames_to_beats(static_cast<int64_t>(offset), mc.tempo_ideal, sr);
        return mc;
    }

    auto clear() -> void { _free_pos = 0; }

private:
    int64_t _free_pos{};
};

// Whether the host is rendering offline (bounce / freeze / export) rather than
// in real time. Lets a kernel switch to a higher-quality / non-realtime-safe
// path during a bounce. Best-effort: a host that never signals offline stays
// realtime.
enum class Render_mode { Realtime, Offline };

struct Dsp_context {
    Musical_context musical_context{};
    std::span<const float*> ibuffers{};
    std::span<const float*> sbuffers{};
    std::span<float*> obuffers{};
    size_t num_frames{};
    std::span<float> meters{};
    std::optional<uint32_t> propose_latency{}; // samples.
    Render_mode render_mode{Render_mode::Realtime};
};

// What a processor is built for: the rate it will run at, and the parameter values it
// comes up holding. Plain space, indexed by address. `params` is borrowed — it is valid
// only for the duration of the `configure` call, so copy what you need.
struct Config {
    double sr{48000};
    std::span<const double> params{};
};

// Block-boundary synchronization. The configuration stays valid across all of these —
// none of them reconfigures, and none carries a frame offset, which is what separates
// them from `Render_event`.
struct Reset {
    // The stream is restarting: host seek, bounce edge, un-bypass. Past history must not
    // influence future samples — forget delay lines, filter state, oscillator phase,
    // transport position. **Total**: every deferred value lands too, including a long
    // musical smoother. Fires when no audio is flowing, so that costs nothing audible.
    //
    // Owes convergence with `configure`: it must leave the processor where
    // `configure(sr, current_values)` would have. Logic bounces through this call alone
    // while AAX bounces through a full reconstruct, and the two have to agree bit-for-bit.
    struct Hard {};

    // A parameter sync-up with no audio to artifact: a flush block, or resuming from a
    // stretch where `process` did not run. Land what must be exact *now* — anything
    // feeding `latency_samps()` or structural configuration. **Deliberately partial**:
    // history survives, and a long musical glide is explicitly permitted to keep gliding.
    //
    // It exists because ramps only advance inside `process`, and there are stretches where
    // `process` does not run (bypass skip, inactive, flush with no audio). Without a call
    // that manifests outside `process`, a delivered value can stay unrealized for the
    // whole stretch and a client reading realized state sees stale values.
    struct Soft {};

    // The host accepted a latency proposal and has aligned its processing graph. Adopt it
    // immediately: `latency_samps()` must equal `samples` when this call returns.
    //
    // Unlike the two above — which the framework issues whenever it judges them needed,
    // and which are harmless if issued twice — this one is delivered exactly once per
    // acceptance and carries that post-condition.
    struct Latency { std::uint32_t samples{}; };

    using Any = std::variant<Hard, Soft, Latency>;
};

template<typename T>
concept Some_plug_processor = requires(T t) {
    // Two entry points for state, split by what they cost rather than by depth:
    //
    //   sample rate / configuration   configure(cfg)      allocates, off the audio thread
    //   everything else               reset(Reset::Any)   never allocates
    //
    // `reset` is always individually invocable — it never needs a `configure` first — and
    // an implementation is free to do more than its alternative strictly asks. A parameter
    // smoother's history-clearing necessarily lands it, and that overlap is fine because
    // every alternative is idempotent.
    //
    // Resources: size and allocate for this sample rate *and these parameter values*.
    // Off the audio thread. Named `configure` rather than `reset` because it adopts a
    // state rather than returning to an initial one.
    //
    // `configure` is *sufficient on its own* — it implies every `Reset` alternative. On
    // return the processor renders from exactly this configuration with defined output.
    //
    // **The state handed in is the truth, not a request.** `latency_samps()` is final for
    // this configuration and the framework reports it to the host directly, so a processor
    // must come up already in whatever `params` implies: no negotiation, no glide up from
    // defaults on block 1, no dip or cross-fade into it. Concretely, leave
    // `Dsp_context::propose_latency` disengaged on every block until a *live* parameter
    // change moves a structural parameter — re-proposing your own configured latency makes
    // the host renegotiate delay compensation at every reset.
    //
    // Two further obligations, easy to leave unstated and expensive to discover late:
    // `configure` must be **deterministic in `(sr, params)`** — AAX default-constructs the
    // processor at every reset, so anything not reconstructible from those two is gone —
    // and it must be **re-entrant**, since VST3 reconfigures a live object as a routine
    // path.
    { t.configure(std::declval<const Config&>(/*config*/)) } -> std::same_as<void>;

    // Block-boundary synchronization — see `Reset` above for what each alternative owes.
    // Never allocates. Safe on a never-configured processor: a host that resets before it
    // initializes (validators do) must find a no-op, not undefined behaviour.
    { t.reset(std::declval<const Reset::Any&>(/*reset*/)) } -> std::same_as<void>;

    { t.handle(std::declval<const Event::Any&>(/*event*/)) } -> std::same_as<void>;
    { t.process(std::declval<Dsp_context&>(/*context*/)) } -> std::same_as<void>;
    { t.latency_samps() } -> std::same_as<uint32_t>;
    { t.tail_samps() } -> std::same_as<uint32_t>;
};

} // namespace tiny::process