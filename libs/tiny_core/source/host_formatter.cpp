#include "tiny_core/host_formatter.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "tiny_core/value_helper.hpp"

namespace tiny {

auto Host_formatter::to_string(double host_value, const Semantics::Any& semantics) -> std::string
{
    using namespace params;

    const auto plain_value = Value_helper::host_to_plain(host_value, semantics);

    // Rounded as displayed, with no negative zero: "-0.0" would re-parse and print as "+0.0".
    auto shown = [](double value, int precision) {
        const auto scale = std::pow(10.0, precision);
        const auto rounded = std::round(value * scale) / scale;
        return rounded == 0 ? 0.0 : rounded;
    };

    auto format_double = [&](double value, int precision) {
        auto oss = std::ostringstream{};
        oss << std::fixed << std::setprecision(precision) << shown(value, precision);
        return oss.str();
    };

    // "+" by the value as displayed, for the units that show a sign.
    auto signed_text = [&](double value, int precision, const char* suffix) {
        const auto prefix = shown(value, precision) >= 0 ? "+" : "";
        return prefix + format_double(value, precision) + suffix;
    };

    // Pick precision from the value as it will be *displayed*, not as it arrives. Text round-trips
    // through host space are not bit-exact, so a value printing as "10" comes back as 9.99999… —
    // a bare `>= 10` test would then print "10.0" and the host sees an inconsistent conversion.
    auto adaptive_prec = [](double value) {
        return (std::round(std::abs(value) * 10.0) >= 100.0) ? 0 : 1;
    };

    // A Custom without `to_text` falls back to the plain number.
    auto numeric = [&](const Display::Numeric& display, auto&& fallback) -> std::string {
        return std::visit(Inline_visitor{
            [&](const Display::Default&)      { return fallback(); },
            [&](const Display::Percent&)      { return format_double(plain_value, 0) + " %"; },
            [&](const Display::Decibels&)     { return signed_text(plain_value, adaptive_prec(plain_value), " dB"); },
            [&](const Display::Milliseconds&) { return format_double(plain_value, adaptive_prec(plain_value)) + " ms"; },
            [&](const Display::Degrees&)      { return signed_text(plain_value, 0, " °"); },
            [&](const Display::Gain&) {
                if (!(plain_value > 0)) return std::string{"-inf dB"};
                const auto db = 20 * std::log10(plain_value);
                return signed_text(db, adaptive_prec(db), " dB");
            },
            [&](const Display::Hertz&) {
                // Switch to kHz on the *rounded* value, so 999.96 doesn't print "1000.0 Hz"
                // and then re-parse into the kHz branch on the way back.
                if (std::abs(std::round(plain_value)) >= 1000) return format_double(plain_value / 1000, 1) + " kHz";
                return format_double(plain_value, adaptive_prec(plain_value)) + " Hz";
            },
            [&](const Display::Custom& c) { return c.to_text ? c.to_text(plain_value) : fallback(); },
        }, display);
    };

    // Ints keep whole numbers and only take the unit's suffix.
    auto int_suffix = [](const Display::Numeric& display) -> std::string {
        return std::visit(Inline_visitor{
            [](const Display::Percent&)      { return std::string{" %"}; },
            [](const Display::Decibels&)     { return std::string{" dB"}; },
            [](const Display::Gain&)         { return std::string{" dB"}; },
            [](const Display::Hertz&)        { return std::string{" Hz"}; },
            [](const Display::Milliseconds&) { return std::string{" ms"}; },
            [](const Display::Degrees&)      { return std::string{" °"}; },
            [](const auto&)                  { return std::string{}; },
        }, display);
    };

    return std::visit(Inline_visitor{
        [&](const Semantics::Bool& b) {
            const auto on = plain_value >= 0.5;
            return std::visit(Inline_visitor{
                [&](const Display::Default&) { return std::string{on ? "True" : "False"}; },
                [&](const Display::Labels& l) { return on ? l.on : l.off; },
                [&](const Display::Custom& c) { return c.to_text ? c.to_text(on ? 1. : 0.) : std::string{on ? "True" : "False"}; },
            }, b.display);
        },
        [&](const Semantics::List& l) {
            const auto idx = static_cast<size_t>(plain_value);
            return l.items[idx];
        },
        [&](const Semantics::Int& i) {
            if (const auto* c = std::get_if<Display::Custom>(&i.display); c && c->to_text) return c->to_text(plain_value);
            return format_double(plain_value, 0) + int_suffix(i.display);
        },
        [&](const auto& fr) {
            return numeric(fr.display, [&] { return format_double(plain_value, 2); });
        }
    }, semantics);
}

auto Host_formatter::to_value(const std::string& string, const Semantics::Any& semantics) -> std::optional<double>
{
    using namespace params;

    // Parse a double.
    auto parse_double = [](const std::string& s) -> std::optional<double> {
        // Scan to first digit or possible number prefix.
        auto p = s.c_str();
        while (*p && !std::isdigit(static_cast<unsigned char>(*p)) && *p != '+' && *p != '-' && *p != '.') {
            ++p;
        }

        if (*p == '\0') {
            return std::nullopt;
        }

        using Char_ptr = char*;
        auto end = Char_ptr{nullptr};

        errno = 0;
        const auto value = std::strtod(p, &end);

        if (end == p) {
            return std::nullopt;
        }

        if (errno == ERANGE) {
            errno = 0;
            return std::nullopt;
        }

        return value;
    };

    return std::visit(Inline_visitor{
        [&](const Semantics::Bool& b) -> std::optional<double> {
            const auto true_false = [&]() -> std::optional<double> {
                if (string == "True")  return 1.;
                if (string == "False") return 0.;
                return std::nullopt;
            };
            return std::visit(Inline_visitor{
                [&](const Display::Default&) { return true_false(); },
                [&](const Display::Labels& l) -> std::optional<double> {
                    if (string == l.on)  return 1.;
                    if (string == l.off) return 0.;
                    return std::nullopt;
                },
                [&](const Display::Custom& c) { return c.from_text ? c.from_text(string) : true_false(); },
            }, b.display);
        },
        [&](const Semantics::List& l) -> std::optional<double> {
            for (size_t i = 0; i < l.items.size(); ++i) {
                if (string.compare(l.items[i]) == 0) return static_cast<double>(i);
            }
            return std::nullopt;
        },
        [&](const auto& n) -> std::optional<double> {
            if (const auto* c = std::get_if<Display::Custom>(&n.display); c && c->from_text) return c->from_text(string);

            // Gain is typed in dB; "-inf" (any case) mutes.
            if (std::holds_alternative<Display::Gain>(n.display)) {
                auto lower = string;
                std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("inf") != std::string::npos) return 0.;
                const auto db = parse_double(string);
                if (!db) return std::nullopt;
                return std::pow(10., *db / 20.);
            }

            const auto value = parse_double(string);
            if (!value) return std::nullopt;

            // We print "1.5 kHz" above 1000 Hz, so we have to read it back. Hertz is the only
            // unit that carries a multiplier, so the scan is scoped to it.
            const auto is_kilo = (string.find_first_of("kK") != std::string::npos);
            if (std::holds_alternative<Display::Hertz>(n.display) && is_kilo) return *value * 1000.;

            return value;
        }
    }, semantics);
}

} // namespace tiny