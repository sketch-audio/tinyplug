#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <type_traits>
#include <variant>
#include <vector>

#include "tiny_utils.h"

namespace tiny::params {

// MARK: - types

template<Enum Id> struct Param_group;
template<Enum Id> struct Param_spec;

template<Enum Id>
using Param_node = std::variant<Param_group<Id>, Param_spec<Id>>;

struct Bool;
struct List;
struct Float;
using Value_semantics = std::variant<Bool, List, Float>;

template<typename T>
concept Some_value_semantics = is_variant_alternative<T, Value_semantics>::value;

template<Some_value_semantics Vs>
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

struct Bool {
    bool def_val{true};
    Knob_adapter<Bool> knob_adapter; // required.
};

struct List {
    std::vector<const char*> labels{"One", "Two", "Three", "Four"};
    size_t def_val{};
    Knob_adapter<List> knob_adapter; // required.
};

struct Float {
    double min_val{};
    double def_val{0.5f};
    double max_val{1};
    Units units{};
    Knob_adapter<Float> knob_adapter; // required.
};

// MARK: - knob adapters

struct Knob_adapters {
    // 
    static auto make_bool() -> Knob_adapter<Bool>
    {
        return {
            .plain_to_norm = [](const Bool& /*b*/, double value) {
                return value;
            },
            .norm_to_plain = [](const Bool& /*b*/, double value) {
                return value;
            }
        };
    }

    // 
    static auto make_list() -> Knob_adapter<List>
    {
        return {
            .plain_to_norm = [](const List& l, double value) {
                const auto step_count = static_cast<double>(l.labels.size() - 1);
                return value / step_count;
            },
            .norm_to_plain = [](const List& l, double value) {
                const auto step_count = static_cast<double>(l.labels.size() - 1);
                return std::floor(std::min(step_count, value * (step_count + 1)));
            }
        };
    }

    // 
    static auto make_linear() -> Knob_adapter<Float>
    {
        return {
            .plain_to_norm = [](const Float& f, double value) {
                return (value - f.min_val) / (f.max_val - f.min_val);
            },
            .norm_to_plain = [](const Float& f, double value) {
                return (f.max_val - f.min_val) * value + f.min_val;
            }
        };
    }

    // 
    static auto make_tapered(double taper, bool bipolar) -> Knob_adapter<Float>
    {
        return {
            .plain_to_norm = [=](const Float& f, double value) {
                return utils::normalized(value, f.min_val, f.max_val, taper, bipolar);
            },
            .norm_to_plain = [=](const Float& f, double value) {
                return utils::denormalized(value, f.min_val, f.max_val, taper, bipolar);
            }
        };
    }
};

struct Version {
    size_t version{1};
    static constexpr auto deprecated = std::numeric_limits<size_t>::max();
};

// MARK: - param group, spec

template<Enum Id>
struct Param_group {
    const char* name{""};
    std::vector<Param_node<Id>> nodes{};
};

template<Enum Id>
struct Param_spec {
    Id id{};
    Version version{};
    const char* name{""};
    const char* short_name{""};
    Value_semantics semantics{Float{.knob_adapter = Knob_adapters::make_linear()}};
    bool hidden{};
};

// MARK: - tree + sort

template<Enum Id>
inline auto flatten_tree(const Param_node<Id>& root) -> std::vector<Param_spec<Id>>
{
    auto result = std::vector<Param_spec<Id>>{};

    const auto visit = [&](const auto& node, const auto& self) -> void {
        std::visit([&](const auto& item) {
            if constexpr (std::is_same_v<std::decay_t<decltype(item)>, Param_spec<Id>>) {
                result.push_back(item);
            } else {
                for (const auto& child : item.nodes) {
                    self(child, self);
                }
            }
        }, node);
    };

    visit(root, visit);
    return result;
}

template<Enum Id>
inline auto sort_param_specs_by_id(std::vector<Param_spec<Id>>& specs) -> void
{
    std::ranges::sort(specs, [](const auto& a, const auto& b) {
        return utils::to_underlying(a.id) < utils::to_underlying(b.id);
    });
}

// MARK: - space

template<Enum Id>
inline auto plain_to_host_space(double plain_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool&) { return plain_value; },
        [&](const List&) { return plain_value; },
        [&](const Float& f) { return f.knob_adapter.plain_to_norm(f, plain_value); }
    }, spec.semantics);
}

template<Enum Id>
inline auto plain_to_knob_space(double plain_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool&) { return plain_value; },
        [&](const List& l) { return l.knob_adapter.plain_to_norm(l, plain_value); },
        [&](const Float& f) { return f.knob_adapter.plain_to_norm(f, plain_value); }
    }, spec.semantics);
}

template<Enum Id>
inline auto knob_to_host_space(double knob_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool&) { return knob_value; },
        [&](const List& l) { return l.knob_adapter.norm_to_plain(l, knob_value); },
        [&](const Float&) { return knob_value; }
    }, spec.semantics);
}

template<Enum Id>
inline auto knob_to_plain_space(double knob_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool&) { return knob_value; },
        [&](const List& l) { return l.knob_adapter.norm_to_plain(l, knob_value); },
        [&](const Float& f) { return f.knob_adapter.norm_to_plain(f, knob_value); }
    }, spec.semantics);
}

// MARK: - default

template<Enum Id>
inline auto get_host_default(const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List& l) { return static_cast<double>(l.def_val); },
        [&](const Float& f) { return f.knob_adapter.plain_to_norm(f, f.def_val); }
    }, spec.semantics);
}

template<Enum Id>
inline auto get_knob_default(const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List& l) { return l.knob_adapter.plain_to_norm(l, l.def_val); },
        [&](const Float& f) { return f.knob_adapter.plain_to_norm(f, f.def_val); }
    }, spec.semantics);
}

// MARK: - parameter model

template<typename T>
concept Is_param_model = requires(T model) {
    typename T::Param_id;
    requires Enum<typename T::Param_id>;
    { model.build_tree() } -> std::same_as<Param_node<typename T::Param_id>>;
};

}