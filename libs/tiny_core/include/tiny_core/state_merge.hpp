#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace tiny::state {

// A patch is a {base, next} pair: what the author diffed from and what they produced.
// Applying it writes only the bytes that moved, so a concurrent writer elsewhere in the
// object survives a stale edit. Returns how many written bytes someone else had changed
// since `base` was taken; advisory, the patch is applied regardless.
inline auto merge(std::byte* dst, const std::byte* base, const std::byte* next, std::size_t n) -> std::size_t
{
    auto conflicts = std::size_t{};
    constexpr auto W = sizeof(std::uint64_t);

    auto i = std::size_t{};
    while (i + W <= n) {
        auto b = std::uint64_t{};
        auto x = std::uint64_t{};
        std::memcpy(&b, base + i, W);
        std::memcpy(&x, next + i, W);
        if (b == x) { i += W; continue; } // Skip unchanged words.

        for (auto j = i; j < i + W; ++j) {
            if (base[j] == next[j]) continue;
            if (dst[j] != base[j]) ++conflicts;
            dst[j] = next[j];
        }
        i += W;
    }

    for (; i < n; ++i) {
        if (base[i] == next[i]) continue;
        if (dst[i] != base[i]) ++conflicts;
        dst[i] = next[i];
    }

    return conflicts;
}

// How many bytes `merge` would write that someone else has moved since `base`. Scoped to
// the patch's footprint, so a writer working elsewhere in the object is never a conflict.
inline auto drift(const std::byte* dst, const std::byte* base, const std::byte* next, std::size_t n) -> std::size_t
{
    auto moved = std::size_t{};
    constexpr auto W = sizeof(std::uint64_t);

    auto i = std::size_t{};
    while (i + W <= n) {
        auto b = std::uint64_t{};
        auto x = std::uint64_t{};
        std::memcpy(&b, base + i, W);
        std::memcpy(&x, next + i, W);
        if (b == x) { i += W; continue; }

        for (auto j = i; j < i + W; ++j) {
            if (base[j] == next[j]) continue;
            if (dst[j] != base[j]) ++moved;
        }
        i += W;
    }

    for (; i < n; ++i) {
        if (base[i] == next[i]) continue;
        if (dst[i] != base[i]) ++moved;
    }

    return moved;
}

template<typename T>
auto as_bytes(const T& v) -> const std::byte*
{
    return reinterpret_cast<const std::byte*>(&v);
}

template<typename T>
auto as_writable_bytes(T& v) -> std::byte*
{
    return reinterpret_cast<std::byte*>(&v);
}

template<typename T>
auto merge_into(T& dst, const T& base, const T& next) -> std::size_t
{
    static_assert(std::is_trivially_copyable_v<T>);
    return merge(as_writable_bytes(dst), as_bytes(base), as_bytes(next), sizeof(T));
}

template<typename T>
auto drift_from(const T& dst, const T& base, const T& next) -> std::size_t
{
    static_assert(std::is_trivially_copyable_v<T>);
    return drift(as_bytes(dst), as_bytes(base), as_bytes(next), sizeof(T));
}

// True when equal values are guaranteed equal bytes. False for anything holding floats
// (-0.0, NaN payloads); then padding and float identity are the author's to keep stable.
template<typename T>
inline constexpr auto byte_comparable = std::has_unique_object_representations_v<T>;

} // namespace tiny::state
