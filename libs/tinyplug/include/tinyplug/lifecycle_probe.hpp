#pragma once

// A fixed vocabulary for the processor lifecycle and the latency handshake, layered on
// tiny::log.
//
// The point is that all five wrappers say the *same* words for the same events. A Pro
// Tools trace and a Logic trace of the same plug-in then differ only where the formats
// genuinely differ, which is what makes the ordering questions in
// plans/processor-lifecycle.md answerable by reading two logs side by side.
//
// Every method compiles to nothing unless TINY_LOG_ENABLED. Construct one per wrapper
// object, off the audio thread — the constructor is what initializes the logging layer,
// which is how the audio thread is kept out of the one-time setup.

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "tiny_log.hpp"

namespace tiny::log {

namespace detail {

inline auto next_instance() noexcept -> uint16_t
{
    static auto counter = std::atomic<uint16_t>{};
    return static_cast<uint16_t>(counter.fetch_add(1, std::memory_order_relaxed) + 1);
}

// Swallows a probe method's arguments in a build with logging off, so the parameters
// still count as used without anything reaching the optimizer.
template<typename... Args>
constexpr auto ignore(const Args&...) noexcept -> void
{
}

} // namespace detail

// Every probe line is emitted with a null site: the tag already says which wrapper, and
// the verb already says which event, so a file:line would only add noise.
#if TINY_LOG_ENABLED
    #define TINY_PROBE_WRITE(cat_, level_, fmt_, ...) \
        ::tiny::log::write(::tiny::log::Cat::cat_, ::tiny::log::Level::level_, nullptr, \
                           _tag, _instance, fmt_ __VA_OPT__(,) __VA_ARGS__)
#else
    #define TINY_PROBE_WRITE(cat_, level_, fmt_, ...) \
        ::tiny::log::detail::ignore(fmt_ __VA_OPT__(,) __VA_ARGS__)
#endif

class Probe {
public:

    // `tag` names the emitter and must outlive the probe — "VST3/proc", "AAX/alg",
    // "AUv3/kernel". Instances are numbered per process so a session with several
    // plug-in instances stays readable.
#if TINY_LOG_ENABLED
    explicit Probe(const char* tag) noexcept
        : _tag{tag}
        , _instance{detail::next_instance()}
    {
        init(); // Off the audio thread, so the audio thread never runs the one-time setup.

        // All five formats instantiate a plug-in on the host's main/message thread, so
        // claim that role here unless the thread already has one. It saves tagging every
        // main-thread entry point by hand; the audio thread is tagged explicitly in
        // `process` and overrides nothing, because it is a different thread.
        if (detail::this_thread_role() == Thread_role::unknown) {
            set_thread_role(Thread_role::main);
        }

        TINY_PROBE_WRITE(lifecycle, info, "instance created");
    }
#else
    explicit Probe(const char*) noexcept {}
#endif

    ~Probe()
    {
        TINY_PROBE_WRITE(lifecycle, info, "instance destroyed");
    }

    Probe(const Probe&) = delete;
    auto operator=(const Probe&) -> Probe& = delete;

    auto tag() const noexcept -> const char* { return _tag; }
    auto instance() const noexcept -> uint16_t { return _instance; }

    // MARK: - configuration

    // After `configure` returned, with the latency it settled on. `configure` is a fact,
    // not a proposal, so this line is the authoritative latency for the configuration.
    auto configured(double sr, size_t num_params, uint32_t latency) const noexcept -> void
    {
        TINY_PROBE_WRITE(lifecycle, info, "configure sr={} params={} -> latency={}", sr, num_params, latency);
    }

    // The host asked for the plug-in to go live or idle.
    auto activated(bool active, double sr) const noexcept -> void
    {
        TINY_PROBE_WRITE(lifecycle, info, "{} sr={}", active ? "activate" : "deactivate", sr);
    }

    // Discontinuity handling. `why` distinguishes the several sites per wrapper —
    // "seek", "render-mode", "resync", "host-clear".
    auto cleared(const char* why) const noexcept -> void
    {
        TINY_PROBE_WRITE(lifecycle, debug, "clear ({})", why);
    }
    auto snapped(const char* why) const noexcept -> void
    {
        TINY_PROBE_WRITE(lifecycle, debug, "snap ({})", why);
    }

    auto render_mode_changed(bool offline) const noexcept -> void
    {
        TINY_PROBE_WRITE(lifecycle, info, "render mode -> {}", offline ? "offline" : "realtime");
    }

    // MARK: - latency handshake

    // The kernel proposed during `process`. `reported` is what we last asked the host for.
    auto latency_proposed(uint32_t proposed, uint32_t reported) const noexcept -> void
    {
        TINY_PROBE_WRITE(latency, info, "propose {} (host holds {})", proposed, reported);
    }

    // A proposal we did not act on, and why — the dedupe guard, PDC disabled in AAX, an
    // activation that superseded it. Silent drops are the hardest failure to see.
    auto latency_dropped(uint32_t proposed, const char* why) const noexcept -> void
    {
        TINY_PROBE_WRITE(latency, warn, "propose {} DROPPED ({})", proposed, why);
    }

    // We told the host its number is wrong. `how` is the format-native mechanism:
    // "restartComponent", "clap_host_latency::changed", "PropertyChanged", "KVO",
    // "SetSignalLatency".
    auto latency_notified(const char* how, uint32_t value) const noexcept -> void
    {
        TINY_PROBE_WRITE(latency, info, "notify host value={} via {}", value, how);
    }

    // The host read our latency back. `answered` is what we told it — the clause-5
    // question of whether the getter leads or follows.
    auto latency_queried(const char* how, uint32_t answered) const noexcept -> void
    {
        TINY_PROBE_WRITE(latency, info, "host read {} -> answered {}", how, answered);
    }

    // Handed to the kernel, which must match it by the end of the block.
    auto latency_accepted(uint32_t value) const noexcept -> void
    {
        TINY_PROBE_WRITE(latency, info, "accept {} -> kernel", value);
    }

    // The kernel did not match. This is the assertion in release clothing.
    auto latency_mismatch(uint32_t expected, uint32_t actual) const noexcept -> void
    {
        TINY_PROBE_WRITE(latency, error, "MISMATCH kernel reports {} after accepting {}", actual, expected);
    }

    // MARK: - blocks

    // Trace level, so it is off unless asked for. `accepted` is the value the kernel sees
    // for this block.
    auto block(size_t num_frames, uint32_t accepted, size_t num_events) const noexcept -> void
    {
        TINY_PROBE_WRITE(process, trace, "block frames={} latency={} events={}", num_frames, accepted, num_events);
    }

    // MARK: - state

    auto state(const char* which, size_t bytes) const noexcept -> void
    {
        TINY_PROBE_WRITE(state, info, "state {} bytes={}", which, bytes);
    }

private:

    const char* _tag{};
    uint16_t _instance{};
};

} // namespace tiny::log

// Free-form logging that still carries a probe's tag and instance number.
#if TINY_LOG_ENABLED
    #define TINY_PROBE(probe, cat, level, fmt, ...) \
        ::tiny::log::write(::tiny::log::Cat::cat, ::tiny::log::Level::level, TINY_LOG_SITE, \
                           (probe).tag(), (probe).instance(), fmt __VA_OPT__(,) __VA_ARGS__)
#else
    #define TINY_PROBE(probe, cat, level, fmt, ...) ((void)0)
#endif
