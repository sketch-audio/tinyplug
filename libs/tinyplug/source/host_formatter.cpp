#include "tinyplug/host_formatter.hpp"

#include <cerrno>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "tinyplug/value_helper.hpp"

namespace tiny {

auto Host_formatter::to_string(double host_value, const Semantics::Any& semantics) -> std::string
{
    using namespace params;

    const auto plain_value = Value_helper::host_to_plain(host_value, semantics);

    auto format_double = [](double value, int precision) {
        auto oss = std::ostringstream{};
        oss << std::fixed << std::setprecision(precision) << value;
        return oss.str();
    };

    // Pick precision from the value as it will be *displayed*, not as it arrives. Text round-trips
    // through host space are not bit-exact, so a value printing as "10" comes back as 9.99999… —
    // a bare `>= 10` test would then print "10.0" and the host sees an inconsistent conversion.
    auto adaptive_prec = [](double value) {
        return (std::round(std::abs(value) * 10.0) >= 100.0) ? 0 : 1;
    };

    return std::visit(Inline_visitor{
        [&](const Semantics::Bool&) {
            return plain_value >= 0.5f ? std::string{"True"} : std::string{"False"};
        },
        [&](const Semantics::List& l) {
            const auto idx = static_cast<size_t>(plain_value);
            return l.items[idx];
        },
        [&](const Semantics::Int& i) {
            const auto suffix = Value_helper::units_label(i.units);
            return suffix.empty() ? format_double(plain_value, 0)
                                  : format_double(plain_value, 0) + " " + suffix;
        },
        [&](const auto& fr) {
            using enum Units;
            switch (fr.units) {
                case Generic:
                    return format_double(plain_value, 2);
                case Percent: {
                    const auto suffix = " %";
                    return format_double(plain_value, 0) + suffix;
                }
                case Decibels: {
                    const auto prefix = (plain_value >= 0 ? "+" : "");
                    const auto suffix = " dB";
                    return prefix + format_double(plain_value, adaptive_prec(plain_value)) + suffix;
                }
                case Hertz: {
                    // Switch to kHz on the *rounded* value, so 999.96 doesn't print "1000.0 Hz"
                    // and then re-parse into the kHz branch on the way back.
                    if (std::abs(std::round(plain_value)) >= 1000) {
                        const auto suffix = " kHz";
                        return format_double(plain_value / 1000, 1) + suffix;
                    }
                    else {
                        const auto suffix = " Hz";
                        return format_double(plain_value, adaptive_prec(plain_value)) + suffix;
                    }
                }
                case Milliseconds: {
                    const auto suffix = " ms";
                    return format_double(plain_value, adaptive_prec(plain_value)) + suffix;
                }
                case Degrees: {
                    const auto prefix = (plain_value >= 0 ? "+" : "");
                    const auto suffix = " °";
                    return prefix + format_double(plain_value, 0) + suffix;
                }
                default:
                    return std::string{};
            }
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
        [&](const Semantics::Bool&) -> std::optional<double> {
            if (string == "True")  return 1.;
            if (string == "False") return 0.;
            return std::nullopt;
        },
        [&](const Semantics::List& l) -> std::optional<double> {
            for (size_t i = 0; i < l.items.size(); ++i) {
                if (string.compare(l.items[i]) == 0) return static_cast<double>(i);
            }
            return std::nullopt;
        },
        [&](const auto& fr) -> std::optional<double> {
            const auto value = parse_double(string);
            if (!value) return std::nullopt;

            // We print "1.5 kHz" above 1000 Hz, so we have to read it back. Hertz is the only
            // unit that carries a multiplier, so the scan is scoped to it.
            const auto is_kilo = (string.find_first_of("kK") != std::string::npos);
            if (fr.units == Units::Hertz && is_kilo) return *value * 1000.;

            return value;
        }
    }, semantics);
}

} // namespace tiny