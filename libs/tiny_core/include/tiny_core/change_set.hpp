#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <type_traits>

namespace tiny {

// Who may push. `One` drops the producer mutex: for a producer that may be the audio thread,
// such as CLAP's `paramsFlush`, and never runs concurrently with itself.
enum class Producers : std::uint8_t { One, Many };

namespace detail {

// One value per address, and a dirty bit per address so an empty consume costs N/64 loads.
template<std::uint32_t N>
struct Dense {
    static constexpr auto words = std::size_t{(N + 63) / 64};

    std::array<double, N> values{};
    std::array<std::uint64_t, words> dirty{};

    auto set(std::uint32_t address, double value) -> void
    {
        values[address] = value;
        dirty[address / 64] |= std::uint64_t{1} << (address % 64);
    }

    auto clear() -> void { dirty.fill(0); }

    template<typename F>
    auto for_each(F&& on_change) const -> std::size_t
    {
        auto count = std::size_t{};
        for (auto w = std::size_t{}; w < words; ++w) {
            auto bits = dirty[w];
            while (bits != 0) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(bits));
                bits &= bits - 1;

                const auto address = static_cast<std::uint32_t>(w * 64) + bit;
                on_change(address, values[address]);
                ++count;
            }
        }
        return count;
    }
};

} // namespace detail

// The latest value per address, for one consumer that must never wait (the audio thread).
// Fixed memory, no allocation after construction, and it cannot overflow: repeated writes
// to an address collapse to the newest. `push_n` lands as a unit, so a consumer sees a whole
// batch or none of it, which is what a preset restore needs.
//
// Two buffers. The consumer holds the one it last took; the other is the producer's, either
// pending (unread, so the producer merges into it) or free (read, so the producer starts it
// fresh). A coalescing set never needs a third: it never starts fresh while a batch waits.
// Templated on the event type so each instance names its value space.
template<typename Event, std::uint32_t N, Producers P = Producers::Many>
class Change_set {
public:

    static constexpr auto num_addresses = N;

    auto push(const Event& event) -> void
    {
        do_push([&](auto& target) { put(target, event); });
    }

    auto push_n(std::span<const Event> events) -> void
    {
        if (events.empty()) return;
        do_push([&](auto& target) {
            for (const auto& event : events) put(target, event);
        });
    }

    // [consumer] Deliver the pending batch as `on_change(address, value)`. Never waits: a
    // batch being merged into right now is left for the next call. True if anything arrived.
    template<typename F>
    auto consume(F&& on_change) -> bool
    {
        auto state = _state.load(std::memory_order_acquire);
        if ((state & PENDING) == 0 || (state & MERGING) != 0) return false;

        const auto taken = other(state);
        if (!_state.compare_exchange_strong(state, taken, std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return false; // A merge began.
        }
        return _buffers[taken].for_each(std::forward<F>(on_change)) > 0;
    }

private:

    // The low bit is the buffer the consumer holds; the pending buffer is always the other one.
    static constexpr auto READ = std::uint32_t{1};
    static constexpr auto PENDING = std::uint32_t{2};
    static constexpr auto MERGING = std::uint32_t{4};

    static auto other(std::uint32_t state) -> std::uint32_t { return (state & READ) ^ READ; }

    struct Nothing {};

    std::array<detail::Dense<N>, 2> _buffers{};
    std::atomic<std::uint32_t> _state{0};
    [[no_unique_address]] std::conditional_t<P == Producers::Many, std::mutex, Nothing> _push{};

    static auto put(detail::Dense<N>& target, const Event& event) -> void
    {
        assert(event.address < N && "Change_set address out of range.");
        if (event.address < N) target.set(event.address, event.value);
    }

    template<typename F>
    auto do_push(F&& on_push) -> void
    {
        using Lock = std::conditional_t<P == Producers::Many, std::scoped_lock<std::mutex>, Nothing>;
        [[maybe_unused]] const auto lock = Lock{_push};

        auto state = _state.load(std::memory_order_acquire);
        for (;;) {
            if ((state & PENDING) != 0) {
                // Hold the pending batch so the consumer can't take it half-merged. Failing
                // means the consumer took it first: reload and start fresh instead.
                if (!_state.compare_exchange_weak(state, state | MERGING, std::memory_order_acq_rel, std::memory_order_acquire)) {
                    continue;
                }
                on_push(_buffers[other(state)]);
                _state.store(state, std::memory_order_release);
                return;
            }

            // Nothing pending, so the consumer can't move: the other buffer is free, and read
            // before the consumer took the one it holds.
            auto& target = _buffers[other(state)];
            target.clear();
            on_push(target);
            _state.store(state | PENDING, std::memory_order_release);
            return;
        }
    }

};

} // namespace tiny
