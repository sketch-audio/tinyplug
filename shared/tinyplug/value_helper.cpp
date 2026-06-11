#include "value_helper.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <variant>

namespace tiny::params {

// MARK: - Real-adapter normalization

auto Value_helper::plain_to_knob(double x, const Semantics::Real& r) -> double
{
    return std::visit(Inline_visitor{
        [&](const Adapter::Lin&) {
            return (x - r.min_val) / (r.max_val - r.min_val);
        },
        [&](const Adapter::Log&) {
            assert(r.min_val > 0 && "Adapter::Log requires range min_val > 0.");
            const auto log_min = std::log2(r.min_val);
            const auto k = std::log2(r.max_val) - log_min;
            return (std::log2(x) - log_min) / k;
        },
        [&](const Adapter::Pow& p) {
            assert(p.exp > 0 && "Adapter::Pow requires exp > 0.");
            const auto lin = (x - r.min_val) / (r.max_val - r.min_val);
            return std::pow(lin, 1 / p.exp);
        },
        [&](const Adapter::Taper& t) {
            assert(0 < t.taper && t.taper < 1 && "Adapt taper requires 0 < taper < 1.");
            return normalized(x, r.min_val, r.max_val, t.taper, t.bipolar);
        },
        [&](const Adapter::Piece& p) {
            const auto& interior = p.interior();

            for ([[maybe_unused]] const auto& bp : interior) {
                assert(r.min_val < bp.plain && bp.plain < r.max_val && "Break point plain values must be in param range.");
                assert(0 < bp.norm && bp.norm < 1 && "Break point norm values must be in 0...1.");
            }

            if (x <= r.min_val) return double{};

            if (interior.empty()) {
                return (x - r.min_val) / (r.max_val - r.min_val);
            }

            const auto& first = interior.front();
            if (x <= first.plain) {
                const auto t = (x - r.min_val) / (first.plain - r.min_val);
                return t * first.norm;
            }

            for (size_t i = 1; i < interior.size(); ++i) {
                const auto& a = interior[i - 1];
                const auto& b = interior[i];
                if (x <= b.plain) {
                    const auto t = (x - a.plain) / (b.plain - a.plain);
                    return a.norm + t * (b.norm - a.norm);
                }
            }

            const auto& last = interior.back();
            if (x <= r.max_val) {
                const auto t = (x - last.plain) / (r.max_val - last.plain);
                return last.norm + t * (1 - last.norm);
            }

            return double{1};
        },
    }, r.knob_adapter);
}

auto Value_helper::knob_to_plain(double x, const Semantics::Real& r) -> double
{
    return std::visit(Inline_visitor{
        [&](const Adapter::Lin&) {
            return (r.max_val - r.min_val) * x + r.min_val;
        },
        [&](const Adapter::Log&) {
            assert(r.min_val > 0 && "Adapter::Log requires range min_val > 0.");
            const auto log_min = std::log2(r.min_val);
            const auto k = std::log2(r.max_val) - log_min;
            return std::exp2(k * x + log_min);
        },
        [&](const Adapter::Pow& p) {
            assert(p.exp > 0 && "Adapter::Pow requires exp > 0.");
            const auto lin = std::pow(x, p.exp);
            return (r.max_val - r.min_val) * lin + r.min_val;
        },
        [&](const Adapter::Taper& t) {
            assert(0 < t.taper && t.taper < 1 && "Adapt taper requires 0 < taper < 1.");
            return denormalized(x, r.min_val, r.max_val, t.taper, t.bipolar);
        },
        [&](const Adapter::Piece& p) {
            const auto& interior = p.interior();

            for ([[maybe_unused]] const auto& bp : interior) {
                assert(r.min_val < bp.plain && bp.plain < r.max_val && "Break point plain values must be in param range.");
                assert(0 < bp.norm && bp.norm < 1 && "Break point norm values must be in 0...1.");
            }

            if (x <= 0) return r.min_val;

            if (interior.empty()) {
                return (r.max_val - r.min_val) * x + r.min_val;
            }

            const auto& first = interior.front();
            if (x <= first.norm) {
                const auto t = x / first.norm;
                return r.min_val + t * (first.plain - r.min_val);
            }

            for (size_t i = 1; i < interior.size(); ++i) {
                const auto& a = interior[i - 1];
                const auto& b = interior[i];
                if (x <= b.norm) {
                    const auto t = (x - a.norm) / (b.norm - a.norm);
                    return a.plain + t * (b.plain - a.plain);
                }
            }

            const auto& last = interior.back();
            if (x <= 1) {
                const auto t = (x - last.norm) / (1 - last.norm);
                return last.plain + t * (r.max_val - last.plain);
            }

            return r.max_val;
        },
    }, r.knob_adapter);
}

// MARK: - plain <-> knob (Any)

auto Value_helper::plain_to_knob(double x, const Semantics::Any& semantics) -> double
{
    return std::visit(Inline_visitor{
        [&](const Semantics::Bool&) {
            return x;
        },
        [&](const Semantics::List& l) {
            const auto step_count = static_cast<double>(l.items.size() - 1);
            return x / step_count;
        },
        [&](const Semantics::Int& i) {
            const auto step_count = static_cast<double>(i.max_val - i.min_val);
            return (x - i.min_val) / step_count;
        },
        [&](const Semantics::Fixed& f) {
            const auto step_count = (f.max_val - f.min_val) / f.step_size;
            return (x - f.min_val) / (step_count * f.step_size);
        },
        [&](const Semantics::Real& r) {
            return plain_to_knob(x, r);
        },
    }, semantics);
}

auto Value_helper::knob_to_plain(double x, const Semantics::Any& semantics) -> double
{
    return std::visit(Inline_visitor{
        [&](const Semantics::Bool&) {
            return std::floor(std::min(double{1}, 2 * x));
        },
        [&](const Semantics::List& l) {
            const auto step_count = static_cast<double>(l.items.size() - 1);
            return std::floor(std::min(step_count, x * (step_count + 1)));
        },
        [&](const Semantics::Int& i) {
            const auto step_count = static_cast<double>(i.max_val - i.min_val);
            return std::floor(std::min(step_count, x * (step_count + 1))) + i.min_val;
        },
        [&](const Semantics::Fixed& f) {
            const auto step_count = (f.max_val - f.min_val) / f.step_size;
            return std::floor(std::min(step_count, x * (step_count + 1))) * f.step_size + f.min_val;
        },
        [&](const Semantics::Real& r) {
            return knob_to_plain(x, r);
        },
    }, semantics);
}

// MARK: - cross-space conversions

auto Value_helper::plain_to_host(double plain_value, const Semantics::Any& semantics) -> double
{
    // Normalize real params; quantize fixed.
    if (const auto* r = std::get_if<Semantics::Real>(&semantics)) {
        return plain_to_knob(plain_value, *r);
    }
    if (std::holds_alternative<Semantics::Fixed>(semantics)) {
        return quantize(plain_value, semantics);
    }
    return plain_value;
}

auto Value_helper::host_to_plain(double host_value, const Semantics::Any& semantics) -> double
{
    // Denormalize real params; quantize fixed.
    if (const auto* r = std::get_if<Semantics::Real>(&semantics)) {
        return knob_to_plain(host_value, *r);
    }
    if (std::holds_alternative<Semantics::Fixed>(semantics)) {
        return quantize(host_value, semantics);
    }
    return host_value;
}

auto Value_helper::host_to_knob(double host_value, const Semantics::Any& semantics) -> double
{
    // Normalize list, integer, fixed params; real host space is already knob space.
    return std::visit(Inline_visitor{
        [&](const Semantics::List&) { return plain_to_knob(host_value, semantics); },
        [&](const Semantics::Int&) { return plain_to_knob(host_value, semantics); },
        [&](const Semantics::Fixed&) { return plain_to_knob(host_value, semantics); },
        [=](const auto&) { return host_value; },
    }, semantics);
}

auto Value_helper::knob_to_host(double knob_value, const Semantics::Any& semantics) -> double
{
    // Denormalize list, integer, fixed params; real host space is already knob space.
    return std::visit(Inline_visitor{
        [&](const Semantics::List&) { return knob_to_plain(knob_value, semantics); },
        [&](const Semantics::Int&) { return knob_to_plain(knob_value, semantics); },
        [&](const Semantics::Fixed&) { return knob_to_plain(knob_value, semantics); },
        [=](const auto&) { return knob_value; },
    }, semantics);
}

auto Value_helper::convert(double value, Space from, Space to, const Semantics::Any& semantics) -> double
{
    if (from == to) return value;

    // Pivot through plain space.
    const auto plain = [&] {
        switch (from) {
            case Space::Plain: return value;
            case Space::Host:  return host_to_plain(value, semantics);
            case Space::Knob:  return knob_to_plain(value, semantics);
        }
        return value;
    }();

    switch (to) {
        case Space::Plain: return plain;
        case Space::Host:  return plain_to_host(plain, semantics);
        case Space::Knob:  return plain_to_knob(plain, semantics);
    }
    return plain;
}

auto Value_helper::quantize(double plain_value, const Semantics::Any& semantics) -> double
{
    return knob_to_plain(plain_to_knob(plain_value, semantics), semantics);
}

// MARK: - range

auto Value_helper::plain_min(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool&) { return 0.0; },
        [](const Semantics::List&) { return 0.0; },
        [](const Semantics::Int& i) { return static_cast<double>(i.min_val); },
        [](const Semantics::Fixed& f) { return f.min_val; },
        [](const Semantics::Real& r) { return r.min_val; },
    }, spec.semantics);
}

auto Value_helper::plain_max(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool&) { return 1.0; },
        [](const Semantics::List& l) { return static_cast<double>(l.items.size() - 1); },
        [](const Semantics::Int& i) { return static_cast<double>(i.max_val); },
        [](const Semantics::Fixed& f) { return f.max_val; },
        [](const Semantics::Real& r) { return r.max_val; },
    }, spec.semantics);
}

// MARK: - defaults

auto Value_helper::default_value(const Spec& spec, Space space) -> double
{
    const auto plain_default = std::visit(Inline_visitor{
        [](const Semantics::Bool& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const Semantics::List& l) { return static_cast<double>(l.def_val); },
        [](const Semantics::Int& i) { return static_cast<double>(i.def_val); },
        [](const Semantics::Fixed& f) { return f.def_val; },
        [](const Semantics::Real& r) { return r.def_val; },
    }, spec.semantics);

    switch (space) {
        case Space::Plain: return plain_default;
        case Space::Host:  return plain_to_host(plain_default, spec.semantics);
        case Space::Knob:  return plain_to_knob(plain_default, spec.semantics);
    }
    return plain_default;
}

// MARK: - semantics queries

auto Value_helper::is_discrete(const Semantics::Any& semantics) -> bool
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool&) { return true; },
        [](const Semantics::List&) { return true; },
        [](const Semantics::Int&) { return true; },
        [](const Semantics::Fixed&) { return true; },
        [](const Semantics::Real&) { return false; },
    }, semantics);
}

auto Value_helper::has_units(Units units, const Semantics::Any& semantics) -> bool
{
    return std::visit(Inline_visitor{
        [units](const Semantics::Fixed& s) { return s.units == units; },
        [units](const Semantics::Real& s) { return s.units == units; },
        [](const auto&) { return false; }
    }, semantics);
}

auto Value_helper::units_label(Units units) -> std::string
{
    using enum Units;
    switch (units) {
        case Generic:      return "";
        case Percent:      return "%";
        case Decibels:     return "dB";
        case Hertz:        return "Hz";
        case Milliseconds: return "ms";
        case Degrees:      return "°";
        default:           return "";
    }
}

// MARK: - clamp / step

auto Value_helper::clamp(double x, const Semantics::Any& semantics) -> double
{
    return std::visit(Inline_visitor{
        [x](const Semantics::Bool&) {
            return std::clamp(x, 0.0, 1.0);
        },
        [x](const Semantics::List& s) {
            const auto max_val = static_cast<double>(s.items.size() - 1);
            return std::clamp(x, 0.0, max_val);
        },
        [x](const Semantics::Int& s) {
            return std::clamp(x, static_cast<double>(s.min_val), static_cast<double>(s.max_val));
        },
        [x](const Semantics::Fixed& s) {
            return std::clamp(x, s.min_val, s.max_val);
        },
        [x](const Semantics::Real& s) {
            return std::clamp(x, s.min_val, s.max_val);
        }
    }, semantics);
}

auto Value_helper::knob_next(double x, const Semantics::Any& semantics) -> double
{
    return std::visit(Inline_visitor{
        [x](const Semantics::Bool&) {
            return x > 0.5 ? 0.0 : 1.0;
        },
        [x](const Semantics::List& s) {
            const auto plain = knob_to_plain(x, s);
            const auto idx = static_cast<size_t>(plain);
            const auto next = (idx + 1) % s.items.size();
            return plain_to_knob(static_cast<double>(next), s);
        },
        [x](const Semantics::Int& s) {
            const auto plain = knob_to_plain(x, s);
            const auto val = static_cast<int32_t>(plain);
            const auto range = s.max_val - s.min_val + 1;
            const auto next = ((val - s.min_val + 1) % range) + s.min_val;
            return plain_to_knob(static_cast<double>(next), s);
        },
        [x](const Semantics::Fixed& s) {
            const auto plain = knob_to_plain(x, s);
            const auto next = plain + s.step_size;
            if (next > s.max_val) {
                return plain_to_knob(s.min_val, s);
            }
            return plain_to_knob(next, s);
        },
        [x](const Semantics::Real&) {
            return std::nextafter(x, 1.0);
        }
    }, semantics);
}

} // namespace tiny::params
