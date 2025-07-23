#ifndef lock_free_queue_h
#define lock_free_queue_h

#include <assert.h>

#include <array>
#include <atomic>

namespace lark {

/// @brief Queue on full behavior options.
enum class On_full_behavior { report, overwrite };

/// This is basically the boost queue.
template <typename T, size_t MIN_SIZE, On_full_behavior Behavior>
struct Lock_free_queue {

    auto push(T&& value) -> bool 
    {
        const auto curr = writePos.load(std::memory_order_relaxed);
        const auto next = curr + 1;

        if constexpr (Behavior == On_full_behavior::report) {
            if (curr >= readPos.load(std::memory_order_acquire) + N) {
                return false;
            }
        }

        storage[curr & INDEX_MASK] = std::move(value);

        writePos.store(next, std::memory_order_release);

        return true;
    }

    auto pop(T& output) -> bool 
    {
        const auto curr = readPos.load(std::memory_order_relaxed);
        const auto next = curr + 1;

        if (curr >= writePos.load(std::memory_order_acquire)) {
            return false;
        }

        output = std::move(storage[curr & INDEX_MASK]);

        readPos.store(next, std::memory_order_release);

        return true;
    }

private:
    
    static constexpr auto pow2_at_least(size_t x) {
        auto result = size_t{1};
        while (result < x) {
            result *= 2;
        }
        return result;
    }
    
    static constexpr auto N = pow2_at_least(MIN_SIZE);
    static constexpr auto INDEX_MASK = N - 1;

    std::atomic<size_t> readPos{};
    std::atomic<size_t> writePos{};

    std::array<T, N> storage{};
    
};

}

#endif /* lock_free_queue_h */
