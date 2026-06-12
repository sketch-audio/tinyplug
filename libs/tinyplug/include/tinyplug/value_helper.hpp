#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "tiny_params.hpp"
#include "tiny_utils.hpp"

namespace tiny::params {

// MARK: - Value space

// The three spaces a parameter value can live in. See Value_helper.
enum class Space : uint32_t { Plain, Host, Knob };

// MARK: - Value helper

// All value-domain logic for a parameter: space conversions, range/default
// queries, and semantics predicates. Stateless; every method takes the
// parameter's semantics (or spec). Successor to the old Value_conv.
//
//     Semantics    Implies Linear?    Plain Space         Host Space         Knob Space
//     -----------------------------------------------------------------------------------------
//     Bool         Yes                0...1               0...1              0...1
//     List         Yes                0...(size - 1)      0...(size - 1)     0...1
//     Int          Yes                min...max           min...max          0...1
//     Fixed        Yes                min...max           min...max          0...1
//     Real         No                 min...max           0...1              0...1
//
// The DSP kernel sees plain; the host sees host; the UI draws in knob (0...1).
// Knob space and the old "norm" space are the same thing.
struct Value_helper {

    // --- Real-adapter normalization (knob == norm) ---

    // Normalize a real-semantics plain value to knob space via its adapter.
    static auto plain_to_knob(double plain_value, const Semantics::Real& real) -> double;

    // Denormalize a knob value to real-semantics plain space via its adapter.
    static auto knob_to_plain(double knob_value, const Semantics::Real& real) -> double;

    // --- Conversions across the three spaces ---

    // Convert a plain value to knob space.
    static auto plain_to_knob(double plain_value, const Semantics::Any& semantics) -> double;

    // Convert a knob value to plain space.
    static auto knob_to_plain(double knob_value, const Semantics::Any& semantics) -> double;

    // Convert a plain value to host space.
    static auto plain_to_host(double plain_value, const Semantics::Any& semantics) -> double;

    // Convert a host value to plain space.
    static auto host_to_plain(double host_value, const Semantics::Any& semantics) -> double;

    // Convert a host value to knob space.
    static auto host_to_knob(double host_value, const Semantics::Any& semantics) -> double;

    // Convert a knob value to host space.
    static auto knob_to_host(double knob_value, const Semantics::Any& semantics) -> double;

    // Convert a value between any two spaces.
    static auto convert(double value, Space from, Space to, const Semantics::Any& semantics) -> double;

    // Quantize a plain value onto its step grid (round-trips through knob space).
    static auto quantize(double plain_value, const Semantics::Any& semantics) -> double;

    // --- Spec-level range / defaults ---

    // Minimum plain value.
    static auto plain_min(const Spec& spec) -> double;

    // Maximum plain value.
    static auto plain_max(const Spec& spec) -> double;

    // Default value of `spec` expressed in `space`.
    static auto default_value(const Spec& spec, Space space) -> double;

    // --- Semantics queries ---

    // Whether the parameter takes only discrete values.
    static auto is_discrete(const Semantics::Any& semantics) -> bool;

    // Whether the parameter carries the given display units.
    static auto has_units(Units units, const Semantics::Any& semantics) -> bool;

    // Display string for `units` (e.g. "dB", "Hz", "%").
    static auto units_label(Units units) -> std::string;

    // --- Clamp / step ---

    // Clamp a plain value to the parameter's range.
    static auto clamp(double value, const Semantics::Any& semantics) -> double;

    // Advance a knob value to the next discrete step (wraps); continuous for Real.
    static auto knob_next(double knob_value, const Semantics::Any& semantics) -> double;
};

// MARK: - array builders

namespace detail {

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

} // namespace detail

// Build a std::array<T, N> whose element i is f(i). Supports T = std::atomic<...>.
template<typename T, size_t N, typename F>
constexpr auto make_array_by_indices(F f) -> std::array<T, N>
{
    return detail::make_array_by_indices_impl<T>(f, std::make_index_sequence<N>{});
}

// Build a per-parameter array of defaults in `space`.
// `Infos` is a params::Infos<Model> instantiation.
template<typename T, typename Infos>
auto make_defaults(Space space) -> std::array<T, Infos::num_params>
{
    return make_array_by_indices<T, Infos::num_params>(
        [space](auto i) {
            return Value_helper::default_value(Infos::param_spec(static_cast<uint32_t>(i)), space);
        }
    );
}

} // namespace tiny::params
