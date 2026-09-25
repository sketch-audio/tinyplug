#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <variant>

#include <tiny_core/note_out.hpp>
#include <tiny_core/tiny_midi.hpp>
#include <tiny_core/tiny_state.hpp>
#include <tiny_core/tiny_utils.hpp>

#include <tiny_models.hpp>

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

// Anything a wrapper delivers through `handle` at a frame offset.
using Input = std::variant<Event::Any, Note::Any, Control::Any>;

struct Tagged_event {
    Input event{};
    int32_t offset{std::numeric_limits<decltype(offset)>::max()}; // Frame offset in current buffer.
    uint32_t order{}; // Arrival order, so events at one offset keep the order they came in.
};

// Sorts by offset, keeping arrival order at equal offsets without `std::stable_sort`, which may allocate.
inline auto before(const Tagged_event& a, const Tagged_event& b) -> bool
{
    return a.offset != b.offset ? a.offset < b.offset : a.order < b.order;
}

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
#if TINY_HAS_METERS
    std::span<float> meters{};
#endif
#if TINY_HAS_BLOCKS
    blocks::Writer<models::Resolved::Blocks> blocks{};
#endif
#if TINY_HAS_STATE
    state::Access<models::Resolved::State, state::writers_of<models::Resolved::State>> state{}; // This block only.
#endif
    std::optional<uint32_t> propose_latency{}; // samples.
    Render_mode render_mode{Render_mode::Realtime};
#if TINY_HAS_NOTES_OUT
    Note_outbox::Writer notes{}; // Frames count from the start of this `process` call.
#endif
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
concept Interface = requires(T t) {
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
}
#if TINY_HAS_WORK
// A work model that replies to the processor means handling the replies. Required rather than
// detected, like notes: a misspelled handler would otherwise drop every reply silently.
&& (std::is_same_v<User_work::To_processor, std::monostate> || requires(T t, const User_work::To_processor& reply) {
    { t.handle_worker_reply(reply) } -> std::same_as<void>;
})
#endif
#if TINY_HAS_NOTES_IN
// Declaring notes in (TINY_PLUGIN_WANTS_NOTES) means handling notes and controls, even if one
// visitor is empty: required, so a misspelled overload is a compile error rather than silence.
&& requires(T t) {
    { t.handle(std::declval<const Note::Any&>(/*note*/)) } -> std::same_as<void>;
    { t.handle(std::declval<const Control::Any&>(/*control*/)) } -> std::same_as<void>;
}
#endif
;

// The most latency this configuration can ever propose, read straight after `configure`, where
// what it sizes (the bypass delays) may allocate. Optional: without `max_latency_samps()` the
// latency is fixed at what `configure` came up with, and any proposal above it is refused. Must
// hold from `configure` to the next one, so compute it from the rate, not the live parameters.
template<typename P>
auto max_latency_of(const P& processor) -> uint32_t
{
    if constexpr (requires { processor.max_latency_samps(); }) {
        // Detected by name, so a size_t or int return is used rather than silently ignored.
        static_assert(std::is_integral_v<decltype(processor.max_latency_samps())>, "max_latency_samps() must return a sample count.");
        return std::max(static_cast<uint32_t>(processor.max_latency_samps()), processor.latency_samps());
    }
    else {
        return processor.latency_samps();
    }
}

// Hand one delivered input to the processor's matching `handle`.
template<typename P>
auto deliver(P& processor, const Input& input) -> void
{
    std::visit([&](const auto& e) {
        if constexpr (requires { processor.handle(e); }) processor.handle(e);
    }, input);
}

// A processor that switches MPE itself (a player-facing toggle, typically backed by a
// parameter) answers `mpe_enabled()`. Read at every block, so realtime-safe.
template<typename P>
concept Switches_mpe = requires(const P& p) {
    { p.mpe_enabled() } -> std::same_as<bool>;
};

// Whether MIDI 1.0 member channels read as MPE: never without `expression` declared, always
// with it unless the processor says otherwise.
template<typename P>
auto mpe_enabled([[maybe_unused]] const P& processor) -> bool
{
#if TINY_HAS_NOTE_EXPRESSION
    if constexpr (Switches_mpe<P>) return processor.mpe_enabled();
    else return true;
#else
    return false;
#endif
}

} // namespace tiny::process