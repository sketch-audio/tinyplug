#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <span>
#include <type_traits>

namespace tiny::meters {

// Linear range adapter.
struct Range {
    double min_val{0.};
    double max_val{1.};
};

// Framework policy.
enum class Policy : std::uint32_t {
    Stream = 0, // Editor receives the latest value.
    Peak,       // Editor receives the max unconsumed value.
    Trig,       // Editor receives the non-zero value as an event, may be coalesced or dropped.
};

struct Spec {
    Range range{};
    Policy policy{};
};

template<typename T>
concept Model = requires {
    // Number of meters. Implies runtime-stable addresses 0..<num_meters.
    typename std::integral_constant<std::uint32_t, T::num_meters>;

    // Called for each address in 0..<num_meters, on the audio thread every block: keep it cheap and pure.
    { T::make_spec(std::uint32_t{}) } -> std::same_as<Spec>;
};

// Fallback when the plug-in has no `models/meters.hpp`.
struct None {
    static constexpr auto num_meters = std::uint32_t{0};
    static constexpr auto make_spec(std::uint32_t) -> Spec { return {}; }
};

// Specs cached once per model, indexable by address.
template<Model M>
class Infos {
public:

    static constexpr auto num_meters = M::num_meters;

    static auto specs() -> const std::array<Spec, num_meters>&
    {
        return _specs;
    }

    static auto spec(std::uint32_t address) -> const Spec&
    {
        assert(address < num_meters && "Meter address out of range.");
        return _specs[address];
    }

private:

    inline static const std::array<Spec, num_meters> _specs = [] {
        auto arr = std::array<Spec, num_meters>{};
        for (auto i = std::uint32_t{}; i < num_meters; ++i) {
            arr[i] = M::make_spec(i);
        }
        return arr;
    }();

};

// MARK: - Publisher

// Processor helper
template<Model M>
class Publisher {
public:

    using Value = float;

    auto scratch() -> std::span<Value>
    {
        return {_scratch.data(), _scratch.size()};
    }

    template<typename Send>
    auto publish(bool suspend, Send&& send) -> void
    {
        for (auto i = std::uint32_t{}; i < num_meters; ++i) {
            const auto policy = M::make_spec(i).policy;

            if (!suspend) {
                switch (policy) {
                    case Policy::Stream:
                        _publish_stream(i, send);
                        break;
                    case Policy::Peak:
                        _publish_peak(i, send);
                        break;
                    case Policy::Trig:
                        _publish_trig(i, send);
                        break;
                    default:
                        break;
                }
            }

            // Reset even when publishing is suspended.
            const auto reset = (policy == Policy::Peak) || (policy == Policy::Trig);
            if (reset) {
                _scratch[i] = 0;
            }
        }
    }

private:

    static constexpr auto num_meters = M::num_meters;

    std::array<Value, num_meters> _scratch{};
    std::array<Value, num_meters> _previous{}; // Deduplication (stream).
    std::array<Value, num_meters> _held{};     // Peaks not yet delivered (e.g. transport refused).

    template<typename Send>
    auto _publish_stream(std::uint32_t addr, Send& send) -> void
    {
        // Deduplicate with previous and update previous if delivered.
        const auto value = _scratch[addr];
        if (value == _previous[addr]) return;
        const auto sent = send(addr, value);
        if (sent) {
            _previous[addr] = value;
        }
    }

    template<typename Send>
    auto _publish_peak(std::uint32_t addr, Send& send) -> void
    {
        // Max with held. If delivered, reset held, otherwise save.
        auto& held = _held[addr];
        const auto value = std::max(held, _scratch[addr]);
        const auto sent = send(addr, value);
        held = sent ? 0 : value;
    }

    template<typename Send>
    auto _publish_trig(std::uint32_t addr, Send& send) -> void
    {
        const auto value = _scratch[addr];
        if (value != 0) {
            send(addr, value); // Processor can send a level.
        }
    }
};

// MARK: - Mailbox

enum class Transport : std::uint32_t {
    Framework = 0,
    Host           // Implies host handles thread-safety.
};

// Editor helper.
template<Model M, Transport T = Transport::Framework>
class Mailbox;

// We own the transport and handle the thread-safety.
template<Model M>
class Mailbox<M, Transport::Framework> {
public:

    using Value = float;

    auto post(std::uint32_t addr, Value value) -> void
    {
        auto& slot = _slots[addr];

        const auto policy = M::make_spec(addr).policy;

        switch (policy) {
            case Policy::Stream: {
                auto curr = slot.load(std::memory_order_relaxed);
                slot.store(Slot{value, curr.count + 1}, std::memory_order_release);
                break;
            }
            case Policy::Peak: {
                auto curr = slot.load(std::memory_order_relaxed);
                auto next = Slot{};
                do {
                    next = Slot{value > curr.value ? value : curr.value, curr.count + 1};
                } while (!slot.compare_exchange_weak(curr, next, std::memory_order_release, std::memory_order_relaxed));
                break;
            }
            case Policy::Trig: {
                if (value == 0) break;
                auto curr = slot.load(std::memory_order_relaxed);
                slot.store(Slot{value, curr.count + 1}, std::memory_order_release);
                break;
            }
            default:
                break;
        }
    }

    auto read(std::span<Value> out) -> void
    {
        assert(out.size() == num_meters && "Output span size must match number of meters.");

        for (auto i = std::uint32_t{}; i < num_meters; ++i) {
            auto& slot = _slots[i];

            const auto policy = M::make_spec(i).policy;

            switch (policy) {
                case Policy::Stream: {
                    out[i] = slot.load(std::memory_order_acquire).value;
                    break;
                }
                case Policy::Peak: {
                    auto curr = slot.load(std::memory_order_acquire);
                    while (!slot.compare_exchange_weak(curr, Slot{0, curr.count}, std::memory_order_acq_rel, std::memory_order_acquire)) {}

                    auto& seen = _seen[i];
                    if (curr.count != seen.count) {
                        seen = {curr.value, curr.count};
                    }
                    
                    out[i] = seen.value;
                    break;
                }
                case Policy::Trig: {
                    auto curr = slot.load(std::memory_order_acquire);
                    while(!slot.compare_exchange_weak(curr, Slot{0, curr.count}, std::memory_order_acq_rel, std::memory_order_acquire)) {}
                    out[i] = curr.value;
                    break;
                }
                default:
                    out[i] = Value{};
                    break;
            }
        }
    }

private:

    static constexpr auto num_meters = M::num_meters;

    struct Slot {
        Value value{};
        std::uint32_t count{};
    };
    static_assert(std::atomic<Slot>::is_always_lock_free);
    static_assert(sizeof(Slot) == sizeof(Value) + sizeof(std::uint32_t)); // No padding.

    std::array<std::atomic<Slot>, num_meters> _slots{};
    std::array<Slot, num_meters> _seen{}; // Peak meters need to remember the last observed slot.

};

// Host owns the transport and handles the thread-safety.
template<Model M>
class Mailbox<M, Transport::Host> {
public:

    using Value = float;

    auto post(std::uint32_t addr, Value value) -> void
    {
        auto& slot = _slots[addr];

        const auto policy = M::make_spec(addr).policy;

        switch (policy) {
            case Policy::Stream: {
                slot = Slot{value, slot.count + 1};
                break;
            }
            case Policy::Peak: {
                if (value > slot.value) slot.value = value;
                slot.count += 1;
                _latest[addr] = value;
                break;
            }
            case Policy::Trig: {
                if (value == 0) break;
                slot = Slot{value, slot.count + 1};
                break;
            }
            default:
                break;
        }
    }

    auto read(std::span<Value> out) -> void
    {
        assert(out.size() == num_meters && "Output span size must match number of meters.");

        for (auto i = std::uint32_t{}; i < num_meters; ++i) {
            auto& slot = _slots[i];

            const auto policy = M::make_spec(i).policy;

            switch (policy) {
                case Policy::Stream: {
                    out[i] = slot.value;
                    break;
                }
                case Policy::Peak: {
                    // Host may have dropped a duplicate, so we check if the count changed.
                    auto& seen = _seen[i];
                    out[i] = slot.count != seen ? slot.value : _latest[i];

                    seen = slot.count;
                    slot.value = 0;
                    break;
                }
                case Policy::Trig: {
                    out[i] = slot.value;
                    slot.value = 0;
                    break;
                }
                default:
                    out[i] = Value{};
                    break;
            }
        }
    }

private:

    static constexpr auto num_meters = M::num_meters;

    struct Slot {
        Value value{};
        std::uint32_t count{};
    };

    std::array<Slot, num_meters> _slots{};
    std::array<std::uint32_t, num_meters> _seen{}; // Peak: post count at the last read.
    std::array<Value, num_meters> _latest{};       // Peak: the last value the host delivered.

};

} // namespace tiny::meters
