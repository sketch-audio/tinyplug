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
                return utils::normalized(value, f.min_val, f.max_val, taper, bipolar);
            },
            .norm_to_plain = [=](auto&& f, double value) {
                return utils::denormalized(value, f.min_val, f.max_val, taper, bipolar);
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
    Value_semantics semantics{Float_semantics{.knob_adapter = Knob_adapters::make_linear()}};
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
        [&](const Bool_semantics&) { return plain_value; },
        [&](const List_semantics&) { return plain_value; },
        [&](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, plain_value); },
        [&](const Int_semantics&) { return plain_value; }
    }, spec.semantics);
}

template<Enum Id>
inline auto plain_to_knob_space(double plain_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) { return plain_value; },
        [&](const List_semantics& l) { return l.knob_adapter.plain_to_norm(l, plain_value); },
        [&](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, plain_value); },
        [&](const Int_semantics& i) { return i.knob_adapter.plain_to_norm(i, plain_value); }
    }, spec.semantics);
}

template<Enum Id>
inline auto knob_to_host_space(double knob_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) { return knob_value; },
        [&](const List_semantics& l) { return l.knob_adapter.norm_to_plain(l, knob_value); },
        [&](const Float_semantics&) { return knob_value; },
        [&](const Int_semantics& i) { return i.knob_adapter.norm_to_plain(i, knob_value); }
    }, spec.semantics);
}

template<Enum Id>
inline auto knob_to_plain_space(double knob_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) { return knob_value; },
        [&](const List_semantics& l) { return l.knob_adapter.norm_to_plain(l, knob_value); },
        [&](const Float_semantics& f) { return f.knob_adapter.norm_to_plain(f, knob_value); },
        [&](const Int_semantics& i) { return i.knob_adapter.norm_to_plain(i, knob_value); }
    }, spec.semantics);
}

template<Enum Id>
inline auto host_to_plain_space(double host_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) { return host_value; },
        [&](const List_semantics&) { return host_value; },
        [&](const Float_semantics& f) { return f.knob_adapter.norm_to_plain(f, host_value); },
        [&](const Int_semantics&) { return host_value; }
    }, spec.semantics);
}

template<Enum Id>
inline auto host_to_knob_space(double host_value, const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics&) { return host_value; },
        [&](const List_semantics& l) { return l.knob_adapter.plain_to_norm(l, host_value); },
        [&](const Float_semantics&) { return host_value; },
        [&](const Int_semantics& i) { return i.knob_adapter.plain_to_norm(i, host_value); }
    }, spec.semantics);
}

// MARK: - default

template<Enum Id>
inline auto get_host_default(const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List_semantics& l) { return static_cast<double>(l.def_val); },
        [&](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, f.def_val); },
        [&](const Int_semantics& i) { return static_cast<double>(i.def_val); }
    }, spec.semantics);
}

template<Enum Id>
inline auto get_knob_default(const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List_semantics& l) { return l.knob_adapter.plain_to_norm(l, l.def_val); },
        [&](const Float_semantics& f) { return f.knob_adapter.plain_to_norm(f, f.def_val); },
        [&](const Int_semantics& i) { return i.knob_adapter.plain_to_norm(i, i.def_val); }
    }, spec.semantics);
}

template<Enum Id>
inline auto get_plain_default(const Param_spec<Id>& spec) -> double
{
    return std::visit(Inline_visitor{
        [&](const Bool_semantics& b) { return static_cast<double>(b.def_val ? 1 : 0); },
        [&](const List_semantics& l) { return static_cast<double>(l.def_val); },
        [&](const Float_semantics& f) { return f.def_val; },
        [&](const Int_semantics& i) { return static_cast<double>(i.def_val); }
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