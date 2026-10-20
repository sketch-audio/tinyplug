// process::detail::Timeline_resolver: what a wrapper hands the processor when the host leaves timeline fields out.
#include <cmath>
#include <cstdio>
#include <limits>

#include <tinyplug/tiny_processor.hpp>

namespace {

using namespace tiny::process;
using detail::Timeline_resolver;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

auto near(double a, double b) -> bool { return std::abs(a - b) < 1e-9; }

constexpr auto sr = 48000.;
constexpr auto moving = Timeline::State{.moving = true};

auto test_pass_through() -> void
{
    std::printf("pass through\n");

    auto r = Timeline_resolver{};
    const auto t = r.resolve({.beat_pos = 4., .tempo = 90., .signature = Timeline::Signature{3, 4},
                              .cycle = Timeline::Cycle{0., 8.}, .state = Timeline::State{.moving = true, .cycling = true}},
                             96000, 512, sr);
    expect(t.sample_pos == 96000 && near(*t.beat_pos, 4.), "a full report passes through");
    expect(near(*t.tempo, 90.) && t.signature->numer == 3 && t.signature->denom == 4, "tempo and signature pass through");
    expect(near(t.cycle->end, 8.) && t.state->moving && t.state->cycling, "cycle and state pass through");

    const auto inactive = r.resolve({.cycle = Timeline::Cycle{2., 6.}, .state = moving}, 0, 512, sr);
    expect(inactive.cycle && !inactive.state->cycling, "an inactive cycle region is still reported");

    const auto neg = r.resolve({.state = moving}, -4800, 512, sr);
    expect(neg.sample_pos == -4800 && neg.state->moving, "negative (pre-roll) positions pass through");
}

auto test_missing() -> void
{
    std::printf("missing\n");

    auto r = Timeline_resolver{};
    const auto a = r.resolve({}, std::nullopt, 512, sr);
    const auto b = r.resolve({}, std::nullopt, 256, sr);
    const auto c = r.resolve({}, std::nullopt, 128, sr);
    expect(a.sample_pos == 0 && b.sample_pos == 512 && c.sample_pos == 768, "no host info free-runs");
    expect(!c.beat_pos && !c.tempo && !c.signature && !c.cycle && !c.state, "and nothing else is invented");

    auto s = Timeline_resolver{};
    (void)s.resolve({.state = moving}, 10000, 512, sr);
    const auto lost = s.resolve({.state = moving}, std::nullopt, 512, sr);
    expect(lost.sample_pos == 10512, "losing the position continues from the host's last one");
    expect(lost.state && !lost.state->moving, "and stops");

    const auto beats_only = Timeline_resolver{}.resolve({.beat_pos = 4., .tempo = 120., .state = moving}, std::nullopt, 512, sr);
    expect(beats_only.sample_pos == 96000 && beats_only.state->moving, "missing position derives from host beats and tempo");

    const auto no_tempo = Timeline_resolver{}.resolve({.beat_pos = 4., .state = moving}, std::nullopt, 512, sr);
    expect(no_tempo.sample_pos == 0 && !no_tempo.state->moving, "beats without a tempo don't make a position");
    expect(near(*no_tempo.beat_pos, 4.), "but the beats still pass through");

    const auto pos_only = Timeline_resolver{}.resolve({.tempo = 120., .state = moving}, 48000, 512, sr);
    expect(!pos_only.beat_pos, "missing beats stay missing");
}

auto test_invalid() -> void
{
    std::printf("invalid\n");

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    auto r = Timeline_resolver{};
    expect(!r.resolve({.tempo = 0.}, 0, 1, sr).tempo, "tempo 0 is missing");
    expect(!r.resolve({.tempo = -5.}, 0, 1, sr).tempo, "negative tempo is missing");
    expect(!r.resolve({.tempo = nan}, 0, 1, sr).tempo, "NaN tempo is missing");
    expect(!r.resolve({.beat_pos = inf}, 0, 1, sr).beat_pos, "non-finite beats are missing");
    expect(!r.resolve({.signature = Timeline::Signature{4, 0}}, 0, 1, sr).signature, "a zero denominator is missing");

    const auto cyc = r.resolve({.cycle = Timeline::Cycle{0., nan}, .state = Timeline::State{.cycling = true}}, 0, 1, sr);
    expect(!cyc.cycle && !cyc.state->cycling, "a broken cycle isn't cycling");

    expect(!Timeline_resolver::samples(nan) && *Timeline_resolver::samples(127.6) == 128, "samples() rounds, drops non-finite");
}

auto test_offset() -> void
{
    std::printf("offset\n");

    const auto t = Timeline_resolver{}.resolve({.beat_pos = 1., .tempo = 60.}, 1000, 512, sr);
    const auto sub = Timeline_resolver::at_offset(t, 480, sr);
    expect(sub.sample_pos == 1480 && near(*sub.beat_pos, 1.01), "at_offset advances position and beats");

    const auto no_tempo = Timeline_resolver{}.resolve({.beat_pos = 1.}, 1000, 512, sr);
    expect(Timeline_resolver::at_offset(no_tempo, 0, sr).beat_pos.has_value(), "beats without a tempo hold at offset 0");
    expect(!Timeline_resolver::at_offset(no_tempo, 480, sr).beat_pos, "and drop further in");
}

} // namespace

auto main() -> int
{
    test_pass_through();
    test_missing();
    test_invalid();
    test_offset();
    std::printf("%s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
