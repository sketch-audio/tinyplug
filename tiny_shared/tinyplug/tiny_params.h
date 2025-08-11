#pragma once

#include <algorithm>
#include <concepts>
#include <format>
#include <functional>
#include <type_traits>
#include <ranges>
#include <unordered_set>
#include <variant>
#include <vector>

#include "tiny_utils.h"

namespace tiny {

// MARK: - types

struct Param_group;
struct Param_spec;
using Param_node = std::variant<Param_group, Param_spec>;

struct Bool_semantics;
struct List_semantics;
struct Float_semantics;
struct Int_semantics;
using Value_semantics = std::variant<Bool_semantics, List_semantics, Float_semantics, Int_semantics>;

template<typename T>
concept Some_semantics = is_variant_alternative<T, Value_semantics>::value;

template<Some_semantics Vs>
struct Knob_adapter {
    using Value_transform = std::function<double(const Vs&, double)>;

    Value_transform plain_to_norm{[](const Vs&, double) {
        static_assert(deferred_false_v<Vs>, "Your parameter needs a knob adapter.");
        return 0;
    }};

    Value_transform norm_to_plain{[](const Vs&, double) {
        static_assert(deferred_false_v<Vs>, "Your parameter needs a knob adapter.");
        return 0;
    }};
};

// Unit hints for float semantics parameters.
enum class Units : uint32_t {
    generic = 0,
    percent,
    decibels,
    hertz,
};

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
    }
}

struct Bool_semantics {
    bool def_val{true};
    Knob_adapter<Bool_semantics> knob_adapter; // required.
};

struct List_semantics {
    std::vector<const char*> labels{"One", "Two", "Three", "Four"};
    size_t def_val{};
    Knob_adapter<List_semantics> knob_adapter; // required.
};

struct Float_semantics {
    double min_val{};
    double def_val{0.5f};
    double max_val{1};
    Units units{};
    Knob_adapter<Float_semantics> knob_adapter; // required.
};

struct Int_semantics {
    int32_t min_val{};
    int32_t def_val{};
    int32_t max_val{1};
    Units units{};
    Knob_adapter<Int_semantics> knob_adapter; // required.
};

// MARK: - knob adapters

struct Knob_adapters {
    // 
    static auto make_bool() -> Knob_adapter<Bool_semantics>
    {
        return {
            .plain_to_norm = [](auto&&, double value) {
                return value;
            },
            .norm_to_plain = [](auto&&, double value) {
                return value;
            }
        };
    }

    // 
    static auto make_list() -> Knob_adapter<List_semantics>
    {
        return {
            .plain_to_norm = [](auto&& l, double value) {
                const auto step_count = static_cast<double>(l.labels.size() - 1);
                return value / step_count;
            },
            .norm_to_plain = [](auto&& l, double value) {
                const auto step_count = static_cast<double>(l.labels.size() - 1);
                return std::floor(std::min(step_count, value * (step_count + 1)));
            }
        };
    }

    // 
    static auto make_linear() -> Knob_adapter<Float_semantics>
    {
        return {
            .plain_to_norm = [](auto&& f, double value) {
                return (value - f.min_val) / (f.max_val - f.min_val);
            },
            .norm_to_plain = [](auto&& f, double value) {
                return (f.max_val - f.min_val) * value + f.min_val;
            }
        };
    }

    //
    static auto make_power(double exponent) -> Knob_adapter<Float_semantics>
    {
        return {
            .plain_to_norm = [=](auto&& f, double value) {
                const auto lin = (value - f.min_val) / (f.max_val - f.min_val);
                return std::pow(lin, 1 / exponent);
            },
            .norm_to_plain = [=](auto&& f, double value) {
                const auto lin = (f.max_val - f.min_val) * value + f.min_val;
                return std::pow(lin, exponent);
            }
        };
    }

    //
    static auto make_log() -> Knob_adapter<Float_semantics>
    {
        return {
            .plain_to_norm = [](auto&& f, double value) {
                const auto log_min = std::log2(f.min_val);
                const auto k = std::log2(f.max_val) - log_min;
                return (std::log2(value) - log_min) / k;
            },
            .norm_to_plain = [](auto&& f, double value) {
                const auto log_min = std::log2(f.min_val);
                const auto k = std::log2(f.max_val) - log_min;
                return std::exp2(k * value + log_min);
            }
        };
    }

    // 
    static auto make_tapered(double taper, bool bipolar) -> Knob_adapter<Float_semantics>
    {
        return {
            .plain_to_norm = [=](auto&& f, double value) {
                return normalized(value, f.min_val, f.max_val, taper, bipolar);
            },
            .norm_to_plain = [=](auto&& f, double value) {
                return denormalized(value, f.min_val, f.max_val, taper, bipolar);
            }
        };
    }

    struct Break_point { double plain{};  double norm{}; };

    static auto make_piecewise(const std::vector<Break_point>& interior) -> Knob_adapter<Float_semantics>
    {
        return {
            .plain_to_norm = [=](auto&& f, double value) -> double {
                if (value <= f.min_val) return 0;
                if (value >= f.max_val) return 1;

                if (interior.empty() || value <= interior.front().plain) {
                    const auto x0 = f.min_val;
                    const auto y0 = 0;
                    const auto x1 = interior.empty() ? f.max_val : interior.front().plain;
                    const auto y1 = interior.empty() ? 1 : interior.front().norm;
                    const auto t = (value - x0) / (x1 - x0);
                    return y0 + t * (y1 - y0);
                }

                for (size_t i = 1; i < interior.size(); ++i) {
                    const auto& a = interior[i - 1];
                    const auto& b = interior[i];
                    if (value <= b.plain) {
                        const auto t = (value - a.plain) / (b.plain - a.plain);
                        return a.norm + t * (b.norm - a.norm);
                    }
                }

                const auto& last = interior.back();
                if (value <= f.max_val) {
                    const auto t = (value - last.plain) / (f.max_val - last.plain);
                    return last.norm + t * (1 - last.norm);
                }

                return 1;
            },

            .norm_to_plain = [=](auto&& f, double norm) -> double {
                if (norm <= 0) return f.min_val;
                if (norm >= 1) return f.max_val;

                if (interior.empty() || norm <= interior.front().norm) {
                    const auto x0 = 0;
                    const auto y0 = f.min_val;
                    const auto x1 = interior.empty() ? 1 : interior.front().norm;
                    const auto y1 = interior.empty() ? f.max_val : interior.front().plain;
                    const auto t = (norm - x0) / (x1 - x0);
                    return y0 + t * (y1 - y0);
                }

                for (size_t i = 1; i < interior.size(); ++i) {
                    const auto& a = interior[i - 1];
                    const auto& b = interior[i];
                    if (norm <= b.norm) {
                        const auto t = (norm - a.norm) / (b.norm - a.norm);
                        return a.plain + t * (b.plain - a.plain);
                    }
                }

                const auto& last = interior.back();
                if (norm <= 1) {
                    double t = (norm - last.norm) / (1 - last.norm);
                    return last.plain + t * (f.max_val - last.plain);
                }

                return f.max_val;
            }
        };
    }

    static auto make_discrete() -> Knob_adapter<Int_semantics>
    {
        return {
            .plain_to_norm = [](auto&& i, double value) {
                const auto step_count = static_cast<double>(i.max_val - i.min_val);
                return (value - i.min_val) / step_count;
            },
            .norm_to_plain = [](auto&& i, double value) {
                const auto step_count = static_cast<double>(i.max_val - i.min_val);
                return std::floor(std::min(step_count, value * (step_count + 1))) + i.min_val;
            }
        };
    }
};

// MARK: - param group, spec

struct Param_group {
    const char* name{""};
    std::vector<Param_node> nodes{};
};

struct Param_spec {
    uint32_t id{};
    uint32_t version{};
    const char* name{""};
    const char* short_name{""};
    Value_semantics semantics{Float_semantics{.knob_adapter = Knob_adapters::make_linear()}};
    bool hidden{};
};

// MARK: - space

struct Value_conv {
    /*
        Semantics    Implies Linear?    Plain Space         Host Space         Knob Space
        ------------------------------------------------------------------------------
        Bool         Yes                0...1               0...1              0...1
        List         Yes                0...(size - 1)      0...(size - 1)     0...1
        Float        No                 min...max           0...1              0...1
        Int          Yes                min...max           min...max          0...1
    */
    
    // plain <--> host
    static auto plain_to_host(double plain_value, const Value_semantics& semantics) -> double
    {
        return std::visit(
            Inline_visitor{
                [=](const Bool_semantics&) { return plain_value; },
                [=](const List_semantics&) { return plain_value; },
                [=](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, plain_value); },
                [=](const Int_semantics&) { return plain_value; }
            }
        , semantics);
    }

    static auto host_to_plain(double host_value, const Value_semantics& semantics) -> double
    {
        return std::visit(
            Inline_visitor{
                [=](const Bool_semantics&) { return host_value; },
                [=](const List_semantics&) { return host_value; },
                [=](const Float_semantics& f) { return f.knob_adapter.norm_to_plain(f, host_value); },
                [=](const Int_semantics&) { return host_value; }
            }
        , semantics);
    }

    // host <--> knob
    static auto host_to_knob(double host_value, const Value_semantics& semantics) -> double
    {
        return std::visit(
            Inline_visitor{
                [=](const Bool_semantics&) { return host_value; },
                [=](const List_semantics& l) { return l.knob_adapter.plain_to_norm(l, host_value); },
                [=](const Float_semantics&) { return host_value; },
                [=](const Int_semantics& i) { return i.knob_adapter.plain_to_norm(i, host_value); }
            }
        , semantics);
    }

    static auto knob_to_host(double knob_value, const Value_semantics& semantics) -> double
    {
        return std::visit(
            Inline_visitor{
                [=](const Bool_semantics&) { return knob_value; },
                [=](const List_semantics& l) { return l.knob_adapter.norm_to_plain(l, knob_value); },
                [=](const Float_semantics&) { return knob_value; },
                [=](const Int_semantics& i) { return i.knob_adapter.norm_to_plain(i, knob_value); }
            }
        , semantics);
    }

    // knob <--> plain
    static auto knob_to_plain(double knob_value, const Value_semantics& semantics) -> double
    {
        return std::visit(
            Inline_visitor{
                [=](const Bool_semantics&) { return knob_value; },
                [=](const List_semantics& l) { return l.knob_adapter.norm_to_plain(l, knob_value); },
                [=](const Float_semantics& f) { return f.knob_adapter.norm_to_plain(f, knob_value); },
                [=](const Int_semantics& i) { return i.knob_adapter.norm_to_plain(i, knob_value); }
            }
        , semantics);
    }

    static auto plain_to_knob(double plain_value, const Value_semantics& semantics) -> double
    {
        return std::visit(
            Inline_visitor{
                [=](const Bool_semantics&) { return plain_value; },
                [=](const List_semantics& l) { return l.knob_adapter.plain_to_norm(l, plain_value); },
                [=](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, plain_value); },
                [=](const Int_semantics& i) { return i.knob_adapter.plain_to_norm(i, plain_value); }
            }
        , semantics);
    }
};

// MARK: - defaults

inline auto get_host_default(const Param_spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List_semantics& l) { return static_cast<double>(l.def_val); },
        [&](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, f.def_val); },
        [&](const Int_semantics& i) { return static_cast<double>(i.def_val); }
    }, spec.semantics);
}

inline auto get_knob_default(const Param_spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List_semantics& l) { return l.knob_adapter.plain_to_norm(l, l.def_val); },
        [&](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, f.def_val); },
        [&](const Int_semantics& i) { return i.knob_adapter.plain_to_norm(i, i.def_val); }
    }, spec.semantics);
}

inline auto get_plain_default(const Param_spec& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List_semantics& l) { return static_cast<double>(l.def_val); },
        [&](const Float_semantics& f) { return f.def_val; },
        [&](const Int_semantics& i) { return static_cast<double>(i.def_val); }
    }, spec.semantics);
}

// MARK: - parameter model

enum class Export_type; // See `tiny_exports.h`

template<typename T>
concept Some_param_model = requires {
    // An enum class `Param_id` with a case `num_params`
    typename T::Param_id;
    requires Enum<typename T::Param_id>;
    requires std::same_as<std::underlying_type_t<typename T::Param_id>, uint32_t>;

    // An enum class `Export_id` with a case `num_exports`
    typename T::Export_id;
    requires Enum<typename T::Export_id>;
    requires std::same_as<std::underlying_type_t<typename T::Export_id>, uint32_t>;

    // A static function `build_tree` that returns a `Param_node<Param_id>`
    { T::build_tree() } -> std::same_as<Param_node>;

    // A static function for `export_type` for looking up the export type.
    { T::export_type(std::declval<typename T::Export_id>()) } -> std::same_as<Export_type>;
};

// MARK: - params impl

namespace params_impl {

inline auto flatten_tree(const Param_node& root) -> std::vector<Param_spec>
{
    auto result = std::vector<Param_spec>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit(
            Inline_visitor{
                [&](const Param_spec& spec) { result.push_back(spec); },
                [&](const Param_group& group) { for (const auto& n : group.nodes) self(n, self); }
            }
        , node);
    };

    visit(root, visit);
    return result;
}

inline auto validate_spec(const Param_spec& spec) -> bool
{
    auto in_range = [](auto x, auto a, auto b) -> bool { return a <= x && x <= b; };
    auto ok_range = std::visit(
        Inline_visitor{
            [](const Bool_semantics&) { return true; },
            [](const List_semantics& l) { return l.def_val < l.labels.size(); },
            [&](const Float_semantics& f) { return in_range(f.def_val, f.min_val, f.max_val); },
            [&](const Int_semantics& i) { return in_range(i.def_val, i.min_val, i.max_val); }
        }
    , spec.semantics);
    TINY_ASSERT(ok_range, "Param default must be on a valid range.");
    return ok_range;
}

inline auto validate_tree(const Param_node& root, size_t num_expected) -> bool
{
    auto ids = std::unordered_set<uint32_t>{};

    // Recursive lambda that takes a const ref to itself for recursion
    const auto visit = [&](const Param_node& node, const auto& self) -> void {
        std::visit(
            Inline_visitor{
                [&](const Param_group& group) {
                    for (const auto& child : group.nodes) {
                        self(child, self);
                    }
                },
                [&](const Param_spec& spec) {
                    validate_spec(spec);
                    ids.insert(spec.id);
                }
            }
        , node);
    };

    visit(root, visit);

    const auto num_leaves = ids.size();
    TINY_ASSERT(num_leaves > 0, "The tree must contain at least one parameter.");

    const auto [min_val, max_val] = std::ranges::minmax_element(ids);
    TINY_ASSERT(*min_val == 0, "Identifiers must start at 0.");
    TINY_ASSERT(*max_val == num_leaves - 1, "Identifiers must not exceed (size - 1).");
    TINY_ASSERT(num_leaves == num_expected, "The parameter tree ");

    return true;
}

template<std::ranges::range R, typename Comp>
    requires std::sortable<std::ranges::iterator_t<R>, Comp>
inline auto sorted_copy(R&& range, Comp comp) -> decltype(auto)
{
    using T = std::ranges::range_value_t<R>;
    std::vector<T> copy(std::ranges::begin(range), std::ranges::end(range));
    std::ranges::sort(copy, comp);
    return copy;
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

template<typename T, size_t N, typename F>
constexpr auto make_array_by_indices(F f) -> std::array<T, N>
{
    return make_array_by_indices_impl<T>(f, std::make_index_sequence<N>{});
}

}

// MARK: - params

template<Some_param_model User_model>
class Param_infos {
public:
    //
    static constexpr auto num_params = enum_raw(User_model::Param_id::num_params);

    auto tree() const -> const Param_node&
    {
        return _tree;
    }

    auto presentation_specs() const -> const std::vector<Param_spec>&
    {
        return _pspecs;
    }

    auto kernel_specs() const -> const std::vector<Param_spec>&
    {
        return _kspecs;
    }

    auto param_for(uint32_t id) const -> const Param_spec&
    {
        TINY_ASSERT(id < num_params, "Parameter ID out of range.");
        return _kspecs[id];
    }

    template<typename T>
    constexpr auto make_plain_defaults() const -> std::array<T, num_params>
    {
        return params_impl::make_array_by_indices<T, num_params>(
            [this](auto i) { return get_plain_default(param_for(i)); }
        );
    }

    template<typename T>
    constexpr auto make_host_defaults() const -> std::array<T, num_params>
    {
        return params_impl::make_array_by_indices<T, num_params>(
            [this](auto i) { return get_host_default(param_for(i)); }
        );
    }

    template<typename T>
    constexpr auto make_knob_defaults() const -> std::array<T, num_params>
    {
        return params_impl::make_array_by_indices<T, num_params>(
            [this](auto i) { return get_knob_default(param_for(i)); }
        );
    }

private:
    
    static constexpr auto id_less = [](const auto& a, const auto& b) { return a.id < b.id; };

    Param_node _tree = []() {
        const auto t = User_model::build_tree();
        const auto is_valid = params_impl::validate_tree(t, num_params);
        return is_valid ? t : Param_group{};
    }();
    
    std::vector<Param_spec> _pspecs{params_impl::flatten_tree(_tree)};
    std::vector<Param_spec> _kspecs{params_impl::sorted_copy(_pspecs, id_less)};

};

// MARK: - host formatter

struct Host_formatter {
    // 
    static auto format_string(double host_value, const Value_semantics& semantics, bool include_units = true) -> std::string
    {
        return std::visit(
            Inline_visitor{
                [&](const Bool_semantics&) {
                    return host_value > 0.5f ? std::string{"True"} : std::string{"False"};
                },
                [&](const List_semantics& l) {
                    const auto idx = static_cast<size_t>(host_value);
                    return std::string{l.labels[idx]};
                },
                [&](const Float_semantics& f) {
                    using enum Units;
                    const auto value = f.knob_adapter.norm_to_plain(f, host_value);
                    switch (f.units) {
                        case generic:
                            return std::format("{:.{}f}", value, 2);
                        case percent: {
                            const auto suffix = std::string{include_units ? " %" : ""};
                            return std::format("{:.{}f}", value, 0) + suffix;
                        }
                        case decibels: {
                            const auto prefix = std::string{value >= 0 ? "+" : ""};
                            const auto suffix = std::string{include_units ? " dB" : ""};
                            return prefix + std::format("{:.{}f}", value, 1) + suffix;
                        }
                        case hertz: {
                            // If the host doesn't want us to include units, we should just send back the plain value in Hertz.
                            if (value > 1000 && include_units) {
                                const auto suffix = std::string{include_units ? " kHz" : ""};
                                return std::format("{:.{}f}", value / 1000, 1) + suffix;
                            }
                            else {
                                const auto suffix = std::string{include_units ? " Hz" : ""};
                                return std::format("{:.{}f}", value, 0) + suffix;
                            }
                        }
                        default:
                            return std::string{};
                    }
                },
                [&](const Int_semantics&) {
                    return std::format("{:.0f}", host_value); // TODO: Units
                }
            }
        , semantics);
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