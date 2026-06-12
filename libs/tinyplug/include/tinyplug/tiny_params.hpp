#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <variant>
#include <vector>

#include "tiny_utils.hpp"

namespace tiny::params {

// MARK: - Units

// Unit display hints.
enum class Units : uint32_t {
    Generic = 0,
    Percent,
    Decibels,
    Hertz,
    Milliseconds,
    Degrees
};

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

// A node in a parameter tree.
using Node = std::variant<Group, Spec>;

// A named group of nodes.
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
    Semantics::Any semantics{std::in_place_type<Semantics::Real>};

    // Host policy.
    Policy policy{Policy::Automation};

    // Regular.
    auto operator==(const Spec&) const -> bool = default;
};

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

namespace impl {

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

} // namespace impl

enum class Param_order : uint32_t { Indexable, Presentation };

// MARK: - params registry

template<Model User_model>
class Infos {
public:

    static constexpr auto num_params = enum_raw(User_model::Address::Num_params);

    static auto param_tree() -> const Node&
    {
        // Validate once at startup.
        [[maybe_unused]] static const auto validated = [] {
            [[maybe_unused]] const auto is_valid = impl::validate_tree(user_tree, num_params);
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

private:

    static constexpr auto id_less = [](const auto& a, const auto& b) { return a.address < b.address; };

    inline static const Node user_tree = User_model::build_tree();
    inline static const std::vector<Spec> display_specs = impl::flatten_tree(user_tree);
    inline static const std::vector<Spec> indexed_specs = impl::sorted_copy(display_specs, id_less);

};

} // namespace tiny::params
