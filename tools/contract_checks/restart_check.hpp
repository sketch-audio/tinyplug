#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <tinyplug/tiny_process.hpp>

namespace tiny::process {

// The `Restart` contract, checked on a processor alone: after any history, `Restart{b}` renders
// exactly what a processor freshly configured with `b` renders. A wrapper test can't see a product's
// own state that survives a restart (an oscillator's phase, a one-shot that fires twice). Part of the
// contract checks tool; not for the audio thread.
struct Restart_check {
    double sr{48000};
    std::size_t history{48000};     // Frames of noise and random automation before the restart.
    std::size_t frames{9600};       // Frames compared after it.
    std::size_t block{256};
    std::int64_t position{96000};   // The timeline position of the first compared frame.
    std::uint32_t seed{1};
};

// A processor driven the way a wrapper drives it, with nothing else: deterministic noise in,
// events at block starts, a moving timeline, and somewhere for meters, blocks, state and notes to go.
template<typename P>
class Bench {
public:

    static constexpr auto channels = std::size_t{2};
    static constexpr auto num_params = User_params::num_params;

    Bench(double sr, std::size_t max_block) : _sr{sr}, _processor{std::make_unique<P>()}
    {
        for (auto& c : _in) c.resize(max_block);
        for (auto& c : _side) c.resize(max_block);
        for (auto& c : _out) c.resize(max_block);
    }

    auto processor() -> P& { return *_processor; }
    auto configure(std::span<const double> params) -> void { _processor->configure(Config{.sr = _sr, .params = params}); }
    auto restart(std::span<const double> params) -> void { _processor->reset(Reset::Restart{params}); }
    auto handle(const Event::Param& event) -> void { _processor->handle(event); }

    // Renders `frames` at `position`, the input noise drawn from `noise`. Appends channel 0's output.
    auto render(std::size_t frames, std::int64_t position, std::uint32_t& noise, std::vector<float>& out) -> std::optional<std::uint32_t>
    {
        for (auto c = std::size_t{}; c < channels; ++c) {
            for (auto f = std::size_t{}; f < frames; ++f) {
                noise = noise * 1664525u + 1013904223u;
                _in[c][f] = static_cast<float>(noise >> 8) / static_cast<float>(1u << 24) - 0.5f;
                _side[c][f] = _in[c][f] * 0.5f;
                _out[c][f] = 0.f;
            }
            _ins[c] = _in[c].data();
            _sides[c] = _side[c].data();
            _outs[c] = _out[c].data();
        }

        auto context = Context{};
#if TINY_HAS_STATE
        auto state_block = _state.begin_block();
        context.state = state::Access_for<models::Resolved::State>{&state_block};
#endif
#if TINY_HAS_METERS
        context.meters = _meters.scratch();
#endif
#if TINY_HAS_BLOCKS
        context.blocks = blocks::Writer{&_blocks};
#endif
        context.mode = Mode::Offline;
        context.timeline = Timeline{
            .sample_pos = position,
            .beat_pos = static_cast<double>(position) * 2. / _sr,
            .tempo = 120.,
            .signature = Timeline::Signature{4, 4},
            .state = Timeline::State{.moving = true},
        };
        context.inputs = {_ins.data(), channels};
        context.sidechain = {_sides.data(), channels};
        context.outputs = {_outs.data(), channels};
        context.num_frames = frames;
        _processor->process(context);
        out.insert(out.end(), _out[0].begin(), _out[0].begin() + static_cast<std::ptrdiff_t>(frames));
        return context.propose_latency;
    }

private:

    double _sr{};
    std::unique_ptr<P> _processor{};
    std::array<std::vector<float>, channels> _in{}, _side{}, _out{};
    std::array<const float*, channels> _ins{}, _sides{};
    std::array<float*, channels> _outs{};
#if TINY_HAS_STATE
    state::Processor_for<models::Resolved::State> _state{};
#endif
#if TINY_HAS_METERS
    meters::Publisher<models::Resolved::Meters> _meters{};
#endif
#if TINY_HAS_BLOCKS
    blocks::Publisher<models::Resolved::Blocks> _blocks{};
#endif
};

// Empty when the contract holds for `a` → any history → `Restart{b}`; otherwise what broke.
// Also checks that a second `Restart{b}` changes nothing. When `b` implies another latency than the
// one the processor holds, the outputs can't match (the host owns latency), so instead the first
// block after the restart must propose it.
template<typename P>
auto check_restart(std::span<const double> a, std::span<const double> b, const Restart_check& check = {}) -> std::optional<std::string>
{
    using params::Value_helper;
    constexpr auto num_params = User_params::num_params;

    auto random = check.seed;
    const auto next = [&random] { random = random * 22695477u + 1u; return random >> 8; };
    const auto unit = [&] { return static_cast<double>(next() % 10001u) / 10000.; };

    // Configure with `a`, then a history of noise and automation.
    const auto lived = [&](Bench<P>& bench) {
        bench.configure(a);
        auto noise = check.seed * 7u + 3u;
        auto scratch = std::vector<float>{};
        auto position = check.position - static_cast<std::int64_t>(check.history) - 48000;
        random = check.seed;
        for (auto done = std::size_t{}; done < check.history; done += check.block) {
            if constexpr (num_params > 0) {
                if (next() % 4 == 0) {
                    const auto address = static_cast<std::uint32_t>(next() % num_params);
                    const auto value = Value_helper::knob_to_plain(unit(), User_params::param_spec(address).semantics);
                    if (next() % 2 == 0) bench.handle(Event::Set{.value = value, .address = address});
                    else bench.handle(Event::Ramp{.value = value, .address = address, .samples = static_cast<std::int32_t>(check.block)});
                }
            }
            (void)bench.render(check.block, position, noise, scratch);
            position += static_cast<std::int64_t>(check.block);
        }
    };

    // The same input after the restart as after a fresh configure.
    const auto play = [&](Bench<P>& bench, std::optional<std::uint32_t>* first_proposal) {
        auto noise = check.seed * 31u + 17u;
        auto out = std::vector<float>{};
        for (auto done = std::size_t{}; done < check.frames; done += check.block) {
            const auto frames = std::min(check.block, check.frames - done);
            const auto proposal = bench.render(frames, check.position + static_cast<std::int64_t>(done), noise, out);
            if (first_proposal && done == 0) *first_proposal = proposal;
        }
        return out;
    };

    const auto first_difference = [](const std::vector<float>& x, const std::vector<float>& y) -> std::optional<std::string> {
        for (auto i = std::size_t{}; i < std::min(x.size(), y.size()); ++i) {
            if (x[i] != y[i]) return std::format("sample {}: {} vs {}", i, x[i], y[i]);
        }
        return std::nullopt;
    };

    auto fresh = Bench<P>{check.sr, check.block};
    fresh.configure(b);
    const auto expected_latency = fresh.processor().latency_samps();
    const auto expected = play(fresh, nullptr);

    auto restarted = Bench<P>{check.sr, check.block};
    lived(restarted);
    const auto held_latency = restarted.processor().latency_samps();
    restarted.restart(b);
    if (restarted.processor().latency_samps() != held_latency) {
        return std::format("Restart changed latency_samps() from {} to {}: it must keep the accepted latency and propose", held_latency, restarted.processor().latency_samps());
    }
    auto proposal = std::optional<std::uint32_t>{};
    const auto actual = play(restarted, &proposal);
    if (held_latency != expected_latency) {
        if (proposal != expected_latency) return std::format("after a Restart whose values imply latency {}, the next block must propose it (held {})", expected_latency, held_latency);
        return std::nullopt;
    }
    if (const auto diff = first_difference(actual, expected)) return "after a history, Restart{b} differs from configure(b): " + *diff;

    auto twice = Bench<P>{check.sr, check.block};
    lived(twice);
    twice.restart(b);
    twice.restart(b);
    if (const auto diff = first_difference(play(twice, nullptr), expected)) return "a second Restart{b} changed the output: " + *diff;
    return std::nullopt;
}

} // namespace tiny::process
