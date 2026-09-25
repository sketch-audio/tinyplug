#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new> // std::hardware_destructive_interference_size
#include <type_traits> // std::is_trivially_copyable_v

namespace tiny {

enum class Port_direction : std::uint32_t { Main = 0, Audio };

// Double-buffered SPSC data port. Reader sees latest value.
template<typename T, Port_direction D = Port_direction::Main>
class Data_port;

namespace detail {

// Hot fields are kept a full line apart by padding, not alignas. An over-aligned member makes its
// owner over-aligned, and not every allocator honours that: the AU SDK constructs its instance in
// malloc'd storage, and Objective-C objects are malloc'd. Spacing works at any alignment.
inline constexpr auto line = std::hardware_destructive_interference_size;
using Line_pad = std::array<std::byte, line>;

template<typename T>
struct Slot {
    static_assert(std::is_trivially_copyable_v<T>);
    T value{};
    Line_pad after{};
};

} // namespace detail

// Writer is wait-free.
template<typename T>
class Data_port<T, Port_direction::Main> {
public:

    auto write(const T& value) -> void
    {
        const auto index = _control.fetch_or(FLAG_BUSY, std::memory_order_acquire) & MASK_INDEX;
        _storage[index].value = value;
        _control.store((index & MASK_INDEX) | FLAG_DIRTY, std::memory_order_release);
    }

    // Copies into `out` only if a write landed since the last read. Returns whether it did.
    auto read_fresh(T& out) -> bool
    {
        auto curr = _control.load(std::memory_order_acquire);
        if ((curr & FLAG_DIRTY) == 0) return false;

        auto next = std::uint32_t{};
        do {
            curr &= ~FLAG_BUSY;
            next = (curr ^ MASK_INDEX) & MASK_INDEX;
        } while (!_control.compare_exchange_weak(curr, next, std::memory_order_acq_rel, std::memory_order_relaxed));

        out = _storage[(next & MASK_INDEX) ^ MASK_INDEX].value;
        return true;
    }

    auto read() -> T
    {
        auto curr = _control.load(std::memory_order_acquire);

        if ((curr & FLAG_DIRTY) != 0) {
            auto next = std::uint32_t{};
            do {
                curr &= ~FLAG_BUSY;
                next = (curr ^ MASK_INDEX) & MASK_INDEX;
            } while (!_control.compare_exchange_weak(curr, next, std::memory_order_acq_rel, std::memory_order_relaxed));

            curr = next;
        }

        // We use the opposite index of the writer.
        return _storage[(curr & MASK_INDEX) ^ MASK_INDEX].value;
    }

private:

    static constexpr auto MASK_INDEX = std::uint32_t{1 << 0};
    static constexpr auto FLAG_BUSY = std::uint32_t{1 << 1};
    static constexpr auto FLAG_DIRTY = std::uint32_t{1 << 2};

    detail::Line_pad _before{};
    std::atomic<std::uint32_t> _control{};
    detail::Line_pad _after_control{};
    std::array<detail::Slot<T>, 2> _storage{};

};

// Reader is wait-free.
template<typename T>
class Data_port<T, Port_direction::Audio> {
public:

    auto read() -> T
    {
        const auto index = _control.fetch_or(FLAG_BUSY, std::memory_order_acquire) & MASK_INDEX;
        const auto value = _storage[index].value;
        _control.fetch_and(~FLAG_BUSY, std::memory_order_release);

        return value;
    }

    auto write(const T& value) -> void
    {
        auto curr = _control.load(std::memory_order_acquire);

        // We use the opposite index of the reader.
        _storage[(curr & MASK_INDEX) ^ MASK_INDEX].value = value;

        auto next = std::uint32_t{};
        do {
            curr &= ~FLAG_BUSY;
            next = (curr ^ MASK_INDEX) & MASK_INDEX;
        } while (!_control.compare_exchange_weak(curr, next, std::memory_order_acq_rel, std::memory_order_relaxed));
    }

private:

    // We don't need dirty because writer moves index.
    static constexpr auto MASK_INDEX = std::uint32_t{1 << 0};
    static constexpr auto FLAG_BUSY = std::uint32_t{1 << 1};

    detail::Line_pad _before{};
    std::atomic<std::uint32_t> _control{};
    detail::Line_pad _after_control{};
    std::array<detail::Slot<T>, 2> _storage{};

};

static_assert(alignof(Data_port<std::uint64_t>) <= alignof(std::max_align_t), "Data_port must not make its owner over-aligned.");
static_assert(alignof(Data_port<std::uint64_t, Port_direction::Audio>) <= alignof(std::max_align_t), "Data_port must not make its owner over-aligned.");

} // namespace tiny
