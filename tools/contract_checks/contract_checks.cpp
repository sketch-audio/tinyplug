// A plug-in's contract checks: what the framework requires of a plug-in, checked on its processor and
// models alone, no host. Built per plug-in by `make_contract_checks`; exits non-zero on any failure.
//
//     <Base>_contract_checks [--seed N]
//
// Add a check here, as a function returning what broke; see README.md for the ones planned.

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <optional>
#include <string>

#include <tiny_plugin.hpp>

#include "plug_info.hpp"

#include "restart_check.hpp"

namespace {

using namespace tiny;

constexpr auto num_params = User_params::num_params;
using Values = std::array<double, num_params>;

// Every parameter at one knob position.
auto values_at(double knob) -> Values
{
    auto out = Values{};
    for (auto a = std::uint32_t{}; a < num_params; ++a) out[a] = params::Value_helper::knob_to_plain(knob, User_params::param_spec(a).semantics);
    return out;
}

// A seeded spread of knob positions.
auto values_seeded(std::uint32_t seed) -> Values
{
    auto out = Values{};
    for (auto a = std::uint32_t{}; a < num_params; ++a) {
        seed = seed * 1664525u + 1013904223u;
        const auto knob = static_cast<double>(seed >> 8) / static_cast<double>(1u << 24);
        out[a] = params::Value_helper::knob_to_plain(knob, User_params::param_spec(a).semantics);
    }
    return out;
}

auto failures = 0;

auto check(const std::string& name, const std::optional<std::string>& failure) -> void
{
    if (failure) ++failures;
    std::printf("[%s] %s%s%s\n", failure ? "FAIL" : "pass", name.c_str(), failure ? ": " : "", failure.value_or("").c_str());
}

// After any history, `Restart{b}` renders what `configure(b)` renders, and a second restart changes nothing.
auto check_restart(std::uint32_t seed) -> void
{
    const auto defaults = params::make_defaults<double, User_params>(params::Space::Plain);
    check("restart: defaults, a history, defaults", process::check_restart<User_processor>(defaults, defaults, {.seed = seed}));
    check("restart: every range's low end to its high end", process::check_restart<User_processor>(values_at(0.), values_at(1.), {.seed = seed}));
    check("restart: every range's high end to its low end", process::check_restart<User_processor>(values_at(1.), values_at(0.), {.seed = seed + 7}));
    for (auto n = std::uint32_t{}; n < 4; ++n) {
        const auto s = seed + n;
        check(std::format("restart: seeded values and history {}", s), process::check_restart<User_processor>(values_seeded(s), values_seeded(s + 100), {.seed = s}));
    }
}

} // namespace

auto main(int argc, char** argv) -> int
{
    auto seed = std::uint32_t{1};
    for (auto i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--seed") == 0) seed = static_cast<std::uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
    }
    std::printf("%s contract checks, seed %u\n", Plug_info::plugin_name, seed);
    check_restart(seed);
    std::printf("%s\n", failures == 0 ? "All checks passed." : std::format("{} checks failed.", failures).c_str());
    return failures == 0 ? 0 : 1;
}
