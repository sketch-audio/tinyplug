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
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "tiny_utils.h"

namespace tiny {

// MARK: - Semantics

// Specifies a parameter whose value is to be interpreted as "true" or "false" by the host.
struct Bool_semantics {
    // Default value.
    bool def_val{true};

    // Regular.
    bool operator==(const Bool_semantics&) const = default;
};

// Specifies a parameter whose value is to be interpreted as an item in a list by the host.
// Requires `def_val` < `items.size()`.
struct List_semantics {
    // The list items.
    std::vector<const char*> items{"One", "Two", "Three"};

    // Default list item (index). 
    size_t def_val{};

    // Regular.
    bool operator==(const List_semantics&) const = default;
};

// Unit display hints for integer, real semantics.
enum class Units : uint32_t {
    generic = 0,
    percent,
    decibels,
    hertz,
    milliseconds,
};

// Get the units string for `units`.
inline auto units_string(Units units) -> std::string
{
    using enum Units;
    switch (units) {
        case generic:
            return "";
        case percent:
            return "%";
        case decibels:
            return "dB";
        case hertz:
            return "Hz";
        case milliseconds:
            return "ms";
        default:
            return "";
    }
}

// Specifies a parameter whose value is to be interpreted as an integral value by the host.
struct Int_semantics {
    // Minimum plain value.
    int32_t min_val{};

    // Default plain value.
    int32_t def_val{};

    // Maximum plain value.
    int32_t max_val{1};

    // Units (display hint).
    Units units{};

    // Regular.
    bool operator==(const Int_semantics&) const = default;
};

// MARK: - Knob adapters

// Specifies a linear map between plain and normalized value.
struct Adapt_lin {
    // Regular.
    bool operator==(const Adapt_lin&) const = default;
};

// Specifies a logarithmic map between plain and normalized value.
// Requires param `min_val` > 0.
struct Adapt_log {
    // Regular.
    bool operator==(const Adapt_log&) const = default;
};

// Specifies a power-law map between plain and normalized value.
// Requires `exp` > 0.
struct Adapt_pow {
    // The exponent used for the "norm to plain" transform.
    double exp{2};

    // Regular.
    bool operator==(const Adapt_pow&) const = default;
};

// Specifies a tapered map between plain and normalized value.
// Requires 0 < `taper` < 1.
struct Adapt_taper {
    // Taper pins the knob midpoint to the `taper` factor relative to the output range.
    // E.g. for a param with range 0...100 and taper = 0.25 (and bipolar not set), the param value will be 25 when the knob is at noon.
    double taper{0.5f};

    // When set, the taper factor is applied symmetrically around the knob midpoint.
    bool bipolar{};

    // Regular.
    bool operator==(const Adapt_taper&) const = default;
};

// Specifies a piecewise linear map between plain and normalized value.
// Guarantees `Break_point` corresponding member fields strictly increasing.
// Requires `Break_point` plain values in param range, norm values in 0...1.
class Adapt_piece {
public:
    // Specifies that the piecewise adapter should perform its mapping with a pair of line segments that share (plain, norm).
    struct Break_point {
        // Plain value associated with the break point.
        double plain{};

        // Norm value associated with the break point.
        double norm{};

        // Regular.
        bool operator==(const Break_point&) const = default;
    };

    // An empty piecewise adapter.
    Adapt_piece() = default;

    // A piecewise adapter from an (increasing) set of interior break points.
    constexpr Adapt_piece(std::vector<Break_point> interior) noexcept : _interior{std::move(interior)} {
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
    bool operator==(const Adapt_piece&) const = default;

private:
    
    std::vector<Break_point> _interior{};

};

// Specifies the map between plain and normalized value.
using Knob_adapter = std::variant<Adapt_lin, Adapt_log, Adapt_pow, Adapt_taper, Adapt_piece>;

// MARK: - Real semantics

// Specifies a parameter whose value is to be interpreted as a real value by the host.
struct Real_semantics {
    // Minimum plain value (finite).
    double min_val{};

    // Default plain value.
    double def_val{0.5f};

    // Maximum plain value (finite).
    double max_val{1};

    // Units (display hint).
    Units units{};

    // Knob value adapter.
    Knob_adapter knob_adapter{};

    // Regular.
    bool operator==(const Real_semantics&) const = default;
};

// Specifies how the parameter value is to be interpreted by the host..
using Value_semantics = std::variant<Bool_semantics, List_semantics, Int_semantics, Real_semantics>;

// MARK: - plain to norm

// Normalize a value with real semantics.
constexpr auto plain_to_norm(double x, const Real_semantics& r) -> double
{
    return std::visit(Inline_visitor{
        [&](const Adapt_lin&) {
            return (x - r.min_val) / (r.max_val - r.min_val);
        },
        [&](const Adapt_log&) {
            assert(r.min_val > 0 && "Adapt_log requires range min_val > 0.");
            const auto log_min = std::log2(r.min_val);
            const auto k = std::log2(r.max_val) - log_min;
            return (std::log2(x) - log_min) / k;
        },
        [&](const Adapt_pow& p) {
            assert(p.exp > 0 && "Adapt_pow requires exp > 0.");
            const auto lin = (x - r.min_val) / (r.max_val - r.min_val);
            return std::pow(lin, 1 / p.exp);
        },
        [&](const Adapt_taper& t) {
            assert(0 < t.taper && t.taper < 1 && "Adapt taper requires 0 < taper < 1.");
            return normalized(x, r.min_val, r.max_val, t.taper, t.bipolar);
        },
        [&](const Adapt_piece& p) {
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
constexpr auto plain_to_norm(double x, const Value_semantics& semantics) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) {
            return x;
        },
        [&](const List_semantics& l) {
            const auto step_count = static_cast<double>(l.items.size() - 1);
            return x / step_count;
        },
        [&](const Int_semantics& i) {
            const auto step_count = static_cast<double>(i.max_val - i.min_val);
            return (x - i.min_val) / step_count;
        },
        [&](const Real_semantics& r) {
            return plain_to_norm(x, r);
        },
    }, semantics);
}

// MARK: - norm to plain

// Denormalize a value with real semantics.
constexpr auto norm_to_plain(double x, const Real_semantics& r) -> double
{
    return std::visit(Inline_visitor{
        [&](const Adapt_lin&) {
            return (r.max_val - r.min_val) * x + r.min_val;
        },
        [&](const Adapt_log&) {
            assert(r.min_val > 0 && "Adapt_log requires range min_val > 0.");
            const auto log_min = std::log2(r.min_val);
            const auto k = std::log2(r.max_val) - log_min;
            return std::exp2(k * x + log_min);
        },
        [&](const Adapt_pow& p) {
            assert(p.exp > 0 && "Adapt_pow requires exp > 0.");
            const auto lin = std::pow(x, p.exp);
            return (r.max_val - r.min_val) * lin + r.min_val;
        },
        [&](const Adapt_taper& t) {
            assert(0 < t.taper && t.taper < 1 && "Adapt taper requires 0 < taper < 1.");
            return denormalized(x, r.min_val, r.max_val, t.taper, t.bipolar);
        },
        [&](const Adapt_piece& p) {
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
constexpr auto norm_to_plain(double x, const Value_semantics& semantics) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) {
            return std::floor(std::min(double{1}, 2 * x));
        },
        [&](const List_semantics& l) {
            const auto step_count = static_cast<double>(l.items.size() - 1);
            return std::floor(std::min(step_count, x * (step_count + 1)));
        },
        [&](const Int_semantics& i) {
            const auto step_count = static_cast<double>(i.max_val - i.min_val);
            return std::floor(std::min(step_count, x * (step_count + 1))) + i.min_val;
        },
        [&](const Real_semantics& r) {
            return norm_to_plain(x, r);
        },
    }, semantics);
}

// MARK: - Host policy

enum class Host_policy : uint32_t {
    // The host should provide a control and an automation lane for this parameter.
    // E.g. Any standard parameter.
    automation = 0,

    // The host may provide a control for this parameter, but no automation lane.
    // E.g. Latency mode.
    control,

    // Hidden from host UI. Saves with state.
    // E.g. Any private parameter.
    hidden,

    // Hidden from host UI. Does not save with state.
    // E.g. GUI-only paremters like "mute" or "solo".
    interface,
};

// MARK: - Param group, spec

// Forward.
struct Param_group; struct Param_spec;

// A parameter node is either a group or a spec.
using Param_node = std::variant<Param_group, Param_spec>;

// A named group of parameter nodes.
struct Param_group {
    // The group name.
    const char* name{""};

    // Required for AUv3, otherwise unused.
    const char* string_id{nullptr};

    // The group nodes.
    std::vector<Param_node> nodes{};
};

// A specification for a parameter.
struct Param_spec {
    // The parameter's unique address.
    uint32_t address{};

    // Required for AUv3, otherwise unused.
    const char* string_id{nullptr};

    // Name.
    const char* name{""};

    // Short name. (Optional)
    const char* short_name{""};

    // Parameter semantics.
    Value_semantics semantics{Real_semantics{}};

    // Host policy.
    Host_policy policy{Host_policy::automation};

    // Regular.
    bool operator==(const Param_spec&) const = default;
};

// MARK: - Value spaces

struct Value_conv {
    /*
        Semantics    Implies Linear?    Plain Space         Host Space         Knob Space
        ------------------------------------------------------------------------------
        Bool         Yes                0...1               0...1              0...1
        List         Yes                0...(size - 1)      0...(size - 1)     0...1
        Int          Yes                min...max           min...max          0...1
        Real         No                 min...max           0...1              0...1
    */
    
    // Convert a plain value to host space.
    static auto plain_to_host(double plain_value, const Value_semantics& semantics) -> double
    {
        // Normalize real params.
        if (const auto* r = std::get_if<Real_semantics>(&semantics)) {
            return plain_to_norm(plain_value, *r);
        }
        return plain_value;
    }

    // Convert a host value to plain space.
    static auto host_to_plain(double host_value, const Value_semantics& semantics) -> double
    {
        // Denormalize real params.
        if (const auto* r = std::get_if<Real_semantics>(&semantics)) {
            return norm_to_plain(host_value, *r);
        }
        return host_value;
    }

    // Convert a host value to knob space. 
    static auto host_to_knob(double host_value, const Value_semantics& semantics) -> double
    {
        // Normalize list, integer params.
        return std::visit(Inline_visitor{
            [&](const List_semantics&) { return plain_to_norm(host_value, semantics); },
            [&](const Int_semantics&) { return plain_to_norm(host_value, semantics); },
            [=](const auto&) { return host_value; },
        }, semantics);
    }

    // Convert a knob value to host space.
    static auto knob_to_host(double knob_value, const Value_semantics& semantics) -> double
    {
        // Denormalize list, integer params.
        return std::visit(Inline_visitor{
            [&](const List_semantics&) { return norm_to_plain(knob_value, semantics); },
            [&](const Int_semantics&) { return norm_to_plain(knob_value, semantics); },
            [=](const auto&) { return knob_value; },
        }, semantics);
    }

    // Convert knob value to plain space. 
    static auto knob_to_plain(double knob_value, const Value_semantics& semantics) -> double
    {
        return norm_to_plain(knob_value, semantics);
    }

    // Convert a plain value to knob space.
    static auto plain_to_knob(double plain_value, const Value_semantics& semantics) -> double
    {
        return plain_to_norm(plain_value, semantics);
    }
};

// MARK: - defaults

inline auto get_plain_default(const Param_spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const List_semantics& l) { return static_cast<double>(l.def_val); },
        [](const Int_semantics& i) { return static_cast<double>(i.def_val); },
        [](const Real_semantics& r) { return r.def_val; },
    }, spec.semantics);
}

inline auto get_host_default(const Param_spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const List_semantics& l) { return static_cast<double>(l.def_val); },
        [](const Int_semantics& i) { return static_cast<double>(i.def_val); },
        [](const Real_semantics& r) { return plain_to_norm(r.def_val, r); },
    }, spec.semantics);
}

inline auto get_knob_default(const Param_spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [](const List_semantics& l) { return plain_to_norm(static_cast<double>(l.def_val), l); },
        [](const Int_semantics& i) { return plain_to_norm(static_cast<double>(i.def_val), i); },
        [](const Real_semantics& r) { return plain_to_norm(r.def_val, r); },
    }, spec.semantics);
}

// MARK: - other helpers

template<typename X>
inline auto clamp(X x, const Value_semantics& semantics) -> X
{
    return std::visit(Inline_visitor{
        [x](const Bool_semantics&) {
            return std::clamp(x, X(0), X(1));
        },
        [x](const List_semantics& s) {
            const auto max_val = static_cast<X>(s.items.size() - 1);
            return std::clamp(x, X(0), X(max_val));
        },
        [x](const Int_semantics& s) {
            return std::clamp(x, static_cast<X>(s.min_val), static_cast<X>(s.max_val));
        },
        [x](const Real_semantics& s) {
            return std::clamp(x, static_cast<X>(s.min_val), static_cast<X>(s.max_val));
        }
    }, semantics);
}

template<typename X>
inline auto knob_next(X x, const Value_semantics& semantics) -> X
{
    return std::visit(Inline_visitor{
        [x](const Bool_semantics&) {
            return x > 0.5f ? X(0) : X(1);
        },
        [x](const List_semantics& s) {
            const auto plain = Value_conv::knob_to_plain(x, s);
            const auto idx = static_cast<size_t>(plain);
            const auto next = (idx + 1) % s.items.size();
            return Value_conv::plain_to_knob(static_cast<X>(next), s);
        },
        [x](const Int_semantics& s) {
            const auto plain = Value_conv::knob_to_plain(x, s);
            const auto val = static_cast<int32_t>(plain);
            const auto range = s.max_val - s.min_val + 1;
            const auto next = ((val - s.min_val + 1) % range) + s.min_val;
            return Value_conv::plain_to_knob(static_cast<X>(next), s);
        },
        [x](const Real_semantics&) {
            return std::nextafter(x, X(1));
        }
    }, semantics);
}

inline auto is_param_units(Units units, const Value_semantics& semantics) -> bool
{
    return std::visit(Inline_visitor{
        [units](const Real_semantics& s) { return s.units == units; },
        [](const auto&) { return false; }
    }, semantics);
}

inline auto param_is_discrete(const Value_semantics& semantics) -> bool
{
    return std::visit(Inline_visitor{
        [](const Bool_semantics&) { return true; },
        [](const List_semantics&) { return true; },
        [](const Int_semantics&) { return true; },
        [](const Real_semantics&) { return false; },
    }, semantics);
}

// MARK: - parameter model

template<typename T>
concept Some_param_model = requires {
    // An enum class `Param_address` with a case `num_params`
    typename T::Param_address;
    requires Enum<typename T::Param_address>;
    requires std::same_as<std::underlying_type_t<typename T::Param_address>, uint32_t>;

    // A static function `build_tree` that returns a `Param_node<Param_address>`
    { T::build_tree() } -> std::same_as<Param_node>;
};

// MARK: - params impl

namespace params_impl {

inline auto flatten_tree(const Param_node& root) -> std::vector<Param_spec>
{
    auto result = std::vector<Param_spec>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const Param_spec& spec) { result.push_back(spec); },
            [&](const Param_group& group) { for (const auto& n : group.nodes) self(n, self); }
        }, node);
    };

    visit(root, visit);
    return result;
}

inline auto validate_spec(const Param_spec& spec) -> bool
{
    auto in_range = [](auto x, auto a, auto b) -> bool { return a <= x && x <= b; };
    auto ok_range = std::visit(Inline_visitor{
        [](const Bool_semantics&) { return true; },
        [](const List_semantics& l) { return l.def_val < l.items.size(); },
        [&](const Int_semantics& i) { return in_range(i.def_val, i.min_val, i.max_val); },
        [&](const Real_semantics& r) { return in_range(r.def_val, r.min_val, r.max_val); },
    }, spec.semantics);
    assert(ok_range && "Param default must satisfy min_val <= def_val <= max_val.");
    return ok_range;
}

inline auto validate_tree(const Param_node& root, [[maybe_unused]] size_t num_expected) -> bool
{
    auto ids = std::unordered_set<uint32_t>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit(Inline_visitor{
            [&](const Param_spec& spec) {
                validate_spec(spec);
                ids.insert(spec.address);
            },
            [&](const Param_group& group) {
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

template<Some_param_model User_model>
class Param_infos {
public:

    static constexpr auto num_params = enum_raw(User_model::Param_address::num_params);

    static auto param_tree() -> const Param_node&
    {
        // Validate once at startup.
        [[maybe_unused]] static const auto validated = [] {
            [[maybe_unused]] const auto is_valid = params_impl::validate_tree(user_tree, num_params);
            assert(is_valid && "Param tree validation failed.");
            return true;
        }();
        
        return user_tree;
    }

    static auto param_specs(Param_order ordering) -> const std::vector<Param_spec>&
    {
        return ordering == Param_order::Indexable ? indexed_specs : display_specs;
    }

    static auto param_spec(uint32_t address) -> const Param_spec&
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

    inline static const Param_node user_tree = User_model::build_tree();
    inline static const std::vector<Param_spec> display_specs = params_impl::flatten_tree(user_tree);
    inline static const std::vector<Param_spec> indexed_specs = params_impl::sorted_copy(display_specs, id_less);

};

// MARK: - host formatter

struct Host_formatter {
    // 
    static auto format_string(double host_value, const Value_semantics& semantics, bool include_units = true) -> std::string
    {
        const auto plain_value = Value_conv::host_to_plain(host_value, semantics);
        return std::visit(Inline_visitor{
            [&](const Bool_semantics&) {
                return plain_value > 0.5f ? std::string{"True"} : std::string{"False"};
            },
            [&](const List_semantics& l) {
                const auto idx = static_cast<size_t>(plain_value);
                return std::string{l.items[idx]};
            },
            [&](const Int_semantics&) {
                // We were using std::format but it requires iOS 16.3 for float formatting.
                // https://developer.apple.com/xcode/cpp/ (see std::to_chars)
                auto format_float = [](double value, int precision, bool fixed = true) {
                    auto oss = std::ostringstream{};
                    if (fixed)
                        oss << std::fixed;
                    oss << std::setprecision(precision) << value;
                    return oss.str();
                };
                return format_float(plain_value, 0); // TODO: - Units
            },
            [&](const Real_semantics& r) {
                using enum Units;

                auto format_float = [](double value, int precision, bool fixed = true) {
                    auto oss = std::ostringstream{};
                    if (fixed)
                        oss << std::fixed;
                    oss << std::setprecision(precision) << value;
                    return oss.str();
                };

                switch (r.units) {
                    case generic:
                        return format_float(plain_value, 2);
                    case percent: {
                        const auto suffix = include_units ? " %" : "";
                        return format_float(plain_value, 0) + suffix;
                    }
                    case decibels: {
                        const auto prefix = (plain_value >= 0 ? "+" : "");
                        const auto suffix = include_units ? " dB" : "";
                        return prefix + format_float(plain_value, 1) + suffix;
                    }
                    case hertz: {
                        if (plain_value > 1000 && include_units) {
                            const auto suffix = " kHz";
                            return format_float(plain_value / 1000, 1) + suffix;
                        } else {
                            const auto suffix = include_units ? " Hz" : "";
                            return format_float(plain_value, 0) + suffix;
                        }
                    }
                    case milliseconds: {
                        const auto suffix = include_units ? " ms" : "";
                        return format_float(plain_value, 1) + suffix;
                    }
                    default:
                        return std::string{};
                }
            }
        }, semantics);
    }

    static auto format_value(const std::string& string, const Value_semantics& /*semantics*/) -> std::optional<double>
    {
        char* end = nullptr;
        errno = 0;
        const auto result = std::strtod(string.c_str(), &end);

        // Check for conversion success
        if (end != string.c_str() && *end == '\0' && errno == 0) {
            return result;
        }

        return std::nullopt;
    }
};

} // namespace tiny
