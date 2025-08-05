#pragma once

#include <concepts>
#include <functional>
#include <limits>
#include <variant>

namespace tiny {

struct Set_param {
    uint32_t id{};
    double value{};
};

struct Ramp_param {
    uint32_t id{};
    double target{};
    int32_t dur_samples{};
};

using Event = std::variant<Set_param, Ramp_param>;

struct Tagged_event {
    int32_t offset{std::numeric_limits<decltype(offset)>::max()}; // Frame offset in current buffer.
    Event event{};
};

// MARK: - UI events

struct Export_event {
    uint32_t id{};
    double value{};
};

//
template<typename F, typename R, typename... Args>
concept Callable =
    std::invocable<F, Args...> &&
    std::same_as<std::invoke_result_t<F, Args...>, R>;

template<typename F>
concept Pop_fn = Callable<F, bool, Export_event&>;

template<typename T>
concept Pop_interface = requires(T t) {
    { t.pop_fn } -> Pop_fn;
};

namespace events_impl {
template<Pop_fn P>
struct Interface {
    P pop_fn;
};
}

template<Pop_fn P>
    requires Pop_interface<events_impl::Interface<P>>
auto make_interface(P p)
{
    return events_impl::Interface{.pop_fn = p};
}

using Pop_export = std::function<bool(Export_event&)>;

} // namespace tiny