// process::Timeline_resolver: what a wrapper reports when the host leaves timeline fields out.
#include <tinyplug/tiny_processor.hpp>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace tiny::process;
using Report = Timeline_resolver::Report;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static bool near(double a, double b) { return std::abs(a - b) < 1e-9; }

constexpr auto sr = 48000.0;

int main() {
    std::printf("Timeline_resolver\n");

    {
        auto t = Timeline_resolver{};
        const auto r = Report{.sample_pos = 96000, .beat_pos = 4.0, .tempo = 90.0, .time_sig = Time_sig{3, 4},
                              .cycle_start = 0.0, .cycle_end = 8.0, .transport_state = {.moving = true, .cycling = true}};
        const auto mc = t.resolve(r, 512, sr);
        check(mc.sample_pos == 96000 && near(mc.beat_pos, 4.0), "a full report passes through");
        check(near(mc.tempo_ideal, 90) && near(mc.tempo_real, 90), "tempo passes through, real == ideal");
        check(mc.time_sig.numer == 3 && mc.transport_state.moving && mc.transport_state.cycling, "time sig and transport pass through");
    }
    {
        auto t = Timeline_resolver{};
        auto a = t.resolve(Report{}, 512, sr);
        auto b = t.resolve(Report{}, 256, sr);
        auto c = t.resolve(Report{}, 128, sr);
        check(a.sample_pos == 0 && b.sample_pos == 512 && c.sample_pos == 768, "no host info free-runs");
        check(!a.transport_state.moving, "no position means not moving");
        check(near(a.tempo_ideal, 120) && a.time_sig.numer == 4 && a.time_sig.denom == 4, "defaults: 120 bpm, 4/4");
        check(near(c.beat_pos, 768 / sr * 2), "free-running beats follow the position");
    }
    {
        auto t = Timeline_resolver{};
        (void)t.resolve(Report{.sample_pos = 10000, .transport_state = {.moving = true}}, 512, sr);
        const auto mc = t.resolve(Report{.transport_state = {.moving = true}}, 512, sr);
        check(mc.sample_pos == 10512, "losing the position continues from the host's last one");
        check(!mc.transport_state.moving, "and stops");
    }
    {
        auto t = Timeline_resolver{};
        const auto mc = t.resolve(Report{.sample_pos = 48000, .tempo = 120.0, .transport_state = {.moving = true}}, 512, sr);
        check(near(mc.beat_pos, 2.0) && mc.transport_state.moving, "missing beats derive from the position (moving)");
    }
    {
        auto t = Timeline_resolver{};
        const auto mc = t.resolve(Report{.beat_pos = 4.0, .tempo = 120.0, .transport_state = {.moving = true}}, 512, sr);
        check(mc.sample_pos == 96000 && mc.transport_state.moving, "missing position derives from host beats and tempo");
    }
    {
        auto t = Timeline_resolver{};
        const auto mc = t.resolve(Report{.beat_pos = 4.0, .transport_state = {.moving = true}}, 512, sr);
        check(mc.sample_pos == 0 && !mc.transport_state.moving, "beats without a host tempo don't make a position");
        check(near(mc.beat_pos, 4.0), "but the beats still pass through");
    }
    {
        auto t = Timeline_resolver{};
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const auto inf = std::numeric_limits<double>::infinity();
        check(near(t.resolve(Report{.sample_pos = 0, .tempo = 0.0}, 1, sr).tempo_ideal, 120), "tempo 0 is missing");
        check(near(t.resolve(Report{.sample_pos = 0, .tempo = -5.0}, 1, sr).tempo_ideal, 120), "negative tempo is missing");
        check(near(t.resolve(Report{.sample_pos = 0, .tempo = nan}, 1, sr).tempo_ideal, 120), "NaN tempo is missing");
        check(near(t.resolve(Report{.sample_pos = 48000, .beat_pos = inf}, 1, sr).beat_pos, 2.0), "non-finite beats are missing");
        const auto sig = t.resolve(Report{.sample_pos = 0, .time_sig = Time_sig{4, 0}}, 1, sr).time_sig;
        check(sig.numer == 4 && sig.denom == 4, "a zero denominator is missing");
        const auto cyc = t.resolve(Report{.sample_pos = 0, .cycle_start = 0.0, .transport_state = {.cycling = true}}, 1, sr);
        check(!cyc.transport_state.cycling, "a half-reported cycle isn't cycling");
        check(!Timeline_resolver::samples(nan) && *Timeline_resolver::samples(127.6) == 128, "samples() rounds, drops non-finite");
    }
    {
        auto t = Timeline_resolver{};
        const auto mc = t.resolve(Report{.sample_pos = -4800, .tempo = 120.0, .transport_state = {.moving = true}}, 512, sr);
        check(mc.sample_pos == -4800 && near(mc.beat_pos, -0.2), "negative (pre-roll) positions pass through");
    }
    {
        auto t = Timeline_resolver{};
        const auto mc = t.resolve(Report{.sample_pos = 1000, .beat_pos = 1.0, .tempo = 60.0}, 512, sr);
        const auto sub = Timeline_resolver::at_offset(mc, 480, sr);
        check(sub.sample_pos == 1480 && near(sub.beat_pos, 1.01), "at_offset advances position and beats");
    }

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
