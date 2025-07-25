#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <limits>
#include <thread>
#include <utility>

// This is a detemplated version of the farbot queue, but only implements return false on full/empty.
// Original code: https://github.com/hogliux/farbot
// Algorithm here: https://natsys-lab.blogspot.com/2013/05/lock-free-multi-producer-multi-consumer.html?m=1
// See also: https://www.youtube.com/watch?v=PoZAo2Vikbo

namespace tiny {

enum class Queue_concurrency { spsc, spmc, mpsc, mpmc };

template <typename T, size_t min_slots, Queue_concurrency type = Queue_concurrency::spsc>
struct Lock_free_queue {};

namespace queue_impl {

constexpr auto pow2_at_least(size_t n) -> size_t
{
    auto result = size_t{1};
    while (result < n) {
        result *= 2;
    }
    return result;
}
static_assert(pow2_at_least(100) == 128);
static_assert(pow2_at_least(128) == 128);

}

// MARK: - spsc

template <typename T, size_t min_slots>
struct Lock_free_queue<T, min_slots, Queue_concurrency::spsc> {

    auto push(const T& value) -> bool
    {
        const auto pos = _wpos.load(std::memory_order_relaxed);

        if (pos >= _rpos.load(std::memory_order_acquire) + num_slots) {
            return false;
        }

        _storage[pos & GET_INDEX_MASK] = std::move(value);

        _wpos.store(pos + 1, std::memory_order_release);

        return true;
    }

    auto pop(T& output) -> bool
    {
        const auto pos = _rpos.load(std::memory_order_relaxed);

        if (pos >= _wpos.load(std::memory_order_acquire)) {
            return false;
        }

        output = std::move(_storage[pos & GET_INDEX_MASK]);

        _rpos.store(pos + 1, std::memory_order_release);

        return true;
    }

private:

    static constexpr auto num_slots = queue_impl::pow2_at_least(min_slots);
    static constexpr auto GET_INDEX_MASK = num_slots - 1;

    std::array<T, num_slots> _storage{};
    std::atomic<uint32_t> _rpos{};
    std::atomic<uint32_t> _wpos{};

};

// MARK: - thread registry

namespace queue_impl {

static constexpr auto queue_max_threads = size_t{64};

struct Thread_info {
    std::atomic<std::thread::id> identifier{};
    std::atomic<uint32_t> pos{std::numeric_limits<uint32_t>::max()};
    static_assert(std::atomic<std::thread::id>::is_always_lock_free);
};

template<size_t max_threads = queue_max_threads>
struct Thread_registry {
    std::atomic<uint32_t> num_threads{};
    std::array<Thread_info, max_threads> infos{};

    /// Get this thread's temporary position variable or add ourselves to the registry.
    auto get_own_position() -> std::atomic<uint32_t>&
    {
        const auto own_id = std::this_thread::get_id();
        const auto num = num_threads.load(std::memory_order_relaxed);

        for (auto i = decltype(num){}; i < num; ++i) {
            if (own_id == infos[i].identifier.load(std::memory_order_relaxed)) {
                return infos[i].pos;
            }
        }

        auto own_slot = num_threads.fetch_add(1, std::memory_order_relaxed);

        if (own_slot >= num_threads) {
            assert(false);
            own_slot = 0; // Something is wrong!
        }

        infos[own_slot].identifier.store(own_id, std::memory_order_relaxed);

        return infos[own_slot].pos;
    }

    /// Get the least position in the registry starting with `own`.
    auto get_least_position(uint32_t own) -> uint32_t
    {
        // Note that if a new thread gets added to the registry, its position
        // will necessarily be larger than what this function returns anyway.
        const auto num = num_threads.load(std::memory_order_relaxed);

        auto least = own;

        for (auto i = decltype(num){}; i < num; ++i) {
            least = std::min(least, infos[i].pos.load(std::memory_order_acquire));
        }

        return least;
    }
};

} // namespace queue_impl

// MARK: - spmc

template <typename T, size_t min_slots>
struct Lock_free_queue<T, min_slots, Queue_concurrency::spmc> {

    auto push(const T& value) -> bool
    {
        const auto pos = _wpos.load(std::memory_order_relaxed);

        // Make sure there aren't any readers in "scope" reading where we want to write.
        const auto least_rpos = reader_infos.get_least_position(_rpos.load(std::memory_order_acquire));
        if (pos >= least_rpos + num_slots) {
            return false;
        }

        _storage[pos & GET_INDEX_MASK] = std::move(value);

        _wpos.store(pos + 1, std::memory_order_release);

        return true;
    }

    // writer? single? overwrite?

    auto pop(T& output) -> bool
    {
        auto& temp = reader_infos.get_own_position();
        auto pos = _rpos.load(std::memory_order_relaxed);

        // Return if empty at this moment.
        if (pos >= _wpos.load(std::memory_order_acquire)) {
            return false;
        }

        // Attempt to pop. Enter "scope" with a "low enough" position.
        temp.store(pos, std::memory_order_release);

        // Possibly avoid loop with this if.
        if (!_rpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            do {
                // Possible someone else popped the last element.
                if (pos >= _wpos.load(std::memory_order_acquire)) {
                    temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_relaxed);
                    return false;
                }
            } while (!_rpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed));

            // Got our position.
            temp.store(pos, std::memory_order_release);
        }

        output = std::move(_storage[pos & GET_INDEX_MASK]);

        // Exit "scope" now that we're done reading.
        temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);

        return true;
    }

private:

    static constexpr auto num_slots = queue_impl::pow2_at_least(min_slots);
    static constexpr auto GET_INDEX_MASK = num_slots - 1;

    std::array<T, num_slots> _storage{};
    std::atomic<uint32_t> _rpos{};
    std::atomic<uint32_t> _wpos{};

    queue_impl::Thread_registry<> reader_infos{};

};

// MARK: - mpsc

template <typename T, size_t min_slots>
struct Lock_free_queue<T, min_slots, Queue_concurrency::mpsc> {

    auto push(const T& value) -> bool
    {
        auto& temp = writer_infos.get_own_position();
        auto pos = _wpos.load(std::memory_order_relaxed);

        // Return if full at this moment.
        if (pos >= _rpos.load(std::memory_order_acquire) + num_slots) {
            return false;
        }

        // Attempt to push. Enter "scope" with a "low enough" position.
        temp.store(pos, std::memory_order_release);

        // Possibly avoid loop with this if.
        if (!_wpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            do {
                // Possible someone else pushed and filled the queue.
                if (pos >= _rpos.load(std::memory_order_acquire) + num_slots) {
                    temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_relaxed);
                    return false;
                }
            } while (!_wpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed));

            // Got our position.
            temp.store(pos, std::memory_order_release);
        }

        _storage[pos & GET_INDEX_MASK] = std::move(value);

        // Exit "scope" now that we're done writing.
        temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);

        return true;
    }

    auto pop(T& output) -> bool
    {
        auto pos = _rpos.load(std::memory_order_relaxed);

        // Maker sure we're not going to pop from a position that is being written.
        const auto least_wpos = writer_infos.get_least_position(_wpos.load(std::memory_order_acquire));
        if (pos >= least_wpos) {
            return false;
        }

        output = std::move(_storage[pos & GET_INDEX_MASK]);

        _rpos.store(pos + 1, std::memory_order_release);

        return true;
    }

private:

    static constexpr auto num_slots = queue_impl::pow2_at_least(min_slots);
    static constexpr auto GET_INDEX_MASK = num_slots - 1;

    std::array<T, num_slots> _storage{};
    std::atomic<uint32_t> _rpos{};
    std::atomic<uint32_t> _wpos{};

    queue_impl::Thread_registry<> writer_infos{};

};

// MARK: - mpmc

template <typename T, size_t min_slots>
struct Lock_free_queue<T, min_slots, Queue_concurrency::mpmc> {

    auto push(const T& value) -> bool
    {
        auto& temp = writer_infos.get_own_position();
        auto pos = _wpos.load(std::memory_order_relaxed);

        // Make sure there aren't any readers in "scope" reading where we want to write.
        const auto least_rpos = reader_infos.get_least_position(_rpos.load(std::memory_order_acquire));
        if (pos >= least_rpos + num_slots) {
            return false;
        }

        // Attempt to push. Enter "scope" with a "low enough" position.
        temp.store(pos, std::memory_order_release);

        // Possibly avoid loop with this if.
        if (!_wpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            do {
                // Possible someone else pushed and filled the queue.
                if (pos >= least_rpos + num_slots) {
                    temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_relaxed);
                    return false;
                }
            } while (!_wpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed));

            // Got our position.
            temp.store(pos, std::memory_order_release);
        }

        _storage[pos & GET_INDEX_MASK] = std::move(value);

        // Exit "scope" now that we're done writing.
        temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);

        return true;
    }

    auto pop(T& output) -> bool
    {
        auto& temp = reader_infos.get_own_position();
        auto pos = _rpos.load(std::memory_order_relaxed);

        // Return if empty at this moment.
        const auto least_wpos = writer_infos.get_least_position(_wpos.load(std::memory_order_acquire));
        if (pos >= least_wpos) {
            return false;
        }

        // Attempt to pop. Enter "scope" with a "low enough" position.
        temp.store(pos, std::memory_order_release);

        // Possibly avoid loop with this if.
        if (!_rpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
            do {
                // Possible someone else popped the last element.
                //auto least_wpos = writer_infos.get_least_position(_wpos.load(std::memory_order_acquire));
                if (pos >= least_wpos) {
                    temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_relaxed);
                    return false;
                }
            } while (!_rpos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed));

            // Got our position.
            temp.store(pos, std::memory_order_release);
        }

        output = std::move(_storage[pos & GET_INDEX_MASK]);

        // Exit "scope" now that we're done reading.
        temp.store(std::numeric_limits<uint32_t>::max(), std::memory_order_release);

        return true;
    }

private:

    static constexpr auto num_slots = queue_impl::pow2_at_least(min_slots);
    static constexpr auto GET_INDEX_MASK = num_slots - 1;

    std::array<T, num_slots> _storage{};
    std::atomic<uint32_t> _rpos{};
    std::atomic<uint32_t> _wpos{};

    queue_impl::Thread_registry<> reader_infos{};
    queue_impl::Thread_registry<> writer_infos{};

};

} // namespace tiny