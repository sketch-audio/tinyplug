#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

#include <tiny_core/note_out.hpp>
#include <tiny_core/tiny_midi.hpp>
#include <tiny_core/tiny_state.hpp>
#include <tiny_core/tiny_utils.hpp>

#include <tiny_models.hpp>

namespace tiny::process {

struct Event {
    struct Set {
        double value{}; // Plain space.
        std::uint32_t address{};
    };

    struct Ramp {
        double value{}; // Plain space, the ramp's target.
        std::uint32_t address{};
        std::int32_t samples{}; // Ramp length.
    };

    using Param = std::variant<Set, Ramp>;

    // Anything a wrapper delivers through `handle` at a frame offset.
    using Any = std::variant<Param, Note::Any, Control::Any>;

    struct Ordered {
        Any event{};
        std::int32_t offset{}; // Frame offset in the current buffer; untimed is block start.
        std::uint32_t order{}; // Arrival order, so events at one offset keep the order they came in.
    };
};

// Sorts by offset, keeping arrival order at equal offsets without `std::stable_sort`, which may allocate.
inline auto before(const Event::Ordered& a, const Event::Ordered& b) -> bool
{
    return a.offset != b.offset ? a.offset < b.offset : a.order < b.order;
}

// The host's timeline at this block's first frame. Empty means the host didn't say (defaults are yours).
struct Timeline {
    struct Cycle {
        double start{}; // Beats.
        double end{};
    };

    struct Signature {
        std::int32_t numer{};
        std::int32_t denom{};
    };

    struct State {
        bool moving{};
        bool cycling{}; // Whether `cycle` is active; the region may be reported while it isn't.
        bool recording{};
    };

    std::int64_t sample_pos{}; // Always supplied: continuous across one host buffer, may jump between them, free-runs without a host.
    std::optional<double> beat_pos{};
    std::optional<double> tempo{}; // BPM, the host's nominal; drift shows in `beat_pos`.
    std::optional<Signature> signature{};
    std::optional<Cycle> cycle{};
    std::optional<State> state{};
};

// Whether the host is rendering offline (bounce / freeze / export) rather than
// in real time. Lets a kernel switch to a higher-quality / non-realtime-safe
// path during a bounce. Best-effort: a host that never signals offline stays
// realtime.
enum class Mode { Realtime, Offline };

struct Context {
    Timeline timeline{};
    std::span<const float*> inputs{};
    std::span<const float*> sidechain{};
    std::span<float*> outputs{};
    std::size_t num_frames{};
#if TINY_HAS_METERS
    std::span<float> meters{};
#endif
#if TINY_HAS_BLOCKS
    blocks::Writer<models::Resolved::Blocks> blocks{};
#endif
#if TINY_HAS_STATE
    state::Access<models::Resolved::State, state::writers_of<models::Resolved::State>> state{}; // This block only.
#endif
    std::optional<std::uint32_t> propose_latency{}; // samples.
    Mode mode{Mode::Realtime};
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

// Block-boundary commands: facts a processor must adopt to render correctly. The configuration stays
// valid across both, and neither carries a frame offset, which is what separates them from events.
// Everything live (automation, preset and state loads, flushes) arrives as events instead, realized
// by the processor's own policy. Per block the order is `Latency`, then `Restart`, then the events.
struct Reset {
    // Your input stream is discontinuous: a seek, a host reset, the render-mode edge, resuming
    // after the bypass skip. Output from here on must not depend on anything before, and the
    // parameters are exactly `params`: plain space, indexed by address, always filled, borrowed for
    // the call. No reason is given, on purpose: every restart owes the same thing.
    //
    // Owes: the processor ends where `configure(sr, params)` would leave it, except
    // `latency_samps()`, which keeps the accepted value; if `params` imply another, propose it
    // from `process`, since the host owns that number. "Forget history" means transient
    // processing state only: recorded content (a looper's loop, a loaded sample) is persisted
    // state and survives. Idempotent.
    struct Restart { std::span<const double> params{}; };

    // The host accepted a latency proposal and has aligned its processing graph. Adopt it
    // immediately: `latency_samps()` must equal `samples` when this call returns. Delivered
    // exactly once per acceptance.
    struct Latency { std::uint32_t samples{}; };

    using Any = std::variant<Restart, Latency>;
};

template<typename T>
concept Interface = requires(T t) {
    // Two entry points for state, split by what they cost rather than by depth:
    //
    //   sample rate / configuration   configure(cfg)      allocates, off the audio thread
    //   everything else               reset(Reset::Any)   never allocates
    //
    // `reset` is always individually invocable — it never needs a `configure` first. The usual
    // shape is one private `_adopt(params)` that both `configure` and `Restart` call.
    //
    // Resources: size and allocate for this sample rate *and these parameter values*.
    // Off the audio thread. Named `configure` rather than `reset` because it adopts a
    // state rather than returning to an initial one.
    //
    // `configure` is *sufficient on its own* — it implies a `Restart`. On
    // return the processor renders from exactly this configuration with defined output.
    //
    // **The state handed in is the truth, not a request.** `latency_samps()` is final for
    // this configuration and the framework reports it to the host directly, so a processor
    // must come up already in whatever `params` implies: no negotiation, no glide up from
    // defaults on block 1, no dip or cross-fade into it. Concretely, leave
    // `Context::propose_latency` disengaged on every block until a *live* parameter
    // change moves a structural parameter — re-proposing your own configured latency makes
    // the host renegotiate delay compensation at every reset.
    //
    // Two further obligations, easy to leave unstated and expensive to discover late:
    // `configure` must be **deterministic in `(sr, params)`** — AAX default-constructs the
    // processor at every reset, so anything not reconstructible from those two is gone —
    // and it must be **re-entrant**, since VST3 reconfigures a live object as a routine
    // path.
    { t.configure(std::declval<const Config&>(/*config*/)) } -> std::same_as<void>;

    // Block-boundary commands — see `Reset` above for what each alternative owes.
    // Never allocates. Safe on a never-configured processor: a host that resets before it
    // initializes (validators do) must find a no-op, not undefined behaviour.
    { t.reset(std::declval<const Reset::Any&>(/*reset*/)) } -> std::same_as<void>;

    { t.handle(std::declval<const Event::Param&>(/*event*/)) } -> std::same_as<void>;
    { t.process(std::declval<Context&>(/*context*/)) } -> std::same_as<void>;
    { t.latency_samps() } -> std::same_as<std::uint32_t>;
    { t.tail_samps() } -> std::same_as<std::uint32_t>;
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
auto max_latency_of(const P& processor) -> std::uint32_t
{
    if constexpr (requires { processor.max_latency_samps(); }) {
        // Detected by name, so a std::size_t or int return is used rather than silently ignored.
        static_assert(std::is_integral_v<decltype(processor.max_latency_samps())>, "max_latency_samps() must return a sample count.");
        return std::max(static_cast<std::uint32_t>(processor.max_latency_samps()), processor.latency_samps());
    }
    else {
        return processor.latency_samps();
    }
}

// Hand one delivered event to the processor's matching `handle`.
template<typename P>
auto deliver(P& processor, const Event::Any& event) -> void
{
    std::visit([&](const auto& e) {
        if constexpr (requires { processor.handle(e); }) processor.handle(e);
    }, event);
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