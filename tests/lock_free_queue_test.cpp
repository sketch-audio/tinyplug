// Lock_free_queue in all four modes, and Overwrite_queue. Run under TSan too (the `tsan` preset).

#include <atomic>
#include <cstdint>
#include <format>
#include <latch>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tiny_core/lock_free_queue.hpp>

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::Lock_free_queue;
using tiny::Queue_concurrency;

struct Item {
    uint32_t producer{};
    uint32_t seq{};
};

constexpr auto slots = size_t{64};

template<Queue_concurrency mode>
using Queue = Lock_free_queue<Item, slots, mode>;

// MARK: - one thread

template<Queue_concurrency mode>
auto add_single_thread(const char* name) -> void
{
    Tests::add(std::format("{}: holds exactly its slot count, then refuses", name), [] {
        auto q = std::make_unique<Queue<mode>>();
        for (auto i = uint32_t{}; i < slots; ++i) expect_true(q->push(Item{0, i}), std::format("push {}", i));
        expect_true(!q->push(Item{0, 99}), "push past capacity must fail");
        auto out = Item{};
        expect_true(q->pop(out) && out.seq == 0, "a pop frees one slot");
        expect_true(q->push(Item{0, 64}), "then one push fits again");
    });

    Tests::add(std::format("{}: FIFO across many trips round the storage", name), [] {
        auto q = std::make_unique<Queue<mode>>();
        auto next_in = uint32_t{}, next_out = uint32_t{};
        auto out = Item{};
        expect_true(!q->pop(out), "a fresh queue is empty");
        for (auto round = 0; round < 100; ++round) {
            for (auto i = 0; i < 37; ++i) expect_true(q->push(Item{0, next_in++}), "push");
            while (q->pop(out)) {
                expect_true(out.seq == next_out, std::format("expected {}, got {}", next_out, out.seq));
                ++next_out;
            }
        }
        expect_true(next_out == next_in, "everything pushed came out");
    });
}

// MARK: - threads

// Producers retry on full, so nothing is ever dropped; each item carries (producer, seq).
template<Queue_concurrency mode>
auto run_threads(uint32_t num_producers, uint32_t num_consumers, uint32_t per_producer) -> std::vector<std::vector<Item>>
{
    auto q = std::make_unique<Queue<mode>>();
    const auto total = num_producers * per_producer;
    auto consumed = std::atomic<uint32_t>{};
    auto received = std::vector<std::vector<Item>>(num_consumers);

    auto threads = std::vector<std::thread>{};
    for (auto c = uint32_t{}; c < num_consumers; ++c) {
        threads.emplace_back([&, c] {
            auto out = Item{};
            while (consumed.load(std::memory_order_relaxed) < total) {
                if (q->pop(out)) {
                    received[c].push_back(out);
                    consumed.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto p = uint32_t{}; p < num_producers; ++p) {
        threads.emplace_back([&, p] {
            for (auto i = uint32_t{}; i < per_producer; ++i) {
                while (!q->push(Item{p, i})) std::this_thread::yield();
            }
        });
    }
    for (auto& t : threads) t.join();
    return received;
}

// Every item arrives exactly once, and each consumer sees each producer's items in order.
auto check_delivery(const std::vector<std::vector<Item>>& received, uint32_t num_producers, uint32_t per_producer) -> void
{
    auto seen = std::vector<uint8_t>(num_producers * per_producer);
    for (const auto& list : received) {
        auto last = std::vector<int64_t>(num_producers, -1);
        for (const auto& item : list) {
            expect_true(item.producer < num_producers && item.seq < per_producer, "item out of range");
            expect_true(static_cast<int64_t>(item.seq) > last[item.producer], std::format("producer {} out of order at {}", item.producer, item.seq));
            last[item.producer] = item.seq;
            auto& slot = seen[item.producer * per_producer + item.seq];
            expect_true(slot == 0, std::format("producer {} item {} delivered twice", item.producer, item.seq));
            slot = 1;
        }
    }
    for (auto i = size_t{}; i < seen.size(); ++i) expect_true(seen[i] == 1, std::format("item {} lost", i));
}

auto add_threaded() -> void
{
    Tests::add("spsc: one producer, one consumer, nothing lost or reordered", [] {
        check_delivery(run_threads<Queue_concurrency::spsc>(1, 1, 200'000), 1, 200'000);
    });
    Tests::add("mpsc: four producers, one consumer, exactly once, in order per producer", [] {
        check_delivery(run_threads<Queue_concurrency::mpsc>(4, 1, 50'000), 4, 50'000);
    });
    Tests::add("spmc: one producer, four consumers, exactly once", [] {
        check_delivery(run_threads<Queue_concurrency::spmc>(1, 4, 200'000), 1, 200'000);
    });
    Tests::add("mpmc: four producers, four consumers, exactly once", [] {
        check_delivery(run_threads<Queue_concurrency::mpmc>(4, 4, 50'000), 4, 50'000);
    });
}

// MARK: - thread registry

// Slots are claimed per distinct thread and never reclaimed. Past the limit a thread must be
// refused; it used to write past the end of the registry.
auto add_registry() -> void
{
    Tests::add("mpsc: a thread past the registry's limit is refused, and the rest still work", [] {
        constexpr auto limit = tiny::queue_impl::queue_max_threads;
        auto q = std::make_unique<Lock_free_queue<Item, 128, Queue_concurrency::mpsc>>();

        // All registered threads stay alive, so no thread id can be recycled into a new one.
        auto registered = std::latch{static_cast<std::ptrdiff_t>(limit)};
        auto release = std::latch{1};
        auto accepted = std::atomic<uint32_t>{};
        auto threads = std::vector<std::thread>{};
        for (auto i = uint32_t{}; i < limit; ++i) {
            threads.emplace_back([&, i] {
                if (q->push(Item{i, 0})) accepted.fetch_add(1);
                registered.count_down();
                release.wait();
            });
        }
        registered.wait();

        auto extra_pushed = true;
        std::thread{[&] { extra_pushed = q->push(Item{999, 0}); }}.join();

        release.count_down();
        for (auto& t : threads) t.join();

        expect_true(accepted.load() == limit, std::format("{} of {} registered pushes landed", accepted.load(), limit));
        expect_true(!extra_pushed, "the thread past the limit must be refused");

        auto out = Item{};
        auto popped = uint32_t{};
        while (q->pop(out)) {
            expect_true(out.producer != 999, "the refused item must not be in the queue");
            ++popped;
        }
        expect_true(popped == limit, std::format("popped {}, expected {}", popped, limit));
    });
}

// MARK: - overwrite queue

auto add_overwrite() -> void
{
    Tests::add("Overwrite_queue: push always lands, keeping the newest", [] {
        auto q = std::make_unique<tiny::Overwrite_queue<Item, slots>>();
        for (auto i = uint32_t{}; i < 3 * slots; ++i) q->push(Item{0, i});
        auto out = Item{};
        auto expected = uint32_t{2 * slots};
        while (q->pop(out)) {
            expect_true(out.seq == expected, std::format("expected {}, got {}", expected, out.seq));
            ++expected;
        }
        expect_true(expected == 3 * slots, "the newest slot-count items survive");
    });
}

} // namespace

auto main() -> int
{
    add_single_thread<Queue_concurrency::spsc>("spsc");
    add_single_thread<Queue_concurrency::spmc>("spmc");
    add_single_thread<Queue_concurrency::mpsc>("mpsc");
    add_single_thread<Queue_concurrency::mpmc>("mpmc");
    add_threaded();
    add_registry();
    add_overwrite();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
