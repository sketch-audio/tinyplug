// The params value functions and their text as properties over a spread of parameter shapes: endpoints,
// round trips between the three spaces, monotonic knob curves, step grids, and text that
// re-parses to the same text (what CLAP's validator checks, here for every format).

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tiny_core/param_text.hpp>
#include <tiny_core/value_helper.hpp>

namespace {

using audio_bench::Error;
using audio_bench::Tests;
using audio_bench::expect_close;
using audio_bench::expect_true;
using namespace tiny::params;
namespace VH = tiny::params;

struct Case {
    std::string name{};
    Semantics::Any semantics{};
};

auto real(double lo, double def, double hi, Display::Numeric display, Adapter::Any adapter) -> Semantics::Any
{
    return Semantics::Real{.min_val = lo, .def_val = def, .max_val = hi, .display = std::move(display), .knob_adapter = std::move(adapter)};
}

auto continuous_cases() -> std::vector<Case>
{
    return {
        {"Real lin dB", real(-60, 0, 12, Display::Decibels{}, Adapter::Lin{})},
        {"Real log Hz", real(20, 1000, 20000, Display::Hertz{}, Adapter::Log{})},
        {"Real pow 4 linear gain", real(0, 1, 4, Display::Gain{}, Adapter::Pow{.exp = 4})},
        {"Real pow 0.5 ms", real(0, 10, 500, Display::Milliseconds{}, Adapter::Pow{.exp = 0.5})},
        {"Real taper 0.25 %", real(0, 25, 100, Display::Percent{}, Adapter::Taper{.taper = 0.25})},
        {"Real taper bipolar degrees", real(-180, 0, 180, Display::Degrees{}, Adapter::Taper{.taper = 0.2, .bipolar = true})},
        {"Real piecewise Hz", real(20, 1000, 20000, Display::Hertz{}, Adapter::Piece{{{.plain = 200, .norm = 0.3}, {.plain = 2000, .norm = 0.7}}})},
        {"Real lin generic", real(-1, 0, 1, Display::Default{}, Adapter::Lin{})},
    };
}

auto discrete_cases() -> std::vector<Case>
{
    return {
        {"Bool", Semantics::Bool{.def_val = false}},
        {"List of 3", Semantics::List{.items = {"Low", "Mid", "High"}, .def_val = 1}},
        {"List of 1", Semantics::List{.items = {"Only"}, .def_val = 0}},
        {"Int -3..7", Semantics::Int{.min_val = -3, .def_val = 0, .max_val = 7}},
        {"Int 0..127 Hz", Semantics::Int{.min_val = 0, .def_val = 64, .max_val = 127, .display = Display::Hertz{}}},
        {"Fixed 0..10 by 0.5 dB", Semantics::Fixed{.min_val = 0, .def_val = 5, .max_val = 10, .step_size = 0.5, .display = Display::Decibels{}}},
        {"Fixed -1..1 by 0.1", Semantics::Fixed{.min_val = -1, .def_val = 0, .max_val = 1, .step_size = 0.1}},
    };
}

auto plain_range(const Semantics::Any& s) -> std::pair<double, double>
{
    const auto spec = Spec{.semantics = s};
    return {VH::plain_min(spec), VH::plain_max(spec)};
}

// Every value a discrete parameter can hold, in plain space.
auto steps_of(const Semantics::Any& s) -> std::vector<double>
{
    auto out = std::vector<double>{};
    std::visit([&](const auto& x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Semantics::Bool>) out = {0, 1};
        else if constexpr (std::is_same_v<T, Semantics::List>) for (auto i = size_t{}; i < x.items.size(); ++i) out.push_back(static_cast<double>(i));
        else if constexpr (std::is_same_v<T, Semantics::Int>) for (auto v = x.min_val; v <= x.max_val; ++v) out.push_back(v);
        else if constexpr (std::is_same_v<T, Semantics::Fixed>) {
            const auto n = static_cast<int>(std::lround((x.max_val - x.min_val) / x.step_size));
            for (auto i = 0; i <= n; ++i) out.push_back(x.min_val + i * x.step_size);
        }
    }, s);
    return out;
}

constexpr auto tight = Error::Absolute{1e-9};

// MARK: - every shape

auto add_common(const Case& c) -> void
{
    Tests::add(std::format("{}: knob 0 and 1 land exactly on min and max", c.name), [c] {
        const auto [lo, hi] = plain_range(c.semantics);
        expect_true(VH::knob_to_plain(0, c.semantics) == lo, std::format("knob 0 -> {}, min is {}", VH::knob_to_plain(0, c.semantics), lo));
        expect_true(VH::knob_to_plain(1, c.semantics) == hi, std::format("knob 1 -> {:.17g}, max is {}", VH::knob_to_plain(1, c.semantics), hi));
        expect_close(VH::plain_to_knob(lo, c.semantics), 0., tight, "min -> knob 0");
        if (hi > lo) expect_close(VH::plain_to_knob(hi, c.semantics), 1., tight, "max -> knob 1"); // One value: it is knob 0.
    });

    Tests::add(std::format("{}: knob to plain never runs backwards", c.name), [c] {
        auto last = VH::knob_to_plain(0, c.semantics);
        for (auto i = 1; i <= 1000; ++i) {
            const auto plain = VH::knob_to_plain(i / 1000., c.semantics);
            expect_true(plain >= last, std::format("knob {} -> {} after {}", i / 1000., plain, last));
            last = plain;
        }
    });

    Tests::add(std::format("{}: convert round-trips between every pair of spaces", c.name), [c] {
        for (auto i = 0; i <= 64; ++i) {
            const auto knob = VH::plain_to_knob(VH::knob_to_plain(i / 64., c.semantics), c.semantics); // A value the parameter can hold.
            for (const auto from : {Space::Plain, Space::Host, Space::Knob}) {
                const auto value = VH::convert(knob, Space::Knob, from, c.semantics);
                for (const auto to : {Space::Plain, Space::Host, Space::Knob}) {
                    const auto there = VH::convert(value, from, to, c.semantics);
                    const auto back = VH::convert(there, to, from, c.semantics);
                    expect_close(back, value, Error::Relative{1e-9}, std::format("knob {} space {} -> {} -> back", knob, static_cast<int>(from), static_cast<int>(to)));
                }
            }
        }
    });

    Tests::add(std::format("{}: the default agrees across spaces", c.name), [c] {
        const auto spec = Spec{.semantics = c.semantics};
        const auto plain = VH::default_value(spec, Space::Plain);
        expect_close(VH::default_value(spec, Space::Knob), VH::plain_to_knob(plain, c.semantics), tight, "knob default");
        expect_close(VH::default_value(spec, Space::Host), VH::plain_to_host(plain, c.semantics), tight, "host default");
    });

    Tests::add(std::format("{}: clamp holds the plain range", c.name), [c] {
        const auto [lo, hi] = plain_range(c.semantics);
        expect_true(VH::clamp(lo - 1000, c.semantics) == lo && VH::clamp(hi + 1000, c.semantics) == hi, "clamp");
    });

    // Host text must survive text -> value -> text, the way every wrapper parses it back.
    Tests::add(std::format("{}: displayed text re-parses to the same text", c.name), [c] {
        for (auto i = 0; i <= 200; ++i) {
            const auto host = VH::knob_to_host(VH::plain_to_knob(VH::knob_to_plain(i / 200., c.semantics), c.semantics), c.semantics);
            const auto text = VH::to_text(VH::host_to_plain(host, c.semantics), c.semantics);
            const auto plain = VH::from_text(text, c.semantics);
            expect_true(plain.has_value(), std::format("'{}' didn't parse", text));
            const auto again = VH::to_text(VH::clamp(*plain, c.semantics), c.semantics);
            expect_true(again == text, std::format("'{}' re-parsed as '{}'", text, again));
        }
    });
}

// MARK: - discrete only

auto add_discrete(const Case& c) -> void
{
    Tests::add(std::format("{}: every step round-trips exactly, host space is plain", c.name), [c] {
        for (const auto plain : steps_of(c.semantics)) {
            const auto knob = VH::plain_to_knob(plain, c.semantics);
            expect_close(VH::knob_to_plain(knob, c.semantics), plain, tight, std::format("step {} via knob {}", plain, knob));
            expect_close(VH::plain_to_host(plain, c.semantics), plain, tight, std::format("step {} host", plain));
        }
    });

    Tests::add(std::format("{}: any knob lands on a step, and quantize is idempotent", c.name), [c] {
        const auto steps = steps_of(c.semantics);
        for (auto i = 0; i <= 1000; ++i) {
            const auto plain = VH::knob_to_plain(i / 1000., c.semantics);
            auto on_grid = false;
            for (const auto s : steps) on_grid = on_grid || std::abs(s - plain) < 1e-9;
            expect_true(on_grid, std::format("knob {} -> {} is off the grid", i / 1000., plain));
            expect_close(VH::quantize(VH::quantize(plain, c.semantics), c.semantics), VH::quantize(plain, c.semantics), tight, "quantize twice");
        }
    });

    Tests::add(std::format("{}: knob_next and knob_prev visit every step and wrap", c.name), [c] {
        const auto n = steps_of(c.semantics).size();
        auto knob = 0.;
        for (auto i = size_t{}; i < n; ++i) knob = VH::knob_next(knob, c.semantics);
        expect_close(knob, 0., tight, "n nexts from 0 wraps back to 0");
        for (auto i = size_t{}; i < n; ++i) knob = VH::knob_prev(knob, c.semantics);
        expect_close(knob, 0., tight, "n prevs from 0 wraps back to 0");
    });
}

// MARK: - specific text

auto add_text() -> void
{
    Tests::add("text: bools, lists, and Gain's -inf", [] {
        const auto b = Semantics::Any{Semantics::Bool{}};
        expect_true(VH::to_text(1, b) == "True" && VH::from_text("False", b) == 0., "bool");
        const auto l = Semantics::Any{Semantics::List{.items = {"Low", "Mid", "High"}}};
        expect_true(VH::to_text(2, l) == "High" && VH::from_text("Mid", l) == 1., "list");
        expect_true(!VH::from_text("Nope", l).has_value(), "an unknown list item must not parse");
        const auto g = real(0, 1, 4, Display::Gain{}, Adapter::Pow{.exp = 4});
        expect_true(VH::to_text(0, g) == "-inf dB", "0 is -inf dB");
        expect_true(VH::from_text("-INF", g) == 0. && VH::from_text("-inf dB", g) == 0., "-inf parses to 0");
        expect_true(VH::to_text(1, g) == "+0.0 dB", "unity");
        expect_close(*VH::from_text("-6", g), std::pow(10., -6. / 20.), tight, "typed dB");
    });

    Tests::add("text: Hz switches to kHz and reads it back", [] {
        const auto hz = real(20, 1000, 20000, Display::Hertz{}, Adapter::Log{});
        expect_true(VH::to_text(1500, hz) == "1.5 kHz", "1500 Hz");
        expect_close(*VH::from_text("1.5 kHz", hz), 1500., tight, "kHz parse");
        expect_true(VH::to_text(440, hz) == "440 Hz", "440 Hz");
    });

    Tests::add("text: Bool labels", [] {
        const auto b = Semantics::Any{Semantics::Bool{.display = Display::Labels{}}};
        expect_true(VH::to_text(1, b) == "On" && VH::to_text(0, b) == "Off", "labels");
        expect_true(VH::from_text("Off", b) == 0. && !VH::from_text("True", b).has_value(), "parse labels only");
    });

    Tests::add("text: Custom, and its fallbacks", [] {
        const auto ratio = real(1, 4, 20, Display::Custom{
            .to_text = [](double plain) { return std::format("{:.1f}:1", plain); },
            .from_text = [](std::string_view text) -> std::optional<double> { return text == "4.0:1" ? std::optional{4.} : std::nullopt; },
        }, Adapter::Lin{});
        expect_true(VH::to_text(4, ratio) == "4.0:1", "to_text");
        expect_true(VH::from_text("4.0:1", ratio) == 4. && !VH::from_text("4", ratio).has_value(), "from_text");

        const auto empty = real(0, 0.5, 1, Display::Custom{}, Adapter::Lin{});
        expect_true(VH::to_text(0.5, empty) == "0.50", "empty to_text is the plain number");
        expect_true(VH::from_text("0.25", empty) == 0.25, "empty from_text parses the number");
    });

    Tests::add("text: Int takes the unit's suffix", [] {
        const auto i = Semantics::Any{Semantics::Int{.min_val = 0, .def_val = 64, .max_val = 127, .display = Display::Hertz{}}};
        expect_true(VH::to_text(64, i) == "64 Hz", "suffix");
    });

    Tests::add("displays_as", [] {
        const auto db = real(-60, 0, 12, Display::Decibels{}, Adapter::Lin{});
        expect_true(VH::displays_as<Display::Decibels>(db) && !VH::displays_as<Display::Hertz>(db), "Real");
        expect_true(!VH::displays_as<Display::Labels>(db), "an alternative the semantics can't hold");
        expect_true(!VH::displays_as<Display::Default>(Semantics::List{}), "List has no display");
    });
}

} // namespace

auto main() -> int
{
    for (const auto& c : continuous_cases()) add_common(c);
    for (const auto& c : discrete_cases()) {
        add_common(c);
        add_discrete(c);
    }
    add_text();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
