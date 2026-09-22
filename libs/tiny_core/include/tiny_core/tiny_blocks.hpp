#pragma once

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "data_port.hpp"

namespace tiny::blocks {

// A block is a trivially copyable frame the processor publishes and the editor draws.
// Latest wins: frames the editor never saw are dropped, never queued.

struct Spec {
    std::size_t kind{}; // Index into the model's `Types`.
};

namespace detail {

template<typename T, typename V>
struct Kind_of;

template<typename T, typename... Ts>
struct Kind_of<T, std::variant<Ts...>> {
    static constexpr auto matches = std::array<bool, sizeof...(Ts)>{std::is_same_v<T, Ts>...};
    static constexpr auto count = (std::size_t{std::is_same_v<T, Ts>} + ...);
    static_assert(count == 1, "Frame type must appear exactly once in Types.");
    static constexpr auto value = [] {
        auto i = std::size_t{};
        while (!matches[i]) ++i;
        return i;
    }();
};

template<typename V>
struct All_trivially_copyable;

template<typename... Ts>
struct All_trivially_copyable<std::variant<Ts...>> {
    static constexpr auto value = (std::is_trivially_copyable_v<Ts> && ...);
};

} // namespace detail

// The `kind` of a frame type within a model's `Types`.
template<typename T, typename Types>
inline constexpr auto kind_of = detail::Kind_of<T, Types>::value;

template<typename T>
concept Model = requires {
    // Frame types, each trivially copyable.
    typename T::Types;
    requires detail::All_trivially_copyable<typename T::Types>::value;

    // Number of blocks. Implies runtime-stable addresses 0..<num_blocks.
    typename std::integral_constant<std::uint32_t, T::num_blocks>;

    // Must be constexpr: `kind` picks each address's frame type at compile time.
    typename std::integral_constant<std::size_t, T::make_spec(std::uint32_t{}).kind>;
};

// Fallback when the plug-in has no `models/blocks.hpp`.
struct None {
    struct Empty {};
    using Types = std::variant<Empty>;
    static constexpr auto num_blocks = std::uint32_t{0};
    static constexpr auto make_spec(std::uint32_t) -> Spec { return {}; }
};

// An address as a plain integer, from the author's enum or an integer.
template<auto A>
inline constexpr auto index_of = static_cast<std::uint32_t>(A);

// The frame type at address `I`.
template<Model M, std::uint32_t I>
using Frame_at = std::variant_alternative_t<M::make_spec(I).kind, typename M::Types>;

namespace detail {

template<Model M, template<typename> class Wrap, typename Seq = std::make_integer_sequence<std::uint32_t, M::num_blocks>>
struct Per_address;

template<Model M, template<typename> class Wrap, std::uint32_t... I>
struct Per_address<M, Wrap, std::integer_sequence<std::uint32_t, I...>> {
    using type = std::tuple<Wrap<Frame_at<M, I>>...>;

    // Calls `f(std::integral_constant<std::uint32_t, I>{})` for every address.
    template<typename F>
    static auto for_each(F&& f) -> void
    {
        (f(std::integral_constant<std::uint32_t, I>{}), ...);
    }
};

template<typename T> using Same = T;
template<typename T> using Port = Data_port<T, Port_direction::Main>;

} // namespace detail

// Calls `f(std::integral_constant<std::uint32_t, I>{})` for every address, so runtime code
// (a transport walking its addresses) can reach each frame's static type.
template<Model M, typename F>
auto for_each_address(F&& f) -> void
{
    detail::Per_address<M, detail::Same>::for_each(std::forward<F>(f));
}

// Specs cached once per model, indexable by address.
template<Model M>
class Infos {
public:

    static constexpr auto num_blocks = M::num_blocks;

    template<auto A>
    using Frame = Frame_at<M, index_of<A>>;

    static auto specs() -> const std::array<Spec, num_blocks>&
    {
        return _specs;
    }

    static auto spec(std::uint32_t address) -> const Spec&
    {
        assert(address < num_blocks && "Block address out of range.");
        return _specs[address];
    }

private:

    inline static const std::array<Spec, num_blocks> _specs = [] {
        auto arr = std::array<Spec, num_blocks>{};
        for (auto i = std::uint32_t{}; i < num_blocks; ++i) {
            arr[i] = M::make_spec(i);
        }
        return arr;
    }();

};

// MARK: - Frames

// Editor-side retained frames. Whatever arrived last stays readable; `fresh` marks this draw's arrivals.
template<Model M>
class Frames {
public:

    template<auto A>
    using Frame = Frame_at<M, index_of<A>>;

    template<auto A>
    auto latest() const -> const Frame<A>&
    {
        static_assert(index_of<A> < M::num_blocks, "Block address out of range.");
        return std::get<index_of<A>>(_frames);
    }

    // Null unless a new frame arrived since the previous draw.
    template<auto A>
    auto fresh() const -> const Frame<A>*
    {
        static_assert(index_of<A> < M::num_blocks, "Block address out of range.");
        return _fresh[index_of<A>] ? &std::get<index_of<A>>(_frames) : nullptr;
    }

private:

    template<Model> friend class Mailbox;

    using Table = detail::Per_address<M, detail::Same>;

    typename Table::type _frames{};
    std::array<bool, M::num_blocks> _fresh{};

};

// What the editor holds: a view onto the frames its view class retains.
template<Model M>
class View {
public:

    template<auto A>
    using Frame = Frame_at<M, index_of<A>>;

    View() = default;
    explicit View(const Frames<M>* frames) : _frames{frames} {}

    template<auto A>
    auto latest() const -> const Frame<A>& { return _frames->template latest<A>(); }

    template<auto A>
    auto fresh() const -> const Frame<A>* { return _frames->template fresh<A>(); }

private:

    const Frames<M>* _frames{};

};

// MARK: - Mailbox

// One lock-free port per address. Posting never waits; the reader spins only while a post is mid-copy.
template<Model M>
class Mailbox {
public:

    template<auto A>
    using Frame = Frame_at<M, index_of<A>>;

    template<std::uint32_t I>
    auto post(std::integral_constant<std::uint32_t, I>, const Frame_at<M, I>& frame) -> bool
    {
        std::get<I>(_ports).write(frame);
        return true;
    }

    // Copies only what arrived since the last read; marks each address fresh or not.
    auto read(Frames<M>& out) -> void
    {
        Table::for_each([&](auto i) {
            constexpr auto I = decltype(i)::value;
            out._fresh[I] = std::get<I>(_ports).read_fresh(std::get<I>(out._frames));
        });
    }

private:

    using Table = detail::Per_address<M, detail::Port>;

    typename Table::type _ports{};

};

// MARK: - Publisher

// Processor-side staging. Frames persist between publishes, so a producer may update incrementally.
template<Model M>
class Publisher {
public:

    template<auto A>
    using Frame = Frame_at<M, index_of<A>>;

    template<auto A>
    auto write() -> Frame<A>&
    {
        static_assert(index_of<A> < M::num_blocks, "Block address out of range.");
        return std::get<index_of<A>>(_staging);
    }

    template<auto A>
    auto publish() -> void
    {
        static_assert(index_of<A> < M::num_blocks, "Block address out of range.");
        _pending[index_of<A>] = true;
    }

    // Wrapper side, once per block. `send(address_constant, frame) -> bool`; a refused frame retries next block.
    template<typename Send>
    auto transmit(bool suspend, Send&& send) -> void
    {
        Table::for_each([&](auto i) {
            constexpr auto I = decltype(i)::value;
            if (!_pending[I]) return;
            _pending[I] = suspend ? false : !send(i, std::get<I>(_staging));
        });
    }

private:

    using Table = detail::Per_address<M, detail::Same>;

    typename Table::type _staging{};
    std::array<bool, M::num_blocks> _pending{};

};

// What the processor holds: write access to the wrapper's publisher.
template<Model M>
class Writer {
public:

    template<auto A>
    using Frame = Frame_at<M, index_of<A>>;

    Writer() = default;
    explicit Writer(Publisher<M>* publisher) : _publisher{publisher} {}

    // The staging frame for `A`. Contents persist until you overwrite them.
    template<auto A>
    auto write() const -> Frame<A>& { return _publisher->template write<A>(); }

    // Send the staging frame at the end of this block. Publishing twice in one block sends once.
    template<auto A>
    auto publish() const -> void { _publisher->template publish<A>(); }

private:

    Publisher<M>* _publisher{};

};

} // namespace tiny::blocks
