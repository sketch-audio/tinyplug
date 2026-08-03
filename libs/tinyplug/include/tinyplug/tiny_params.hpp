#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <ranges>
#include <string>
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
        std::vector<std::string> items{"One", "Two", "Three"};

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

// A parameter's permanent identity. Frozen the moment a plug-in ships.
struct Identity {
    // The parameter's unique address. The key for persistence and host automation.
    uint32_t address{};

    // Leaf identifier. The AUv3 keyPath and the preset key are this plus the group ancestry.
    // Must be non-empty and unique among siblings.
    std::string identifier{};

    // Regular.
    auto operator==(const Identity&) const -> bool = default;
};

// A named group of nodes.
struct Group {
    // The group name. Display only — free to change after shipping.
    std::string name{""};

    // Frozen — part of every descendant's keyPath and preset path. Unique among siblings.
    // The root group is exempt; it contributes nothing to either path.
    std::string identifier{};

    // The group nodes.
    std::vector<Node> nodes{};
};

// A specification for a parameter.
struct Spec {
    // The parameter's permanent identity.
    Identity identity{};

    // Name. Display only — free to change after shipping.
    std::string name{""};

    // Short name. Display only. (Optional)
    std::string short_name{""};

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

// A model that pins its own AUv2 parameter list order.
// Logic addresses AUv2 automation by index into that list rather than by id, so the list has to
// be append-only across releases. The tree can't carry that guarantee — it also encodes display
// order, and inserting a parameter next to its siblings shifts everything after it. Declaring the
// order separately lets the tree stay free while the AU list stays frozen. Without this, AUv2
// falls back to address order, which is append-only too but gives up control of AU display order.
template<typename T>
concept Au_ordered = Model<T> && requires {
    { T::au_order() } -> std::same_as<std::vector<typename T::Address>>;
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

// Every identifier in the tree is a permanence surface — it spells the AUv3 keyPath and the
// preset key. A missing or duplicated one is silent breakage, so it's checked at startup.
inline auto validate_tree(const Node& root, [[maybe_unused]] size_t num_expected) -> bool
{
    auto ids = std::unordered_set<uint32_t>{};
    auto keypaths = std::unordered_set<std::string>{};

    // `is_root` because the root group contributes to no keypath and needs no identifier.
    const auto visit = [&](const auto& node, const std::string& prefix, bool is_root, const auto& self) -> void {
        auto siblings = std::unordered_set<std::string>{};
        std::visit(Inline_visitor{
            [&](const Spec& spec) {
                validate_spec(spec);
                ids.insert(spec.identity.address);
                [[maybe_unused]] const auto unique = keypaths.insert(prefix + spec.identity.identifier).second;
                assert(unique && "Param keypaths must be unique.");
            },
            [&](const Group& group) {
                [[maybe_unused]] const auto named = is_root || !group.identifier.empty();
                assert(named && "Param groups must have an identifier.");
                const auto path = is_root ? std::string{} : prefix + group.identifier + "/";
                for (const auto& child : group.nodes) {
                    [[maybe_unused]] const auto& child_id = std::visit(Inline_visitor{
                        [](const Spec& s) -> const std::string& { return s.identity.identifier; },
                        [](const Group& g) -> const std::string& { return g.identifier; }
                    }, child);
                    assert(!child_id.empty() && "Param identifiers must not be empty.");
                    [[maybe_unused]] const auto unique = siblings.insert(child_id).second;
                    assert(unique && "Param identifiers must be unique among siblings.");
                    self(child, path, false, self);
                }
            }
        }, node);
    };

    visit(root, std::string{}, true, visit);

    const auto num_leaves = ids.size();
    assert(num_leaves == num_expected && "Param tree must contain all params.");

    if (num_leaves == 0) return true; // Empty tree is valid.

    [[maybe_unused]] const auto [min_val, max_val] = std::ranges::minmax_element(ids);
    assert(*min_val == 0 && "Min param id must be zero.");
    assert(*max_val == num_leaves - 1 && "Max param id must be num_params - 1.");

    return true;
}

// The AU list must name every parameter exactly once, or hosts index into the wrong one.
template<Enum Address>
inline auto validate_au_order(const std::vector<Address>& order, [[maybe_unused]] size_t num_expected) -> bool
{
    auto seen = std::unordered_set<uint32_t>{};
    for (const auto addr : order) {
        [[maybe_unused]] const auto raw = enum_raw(addr);
        assert(raw < num_expected && "au_order() names an address outside the model.");
        [[maybe_unused]] const auto unique = seen.insert(raw).second;
        assert(unique && "au_order() names the same address twice.");
    }
    assert(seen.size() == num_expected && "au_order() must name every parameter exactly once.");
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

// The model's pinned AU order if it declares one, otherwise address order.
// Address order — never tree order — because the `Address` enum is already append-only, so it is
// safe by construction for a model that hasn't opted in. Falling back to tree order would leave
// presentation order load-bearing, which is the whole problem `au_order()` exists to remove.
template<Model User_model>
inline auto au_ordered_copy(const std::vector<Spec>& indexed_specs) -> std::vector<Spec>
{
    if constexpr (Au_ordered<User_model>) {
        const auto order = User_model::au_order();
        [[maybe_unused]] const auto is_valid = validate_au_order(order, indexed_specs.size());

        auto out = std::vector<Spec>{};
        out.reserve(order.size());
        for (const auto addr : order) {
            const auto raw = enum_raw(addr);
            if (raw < indexed_specs.size()) out.push_back(indexed_specs[raw]);
        }
        return out;
    }
    else {
        return indexed_specs;
    }
}

} // namespace impl

// Indexable is address order, Presentation is tree order, Au_ordinal is the AUv2 list order.
enum class Param_order : uint32_t { Indexable, Presentation, Au_ordinal };

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
        switch (ordering) {
            case Param_order::Presentation: return display_specs;
            case Param_order::Au_ordinal:   return au_specs;
            case Param_order::Indexable:    return indexed_specs;
            default:                        return indexed_specs;
        }
    }

    static auto param_spec(uint32_t address) -> const Spec&
    {
        assert(address < num_params && "Param address out of range.");
        return indexed_specs[address];
    }

private:

    static constexpr auto id_less = [](const auto& a, const auto& b) { return a.identity.address < b.identity.address; };

    inline static const Node user_tree = User_model::build_tree();
    inline static const std::vector<Spec> display_specs = impl::flatten_tree(user_tree);
    inline static const std::vector<Spec> indexed_specs = impl::sorted_copy(display_specs, id_less);
    inline static const std::vector<Spec> au_specs = impl::au_ordered_copy<User_model>(indexed_specs);

};

} // namespace tiny::params
