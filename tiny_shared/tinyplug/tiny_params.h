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
template<Enum Id> struct Knob_adapter;
template<Enum Id> struct Knob_adapters; // factory

template<Enum Id>
using Param_node = std::variant<Param_group<Id>, Param_spec<Id>>;

//
enum class Units : uint32_t {
    generic = 0,
    boolean,
    indexed,
    percent,
    decibels,
    hertz,
};

// TODO: -
struct Version {
    size_t version{1};
    static constexpr auto deprecated = std::numeric_limits<size_t>::max();
};

//
template<Enum Id>
struct Param_group {
    const char* name{""};
    std::vector<Param_node<Id>> nodes{};
};

// MARK: - param spec

template<Enum Id>
struct Param_spec {
    //
    Id id{};
    const char* name{""};
    const char* short_name{""};
    double min_val{};
    double max_val{1};
    double def_val{0.5f};
    bool discrete{}; // possibly derive this from units?
    bool hidden{};
    Units units{};
    std::vector<const char*> labels{}; // 
    std::vector<Id> dep_ids{};
    Knob_adapter<Id> knob_adapter{Knob_adapters<Id>::make_linear()};

    // To provide labels, set your units to `indexed` and make sure your `labels` vector size matches your range.
    auto provides_labels() const -> bool
    {
        const auto num_values = max_val - min_val + 1;
        return units == Units::indexed && labels.size() == num_values;
    }
};

// MARK: - control adapter

template<Enum Id>
struct Knob_adapter {
    using Value_transform = std::function<double(const Param_spec<Id>&, double)>;
    Value_transform plain_to_norm{};
    Value_transform norm_to_plain{};
};

template<Enum Id>
struct Knob_adapters {
    //
    static auto make_linear() -> Knob_adapter<Id>
    {
        return {
            .plain_to_norm = [](const Param_spec<Id>& param, double value) {
                return (value - param.min_val) / (param.max_val - param.min_val);
            },
            .norm_to_plain = [](const Param_spec<Id>& param, double value) {
                return (param.max_val - param.min_val) * value + param.min_val;
            }
        };
    }

    //
    static auto make_discrete() -> Knob_adapter<Id>
    {
        return {
            .plain_to_norm = [](const Param_spec<Id>& param, double value) {
                const auto step_count = param.max_val - param.min_val;
                return value / step_count;
            },
            .norm_to_plain = [](const Param_spec<Id>& param, double value) {
                const auto step_count = param.max_val - param.min_val;
                return std::floor(std::min(step_count, value * (step_count + 1)));
            }
        };
    }

    //
    static auto make_tapered(double taper, bool bipolar) -> Knob_adapter<Id>
    {
        return {
            .plain_to_norm = [=](const Param_spec<Id>& param, double value) {
                return utils::normalized(value, param.min_val, param.max_val, taper, bipolar);
            },
            .norm_to_plain = [=](const Param_spec<Id>& param, double value) {
                return utils::denormalized(value, param.min_val, param.max_val, taper, bipolar);
            }
        };
    }
};

// MARK: - functions

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

inline auto linearize(double value, Units from_units) -> double 
{
    using enum Units;
    switch (from_units) {
        case generic: return value;
        case boolean: return value;
        case indexed: return value;
        case percent: return value;
        case decibels: return value;
        case hertz: return std::log2(std::max(value, 1e-6));
    }
}

inline auto delinearize(double value, Units to_units) -> double
{
    using enum Units;
    switch (to_units) {
        case generic: return value;
        case boolean: return value;
        case indexed: return value;
        case percent: return value;
        case decibels: return value;
        case hertz: return std::exp2(value);
    }
}

inline auto units_string(Units units) -> std::string {
    switch (units) {
        case Units::generic:
            return "";
        case Units::boolean:
            return "";
        case Units::indexed:
            return "";
        case Units::percent:
            return "%";
        case Units::decibels:
            return "dB";
        case Units::hertz:
            return "Hz";
        default:
            return "";
    }
}

// MARK: - parameter model

template<typename T>
concept Is_param_model = requires(T model) {
    typename T::Param_id;
    requires Enum<typename T::Param_id>;
    { model.build_tree() } -> std::same_as<Param_node<typename T::Param_id>>;
};

}