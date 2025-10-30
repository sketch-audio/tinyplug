#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>

namespace tiny {

template<typename F>
struct Deferred {
    F fn;

    explicit Deferred(F f) : fn(std::move(f)) {}
    ~Deferred() { fn(); }

    // no copy, no move
    Deferred(const Deferred&) = delete;
    Deferred& operator=(const Deferred&) = delete;
    Deferred(Deferred&&) = delete;
    Deferred& operator=(Deferred&&) = delete;
};

template<typename I = size_t, typename R, typename F>
void enumerate(R&& range, F&& func) {
    auto i = I{};
    for (auto&& elem : range) {
        func(i, elem);
        ++i;
    }
}

// CORE

template<typename T>
struct deferred_false : std::false_type {};

template<typename T>
inline constexpr auto deferred_false_v = deferred_false<T>::value;

template <typename T>
concept Enum = std::is_enum_v<T>;

template<typename... Ts>
struct Inline_visitor : Ts... {
    using Ts::operator()...;
};

template<typename T, typename Variant>
struct is_variant_alternative;

template<typename T, typename... Alts>
struct is_variant_alternative<T, std::variant<Alts...>> : std::disjunction<std::is_same<T, Alts>...> {};

template<Enum E>
constexpr auto enum_raw(E e) noexcept -> std::underlying_type_t<E>
{
    using U = std::underlying_type_t<E>;
    return static_cast<U>(e);
}

namespace utils_impl {
template<typename X, typename B>
inline auto normalized(X x, B in_lo, B in_hi, B taper = 0.5f) -> X
{
    const auto lin_map = (x - in_lo) / (in_hi - in_lo);
    const auto t = 1 / taper - 1;
    const auto b = t * t;
    const auto a = 1 / (b - 1);

    const auto arg = (lin_map + a) / a;
    const auto pow_map = std::log2(arg) / std::log2(b); // log base b

    return taper == 0.5f ? lin_map : pow_map;
}

template<typename X, typename B>
inline auto denormalized(X x, B out_lo, B out_hi, B taper = 0.5f) -> X
{
    const auto t = 1 / taper - 1;
    const auto b = t * t;
    const auto a = 1 / (b - 1);
    const auto y = a * std::pow(X(b), x) - a;

    const auto lin_map = (out_hi - out_lo) * x + out_lo;
    const auto pow_map = (out_hi - out_lo) * y + out_lo;

    return taper == 0.5f ? lin_map : pow_map;
}
}

template<typename X, typename B>
inline auto normalized(X x, B in_lo, B in_hi, B taper = 0.5f, bool bipolar = false) -> X
{
    if (bipolar) {
        const auto midpoint = (in_lo + in_hi) / 2;
        const auto cond = x >= midpoint;

        const auto xu = utils_impl::normalized(x, midpoint, in_hi, taper);
        const auto yu = (xu + 1) / 2;

        const auto xl = (x - in_lo) / (midpoint - in_lo);
        const auto refl = -1 * (xl - 1);
        const auto unwp = utils_impl::normalized(refl, B(0), B(1), taper);
        const auto yl = (unwp - 1) / -2;

        return cond ? yu : yl;
    }
    else {
        return utils_impl::normalized(x, in_lo, in_hi, taper);
    }
}

template<typename X, typename B>
inline auto denormalized(X x, B out_lo, B out_hi, B taper = 0.5f, bool bipolar = false) -> X
{
    if (bipolar) {
        const auto midpoint = (out_lo + out_hi) / 2;
        const auto cond = x >= 0.5f;

        const auto xu = 2 * x - 1;
        const auto yu = utils_impl::denormalized(xu, midpoint, out_hi, taper);

        const auto xl = -2 * x + 1;
        const auto warp = utils_impl::denormalized(xl, B(0), B(1), taper);
        const auto refl = -1 * warp + 1;
        const auto yl = (midpoint - out_lo) * refl + out_lo;

        return cond ? yu : yl;
    }
    else {
        return utils_impl::denormalized(x, out_lo, out_hi, taper);
    }
}

}