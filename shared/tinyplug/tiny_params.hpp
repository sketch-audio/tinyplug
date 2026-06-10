#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <concepts>
#include <format>
#include <functional>
#include <iomanip>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "tiny_utils.hpp"

namespace tiny::params {

// MARK: - Semantics (types defined together near the adapters below)

// Unit display hints for integer, real semantics.
enum class Units : uint32_t {
    Generic = 0,
    Percent,
    Decibels,
    Hertz,
    Milliseconds,
    Degrees
};

// Get the units string for `units`.
inline auto units_string(Units units) -> std::string
{
    using enum Units;
    switch (units) {
        case Generic:
            return "";
        case Percent:
            return "%";
        case Decibels:
            return "dB";
        case Hertz:
            return "Hz";
        case Milliseconds:
            return "ms";
        case Degrees:
            return "°";
        default:
            return "";
    }
}

// MARK: - Knob adapters

// Specifies the map between plain and normalized value.
struct Adapter {
    // Linear map.
    struct Lin {
        // Regular.
        auto operator==(const Lin&) const -> bool = default;
    };

    // Logarithmic map. Requires param `min_val` > 0.
    struct Log {
        // Regular.
        auto operator==(const Log&) const -> bool = default;
    };

    // Power-law map. Requires `exp` > 0.
    struct Pow {
        // The exponent used for the "norm to plain" transform.
        double exp{2};

        // Regular.
        auto operator==(const Pow&) const -> bool = default;
    };

    // Tapered map. Requires 0 < `taper` < 1.
    struct Taper {
        // Taper pins the knob midpoint to the `taper` factor relative to the output range.
        // E.g. for a param with range 0...100 and taper = 0.25 (and bipolar not set), the param value will be 25 when the knob is at noon.
        double taper{0.5f};

        // When set, the taper factor is applied symmetrically around the knob midpoint.
        bool bipolar{};

        // Regular.
        auto operator==(const Taper&) const -> bool = default;
    };

    // Piecewise linear map.
    // Guarantees `Break_point` corresponding member fields strictly increasing.
    // Requires `Break_point` plain values in param range, norm values in 0...1.
    class Piece {
    public:
        // A pair of line segments that share (plain, norm).
        struct Break_point {
            // Plain value associated with the break point.
            double plain{};

            // Norm value associated with the break point.
            double norm{};

            // Regular.
            auto operator==(const Break_point&) const -> bool = default;
        };

        // An empty piecewise adapter.
        Piece() = default;

        // A piecewise adapter from an (increasing) set of interior break points.
        constexpr Piece(std::vector<Break_point> interior) noexcept : _interior{std::move(interior)} {
            const auto non_increasing = std::ranges::adjacent_find(_interior, [](const auto& a, const auto& b) {
                return !(a.plain < b.plain && a.norm < b.norm);
            });
            const auto increasing = (non_increasing == _interior.end());
            assert(increasing && "Break point corresponding member fields must be strictly increasing.");
            if (!increasing) _interior.clear();
        }

        // Get the interior break points.
        auto interior() const -> const std::vector<Break_point>&
        {
            return _interior;
        }

        // Regular.
        auto operator==(const Piece&) const -> bool = default;

    private:

        std::vector<Break_point> _interior{};

    };

    // The set of all knob adapters.
    using Any = std::variant<Lin, Log, Pow, Taper, Piece>;
};

// MARK: - Semantics

// How the parameter value is to be interpreted by the host.
struct Semantics {
    // Interpreted as "true" or "false".
    struct Bool {
        // Default value.
        bool def_val{true};

        // Regular.
        auto operator==(const Bool&) const -> bool = default;
    };

    // Interpreted as an item in a list. Requires `def_val` < `items.size()`.
    struct List {
        // The list items.
        std::vector<std::string_view> items{"One", "Two", "Three"};

        // Default list item (index).
        size_t def_val{};

        // Regular.
        auto operator==(const List&) const -> bool = default;
    };

    // Interpreted as an integral value.
    struct Int {
        // Minimum plain value.
        int32_t min_val{};

        // Default plain value.
        int32_t def_val{};

        // Maximum plain value.
        int32_t max_val{1};

        // Units (display hint).
        Units units{};

        // Regular.
        auto operator==(const Int&) const -> bool = default;
    };

    // Interpreted as a fixed-point value.
    struct Fixed {
        // Minimum plain value (finite).
        double min_val{};

        // Default plain value.
        double def_val{};

        // Maximum plain value (finite).
        double max_val{};

        // Step size.
        double step_size{0.1};

        // Units (display hint).
        Units units{};

        // Regular.
        auto operator==(const Fixed&) const -> bool = default;
    };

    // Interpreted as a real value.
    struct Real {
        // Minimum plain value (finite).
        double min_val{};

        // Default plain value.
        double def_val{0.5f};

        // Maximum plain value (finite).
        double max_val{1};

        // Units (display hint).
        Units units{};

        // Knob value adapter.
        Adapter::Any knob_adapter{};

        // Regular.
        auto operator==(const Real&) const -> bool = default;
    };

    // The set of all parameter semantics.
    using Any = std::variant<Bool, List, Int, Fixed, Real>;
};

// MARK: - plain to norm

// Normalize a value with real semantics.
constexpr auto plain_to_norm(double x, const Semantics::Real& r) -> double
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

// Normalize a plain value.
constexpr auto plain_to_norm(double x, const Semantics::Any& semantics) -> double
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
            return plain_to_norm(x, r);
        },
    }, semantics);
}

// MARK: - norm to plain

// Denormalize a value with real semantics.
constexpr auto norm_to_plain(double x, const Semantics::Real& r) -> double
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

// Denormalize a normalized value.
constexpr auto norm_to_plain(double x, const Semantics::Any& semantics) -> double
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
            return norm_to_plain(x, r);
        },
    }, semantics);
}

// MARK: - Host policy

enum class Policy : uint32_t {
    // The host should provide a control and an automation lane for this parameter.
    // E.g. Any standard parameter.
    Automation = 0,

    // The host may provide a control for this parameter, but no automation lane.
    // E.g. Latency mode.
    Control,

    // Hidden from host UI. Saves with state.
    // E.g. Any private parameter.
    Hidden,

    // Hidden from host UI. Does not save with state.
    // E.g. GUI-only paremters like "mute" or "solo".
    Interface,
};

// MARK: - Param group, spec

// Forward.
struct Group; struct Spec;

// A parameter node is either a group or a spec.
using Node = std::variant<Group, Spec>;

// A named group of parameter nodes.
struct Group {
    // The group name.
    std::string_view name{""};

    // Used by AUv3 and presets. Must be unique among siblings.
    std::string_view string_id{};

    // The group nodes.
    std::vector<Node> nodes{};
};

// A specification for a parameter.
struct Spec {
    // The parameter's unique address.
    uint32_t address{};

    // Used by AUv3 and presets. Must be unique among siblings.
    std::string_view string_id{};

    // Name.
    std::string_view name{""};

    // Short name. (Optional)
    std::string_view short_name{""};

    // Parameter semantics.
    Semantics::Any semantics{Semantics::Real{}};

    // Host policy.
    Policy policy{Policy::Automation};

    // Regular.
    auto operator==(const Spec&) const -> bool = default;
};

// MARK: - Value spaces

struct Value_conv {
    /*
        Semantics    Implies Linear?    Plain Space         Host Space         Knob Space
        ------------------------------------------------------------------------------
        Bool         Yes                0...1               0...1              0...1
        List         Yes                0...(size - 1)      0...(size - 1)     0...1
        Int          Yes                min...max           min...max          0...1
        Fixed        Yes                min...max           min...max          0...1
        Real         No                 min...max           0...1              0...1
    */
    
    // Convert a plain value to host space.
    static auto plain_to_host(double plain_value, const Semantics::Any& semantics) -> double
    {
        // Normalize real params.
        if (const auto* r = std::get_if<Semantics::Real>(&semantics)) {
            return plain_to_norm(plain_value, *r);
        }
        if (const auto* f = std::get_if<Semantics::Fixed>(&semantics)) {
            return norm_to_plain(plain_to_norm(plain_value, *f), *f); // Force quantize.
        }
        return plain_value;
    }

    // Convert a host value to plain space.
    static auto host_to_plain(double host_value, const Semantics::Any& semantics) -> double
    {
        // Denormalize real params.
        if (const auto* r = std::get_if<Semantics::Real>(&semantics)) {
            return norm_to_plain(host_value, *r);
        }
        if (const auto* f = std::get_if<Semantics::Fixed>(&semantics)) {
            return norm_to_plain(plain_to_norm(host_value, *f), *f); // Force quantize.
        }
        return host_value;
    }

    // Convert a host value to knob space. 
    static auto host_to_knob(double host_value, const Semantics::Any& semantics) -> double
    {
        // Normalize list, integer params.
        return std::visit(Inline_visitor{
            [&](const Semantics::List&) { return plain_to_norm(host_value, semantics); },
            [&](const Semantics::Int&) { return plain_to_norm(host_value, semantics); },
            [&](const Semantics::Fixed&) { return plain_to_norm(host_value, semantics); },
            [=](const auto&) { return host_value; },
        }, semantics);
    }

    // Convert a knob value to host space.
    static auto knob_to_host(double knob_value, const Semantics::Any& semantics) -> double
    {
        // Denormalize list, integer params.
        return std::visit(Inline_visitor{
            [&](const Semantics::List&) { return norm_to_plain(knob_value, semantics); },
            [&](const Semantics::Int&) { return norm_to_plain(knob_value, semantics); },
            [&](const Semantics::Fixed&) { return norm_to_plain(knob_value, semantics); },
            [=](const auto&) { return knob_value; },
        }, semantics);
    }

    // Convert knob value to plain space. 
    static auto knob_to_plain(double knob_value, const Semantics::Any& semantics) -> double
    {
        return norm_to_plain(knob_value, semantics);
    }

    // Convert a plain value to knob space.
    static auto plain_to_knob(double plain_value, const Semantics::Any& semantics) -> double
    {
        return plain_to_norm(plain_value, semantics);
    }
};

inline auto get_plain_min(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool&) {
            return 0.0;
        },
        [](const Semantics::List&) {
            return 0.0;
        },
        [](const Semantics::Int& i) {
            return static_cast<double>(i.min_val);
        },
        [](const Semantics::Fixed& f) {
            return f.min_val;
        },
        [](const Semantics::Real& r) {
            return r.min_val;
        },
    }, spec.semantics);
}

inline auto get_plain_max(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool&) {
            return 1.0;
        },
        [](const Semantics::List& l) {
            return static_cast<double>(l.items.size() - 1);
        },
        [](const Semantics::Int& i) {
            return static_cast<double>(i.max_val);
        },
        [](const Semantics::Fixed& f) {
            return f.max_val;
        },
        [](const Semantics::Real& r) {
            return r.max_val;
        },
    }, spec.semantics);
}

// MARK: - defaults

inline auto get_plain_default(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const Semantics::List& l) { return static_cast<double>(l.def_val); },
        [](const Semantics::Int& i) { return static_cast<double>(i.def_val); },
        [](const Semantics::Fixed& f) { return f.def_val; },
        [](const Semantics::Real& r) { return r.def_val; },
    }, spec.semantics);
}

inline auto get_host_default(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const Semantics::List& l) { return static_cast<double>(l.def_val); },
        [](const Semantics::Int& i) { return static_cast<double>(i.def_val); },
        [](const Semantics::Fixed& f) { return f.def_val; },
        [](const Semantics::Real& r) { return plain_to_norm(r.def_val, r); },
    }, spec.semantics);
}

inline auto get_knob_default(const Spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const Semantics::List& l) { return plain_to_norm(static_cast<double>(l.def_val), l); },
        [](const Semantics::Int& i) { return plain_to_norm(static_cast<double>(i.def_val), i); },
        [](const Semantics::Fixed& f) { return plain_to_norm(f.def_val, f); },
        [](const Semantics::Real& r) { return plain_to_norm(r.def_val, r); },
    }, spec.semantics);
}

// MARK: - other helpers

template<typename X>
inline auto clamp(X x, const Semantics::Any& semantics) -> X
{
    return std::visit(Inline_visitor{
        [x](const Semantics::Bool&) {
            return std::clamp(x, X(0), X(1));
        },
        [x](const Semantics::List& s) {
            const auto max_val = static_cast<X>(s.items.size() - 1);
            return std::clamp(x, X(0), X(max_val));
        },
        [x](const Semantics::Int& s) {
            return std::clamp(x, static_cast<X>(s.min_val), static_cast<X>(s.max_val));
        },
        [x](const Semantics::Fixed& s) {
            return std::clamp(x, static_cast<X>(s.min_val), static_cast<X>(s.max_val));
        },
        [x](const Semantics::Real& s) {
            return std::clamp(x, static_cast<X>(s.min_val), static_cast<X>(s.max_val));
        }
    }, semantics);
}

template<typename X>
inline auto knob_next(X x, const Semantics::Any& semantics) -> X
{
    return std::visit(Inline_visitor{
        [x](const Semantics::Bool&) {
            return x > 0.5f ? X(0) : X(1);
        },
        [x](const Semantics::List& s) {
            const auto plain = Value_conv::knob_to_plain(x, s);
            const auto idx = static_cast<size_t>(plain);
            const auto next = (idx + 1) % s.items.size();
            return Value_conv::plain_to_knob(static_cast<X>(next), s);
        },
        [x](const Semantics::Int& s) {
            const auto plain = Value_conv::knob_to_plain(x, s);
            const auto val = static_cast<int32_t>(plain);
            const auto range = s.max_val - s.min_val + 1;
            const auto next = ((val - s.min_val + 1) % range) + s.min_val;
            return Value_conv::plain_to_knob(static_cast<X>(next), s);
        },
        [x](const Semantics::Fixed& s) {
            const auto plain = Value_conv::knob_to_plain(x, s);
            const auto next = plain + s.step_size;
            if (next > s.max_val) {
                return Value_conv::plain_to_knob(static_cast<X>(s.min_val), s);
            }
            return Value_conv::plain_to_knob(static_cast<X>(next), s);
        },
        [x](const Semantics::Real&) {
            return std::nextafter(x, X(1));
        }
    }, semantics);
}

inline auto is_param_units(Units units, const Semantics::Any& semantics) -> bool
{
    return std::visit(Inline_visitor{
        [units](const Semantics::Fixed& s) { return s.units == units; },
        [units](const Semantics::Real& s) { return s.units == units; },
        [](const auto&) { return false; }
    }, semantics);
}

inline auto param_is_discrete(const Semantics::Any& semantics) -> bool
{
    return std::visit(Inline_visitor{
        [](const Semantics::Bool&) { return true; },
        [](const Semantics::List&) { return true; },
        [](const Semantics::Int&) { return true; },
        [](const Semantics::Fixed&) { return true; },
        [](const Semantics::Real&) { return false; },
    }, semantics);
}

// MARK: - parameter model

template<typename T>
concept Model = requires {
    // An enum class `Address` with a case `num_params`
    typename T::Address;
    requires Enum<typename T::Address>;
    requires std::same_as<std::underlying_type_t<typename T::Address>, uint32_t>;
    { T::Address::Num_params } -> std::same_as<typename T::Address>;
    { T::build_tree() } -> std::same_as<Node>;
};

// MARK: - params impl

namespace params_impl {

inline auto flatten_tree(const Node& root) -> std::vector<Spec>
{
    auto result = std::vector<Spec>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const Spec& spec) { result.push_back(spec); },
            [&](const Group& group) { for (const auto& n : group.nodes) self(n, self); }
        }, node);
    };

    visit(root, visit);
    return result;
}

inline auto validate_spec(const Spec& spec) -> bool
{
    auto in_range = [](auto x, auto a, auto b) -> bool { return a <= x && x <= b; };
    auto ok_range = std::visit(Inline_visitor{
        [](const Semantics::Bool&) { return true; },
        [](const Semantics::List& l) { return l.def_val < l.items.size(); },
        [&](const Semantics::Int& i) { return in_range(i.def_val, i.min_val, i.max_val); },
        [&](const Semantics::Fixed& f) { return in_range(f.def_val, f.min_val, f.max_val); },
        [&](const Semantics::Real& r) { return in_range(r.def_val, r.min_val, r.max_val); },
    }, spec.semantics);
    assert(ok_range && "Param default must satisfy min_val <= def_val <= max_val.");
    return ok_range;
}

inline auto validate_tree(const Node& root, [[maybe_unused]] size_t num_expected) -> bool
{
    auto ids = std::unordered_set<uint32_t>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const Spec& spec) {
                validate_spec(spec);
                ids.insert(spec.address);
            },
            [&](const Group& group) {
                for (const auto& child : group.nodes) {
                    self(child, self);
                }
            }
        }, node);
    };

    visit(root, visit);

    const auto num_leaves = ids.size();
    assert(num_leaves == num_expected && "Param tree must contain all params.");

    if (num_leaves == 0) return true; // Empty tree is valid.

    [[maybe_unused]] const auto [min_val, max_val] = std::ranges::minmax_element(ids);
    assert(*min_val == 0 && "Min param id must be zero.");
    assert(*max_val == num_leaves - 1 && "Max param id must be num_params - 1.");

    return true;
}

template <std::ranges::input_range R, typename Comp>
inline auto sorted_copy(const R& range, Comp comp)
{
    using T = std::ranges::range_value_t<R>;
    auto out = std::vector<T>(std::ranges::begin(range), std::ranges::end(range));
    std::sort(out.begin(), out.end(), comp);
    return out;
}

//  MARK: - array builders

template<typename T>
struct identity_or_atomic_underlying { using type = T; };

template<typename T>
struct identity_or_atomic_underlying<std::atomic<T>> { using type = T; };

template<typename T>
using identity_or_atomic_underlying_t = typename identity_or_atomic_underlying<T>::type;

// This allows us to brace-initialize arrays of atomics as well as plain double/float.
template<typename T, typename F, size_t... I>
constexpr auto make_array_by_indices_impl(F f, std::index_sequence<I...>)
{
    using U = identity_or_atomic_underlying_t<T>;
    return std::array<T, sizeof...(I)>{T{static_cast<U>(f(I))}...};
}

} // namespace params_impl

template<typename T, size_t N, typename F>
constexpr auto make_array_by_indices(F f) -> std::array<T, N>
{
    return params_impl::make_array_by_indices_impl<T>(f, std::make_index_sequence<N>{});
}

enum class Param_order : uint32_t { Indexable, Presentation };
enum class Value_space : uint32_t { Plain, Host, Knob };

// MARK: - params

template<Model User_model>
class Infos {
public:

    static constexpr auto num_params = enum_raw(User_model::Address::Num_params);

    static auto param_tree() -> const Node&
    {
        // Validate once at startup.
        [[maybe_unused]] static const auto validated = [] {
            [[maybe_unused]] const auto is_valid = params_impl::validate_tree(user_tree, num_params);
            assert(is_valid && "Param tree validation failed.");
            return true;
        }();
        
        return user_tree;
    }

    static auto param_specs(Param_order ordering) -> const std::vector<Spec>&
    {
        return ordering == Param_order::Indexable ? indexed_specs : display_specs;
    }

    static auto param_spec(uint32_t address) -> const Spec&
    {
        assert(address < num_params && "Param address out of range.");
        return indexed_specs[address];
    }

    template<typename T>
    static auto make_defaults(Value_space space) -> const std::array<T, num_params>
    {
        return make_array_by_indices<T, num_params>(
            [space](auto i) {
                using enum Value_space;
                switch (space) {
                    case Plain:
                        return get_plain_default(indexed_specs[i]);
                    case Host:
                        return get_host_default(indexed_specs[i]);
                    case Knob:
                        return get_knob_default(indexed_specs[i]);
                    default:
                        return get_plain_default(indexed_specs[i]);
                }
            }
        );
    }

private:

    static constexpr auto id_less = [](const auto& a, const auto& b) { return a.address < b.address; };

    inline static const Node user_tree = User_model::build_tree();
    inline static const std::vector<Spec> display_specs = params_impl::flatten_tree(user_tree);
    inline static const std::vector<Spec> indexed_specs = params_impl::sorted_copy(display_specs, id_less);

};

// MARK: - host formatter

struct Host_formatter {
    // 
    static auto format_string(double host_value, const Semantics::Any& semantics, bool include_units = true) -> std::string
    {
        const auto plain_value = Value_conv::host_to_plain(host_value, semantics);

        auto format_float = [](double value, int precision, bool fixed = true) {
            auto oss = std::ostringstream{};
            if (fixed)
                oss << std::fixed;
            oss << std::setprecision(precision) << value;
            return oss.str();
        };

        return std::visit(Inline_visitor{
            [&](const Semantics::Bool&) {
                return plain_value > 0.5f ? std::string{"True"} : std::string{"False"};
            },
            [&](const Semantics::List& l) {
                const auto idx = static_cast<size_t>(plain_value);
                return std::string{l.items[idx]};
            },
            [&](const Semantics::Int&) {
                return format_float(plain_value, 0); // TODO: - Units
            },
            [&](const auto& fr) {
                using enum Units;
                switch (fr.units) {
                    case Generic:
                        return format_float(plain_value, 2);
                    case Percent: {
                        const auto suffix = include_units ? " %" : "";
                        return format_float(plain_value, 0) + suffix;
                    }
                    case Decibels: {
                        const auto prefix = (plain_value >= 0 ? "+" : "");
                        const auto suffix = include_units ? " dB" : "";
                        return prefix + format_float(plain_value, 1) + suffix;
                    }
                    case Hertz: {
                        if (plain_value >= 1000 && include_units) {
                            const auto suffix = " kHz";
                            return format_float(plain_value / 1000, 1) + suffix;
                        } else {
                            const auto suffix = include_units ? " Hz" : "";
                            return format_float(plain_value, 0) + suffix;
                        }
                    }
                    case Milliseconds: {
                        const auto suffix = include_units ? " ms" : "";
                        const auto prec = plain_value >= 10 ? 0 : 1;
                        return format_float(plain_value, prec) + suffix;
                    }
                    case Degrees: {
                        const auto prefix = (plain_value >= 0 ? "+" : "");
                        const auto suffix = include_units ? " °" : "";
                        return prefix + format_float(plain_value, 0) + suffix;
                    }
                    default:
                        return std::string{};
                }
            }
        }, semantics);
    }

    static auto format_value(const std::string& string, const Semantics::Any& semantics) -> std::optional<double>
    {
        // Strip a suffix from a string, also consuming any whitespace between
        // the numeric part and the suffix.
        auto strip_suffix = [](const std::string& s, const char* suffix) -> std::optional<std::string> {
            const auto suf_len = std::strlen(suffix);
            auto i = s.size();
            while (i > 0 && s[i - 1] == ' ') --i; // trailing whitespace
            if (i < suf_len || s.compare(i - suf_len, suf_len, suffix) != 0) return std::nullopt;
            i -= suf_len;
            while (i > 0 && s[i - 1] == ' ') --i; // whitespace before suffix
            return s.substr(0, i);
        };

        // Parse a double, accepting an optional leading '+' that strtod rejects.
        auto parse_double = [](const std::string& s) -> std::optional<double> {
            if (s.empty()) return std::nullopt;
            char* end = nullptr;
            errno = 0;
            const char* start = s.c_str();
            if (*start == '+') ++start;
            const auto result = std::strtod(start, &end);
            if (end != start && *end == '\0' && errno == 0) return result;
            return std::nullopt;
        };

        return std::visit(Inline_visitor{
            [&](const Semantics::Bool&) -> std::optional<double> {
                if (string == "True")  return 1.0;
                if (string == "False") return 0.0;
                return std::nullopt;
            },
            [&](const Semantics::List& l) -> std::optional<double> {
                for (size_t i = 0; i < l.items.size(); ++i) {
                    if (string.compare(l.items[i]) == 0) return static_cast<double>(i);
                }
                return std::nullopt;
            },
            [&](const Semantics::Int&) -> std::optional<double> {
                return parse_double(string);
            },
            [&](const auto& fr) -> std::optional<double> {
                using enum Units;
                switch (fr.units) {
                    case Generic:
                        return parse_double(string);
                    case Percent:
                        if (const auto s = strip_suffix(string, "%"))  return parse_double(*s);
                        return parse_double(string);
                    case Decibels:
                        if (const auto s = strip_suffix(string, "dB")) return parse_double(*s);
                        return parse_double(string);
                    case Hertz:
                        if (const auto s = strip_suffix(string, "kHz")) {
                            if (const auto v = parse_double(*s)) return *v * 1000.0;
                        }
                        if (const auto s = strip_suffix(string, "Hz"))  return parse_double(*s);
                        return parse_double(string);
                    case Milliseconds:
                        if (const auto s = strip_suffix(string, "ms"))  return parse_double(*s);
                        return parse_double(string);
                    case Degrees:
                        if (const auto s = strip_suffix(string, "°"))   return parse_double(*s);
                        return parse_double(string);
                    default:
                        return std::nullopt;
                }
            }
        }, semantics);
    }
};

} // namespace tiny::params

// MARK: - Transitional aliases
// Names not yet migrated out of tiny:: keep resolving while the params refactor
// proceeds. Each is removed as its type group moves into tiny::params.
namespace tiny {

using params::Units;
using params::units_string;
using params::Value_conv;
using params::Host_formatter;
using params::Param_order;
using params::Value_space;
using params::get_plain_min;
using params::get_plain_max;
using params::get_plain_default;
using params::get_host_default;
using params::get_knob_default;
using params::clamp;
using params::knob_next;
using params::is_param_units;
using params::param_is_discrete;
using params::plain_to_norm;
using params::norm_to_plain;
using params::make_array_by_indices;

} // namespace tiny
