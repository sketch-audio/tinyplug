#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <unordered_map>

#include "tiny_events.h"

namespace tiny {

struct Change_list {

    using Map = std::unordered_map<uint32_t, double>;

    explicit Change_list(size_t capacity = 128) {
        for (auto& map : _changes) {
            map.reserve(2 * capacity); // We can't guarantee no rehashing but we can try.
        }
    }

    auto push(const Set_param& event) -> void
    {
        do_push([&](auto& target) {
            target.insert_or_assign(event.address, event.value);
        });
    }

    auto push_n(std::span<const Set_param> events) -> void
    {
        do_push([&](auto& target) {
            for (const auto& event : events) {
                target.insert_or_assign(event.address, event.value);
            }
        });
    }

    template<typename F>
    auto consume(F&& on_change) -> void
    {
        const auto curr = _spare.load(std::memory_order_relaxed);
        const auto updated = curr.updated;

        if (updated) {
            // In order to read, we need to make sure we're not merging.
            auto expected = curr;
            const auto desired = Control{_read, false, false};

            // Possibly avoid the loop
            if (!_spare.compare_exchange_weak(expected, desired, std::memory_order_acq_rel)) {
                return; // For now, we'll just try again later (UI thread could be clearing a large map).
                // do {
                //     expected.merging = false;
                // } while(!_spare.compare_exchange_weak(expected, desired, std::memory_order_acq_rel));
            }
            _read = expected.index;

            // Process
            auto& read_map = _changes[_read];
            for (const auto& [address, value] : read_map) {
                on_change(address, value);
            }
        }
    }

private:

    struct Control {
        uint32_t index{};
        bool updated{};
        bool merging{};
    };
    static_assert(std::atomic<Control>::is_always_lock_free);

    std::mutex _push{};
    std::array<Map, 3> _changes{};

    uint32_t _read{0};
    std::atomic<Control> _spare{{1}};
    uint32_t _write{2};

    template<typename F>
    auto do_push(F&& on_push) -> void
    {
        auto lock = std::scoped_lock{_push};

        // We have to lock out the audio thread. We'll try to be quick.
        const auto check = _spare.exchange(Control{.merging = true}, std::memory_order_acq_rel);

        // If the audio thread hasn't consumed anything yet, write into the spare map.
        if (check.updated) {
            auto& spare_map = _changes[check.index];
            on_push(spare_map);
            // Put it back.
            _spare.store(check, std::memory_order_release);
        }
        // Otherwise, write into the write map then swap.
        else {
            auto& write_map = _changes[_write];
            write_map.clear();
            on_push(write_map);
            // Publish
            const auto next = Control{_write, true, false};
            _spare.store(next, std::memory_order_release);
            _write = check.index;
        }
    }
};

} // namespace tiny