#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <tiny_core/change_set.hpp>

namespace {

std::atomic<long long> g_allocs{0};
std::atomic<long long> g_bytes{0};

} // namespace

auto operator new(std::size_t n) -> void*
{
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    g_bytes.fetch_add(static_cast<long long>(n), std::memory_order_relaxed);
    if (auto* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
auto operator delete(void* p) noexcept -> void { std::free(p); }
auto operator delete(void* p, std::size_t) noexcept -> void { std::free(p); }

namespace {

struct Event {
    std::uint32_t address{};
    double value{};
};

constexpr auto N = std::uint32_t{256};

using Set = tiny::Change_set<Event, N>;
using Single = tiny::Change_set<Event, N, tiny::Producers::One>;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

struct Alloc_scope {
    long long allocs{g_allocs.load()};
    long long bytes{g_bytes.load()};

    auto since() const -> long long { return g_allocs.load() - allocs; }
    auto bytes_since() const -> long long { return g_bytes.load() - bytes; }
};

// MARK: - semantics

template<typename S>
auto test_semantics(const char* name) -> void
{
    std::printf("semantics (%s)\n", name);

    auto set = std::make_unique<S>();
    auto seen = std::vector<std::pair<std::uint32_t, double>>{};
    auto drain = [&] { seen.clear(); return set->consume([&](auto a, auto v) { seen.emplace_back(a, v); }); };

    set->push(Event{3, 1.5});
    expect(drain() && seen.size() == 1 && seen[0].first == 3 && seen[0].second == 1.5, "single change round trips");
    expect(!drain() && seen.empty(), "consumed changes do not repeat");

    set->push(Event{7, 1.0});
    set->push(Event{7, 2.0});
    set->push(Event{7, 3.0});
    drain();
    expect(seen.size() == 1 && seen[0].second == 3.0, "repeats coalesce to newest");

    const auto batch = std::vector<Event>{{1, 10.}, {2, 20.}, {200, 30.}};
    set->push_n(batch);
    set->push(Event{2, 21.});
    drain();
    expect(seen.size() == 3 && seen[1].second == 21., "a batch and a later push merge");

    set->push(Event{N - 1, 9.0});
    drain();
    expect(seen.size() == 1 && seen[0].first == N - 1 && seen[0].second == 9.0, "highest address works, iteration is sparse");

    set->push_n({});
    expect(!drain(), "an empty batch publishes nothing");

    // Alternate fresh and merged pushes across many takes, so both buffers take every role.
    auto ok = true;
    for (auto round = 1; round <= 50; ++round) {
        set->push(Event{static_cast<std::uint32_t>(round), double(round)});
        if (round % 3 == 0) set->push(Event{0, double(round)});
        drain();
        auto expected = (round % 3 == 0) ? std::size_t{2} : std::size_t{1};
        ok = ok && seen.size() == expected;
    }
    expect(ok, "buffers swap roles cleanly over many rounds");
}

// MARK: - fixed memory

auto test_no_allocation() -> void
{
    std::printf("fixed memory\n");

    auto set = std::make_unique<Set>();
    std::printf("  ....  sizeof = %zu bytes (double buffered)\n", sizeof(Set));

    auto batch = std::vector<Event>{};
    for (auto i = std::uint32_t{}; i < N; ++i) batch.push_back(Event{i, double(i)});

    const auto scope = Alloc_scope{};
    for (auto round = 0; round < 1000; ++round) {
        set->push_n(batch);
        set->push(Event{static_cast<std::uint32_t>(round) % N, double(round)});
        set->consume([](auto, auto) {});
    }
    expect(scope.since() == 0, "1000 rounds of full-width traffic allocate nothing");
    if (scope.since() != 0)
        std::printf("  ....  %lld allocations, %lld bytes\n", scope.since(), scope.bytes_since());
}

auto test_reference_allocates() -> void
{
    std::printf("reference: triple-buffered unordered_map\n");

    auto maps = std::array<std::unordered_map<std::uint32_t, double>, 3>{};
    for (auto& m : maps) m.reserve(2 * 128);

    const auto scope = Alloc_scope{};
    for (auto round = 0; round < 1000; ++round) {
        auto& m = maps[static_cast<std::size_t>(round % 3)];
        m.clear();
        for (auto i = std::uint32_t{}; i < N; ++i) m.insert_or_assign(i, double(i));
    }
    std::printf("  ....  %lld allocations, %lld bytes for the same traffic\n",
                scope.since(), scope.bytes_since());
}

// MARK: - concurrency

// Every address in one batch carries the same round, so a partial batch, or a producer
// writing the buffer the consumer is reading, shows up as two values inside one consume.
template<typename S>
auto test_batch_atomicity(const char* name) -> void
{
    std::printf("batch atomicity (%s)\n", name);

    constexpr auto WIDTH = std::uint32_t{64};
    constexpr auto ROUNDS = 20000;

    auto set = std::make_unique<S>();
    auto stop = std::atomic<bool>{false};
    auto torn = 0ll;
    auto observed = 0ll;

    auto producer = std::thread{[&] {
        auto batch = std::vector<Event>(WIDTH);
        for (auto round = 1; round <= ROUNDS; ++round) {
            for (auto i = std::uint32_t{}; i < WIDTH; ++i) batch[i] = Event{i, double(round)};
            set->push_n(batch);
        }
        stop.store(true, std::memory_order_release);
    }};

    auto consume = [&] {
        auto first = -1.0;
        auto mixed = false;
        auto count = 0;
        set->consume([&](auto, auto v) {
            if (count++ == 0) first = v;
            else if (v != first) mixed = true;
        });
        if (count > 0) {
            ++observed;
            if (mixed) ++torn;
            if (count != static_cast<int>(WIDTH)) ++torn;
        }
    };
    while (!stop.load(std::memory_order_acquire)) consume();
    producer.join();
    consume();

    std::printf("  ....  %lld non-empty consumes, %lld saw a torn batch\n", observed, torn);
    expect(torn == 0, "batches are never observed half-applied");
    expect(observed > 1, "and the consumer kept taking them");
}

// Disjoint addresses per producer, so the last value written to each is known: whatever the
// interleaving of takes and merges, the consumer must end on it.
auto test_multi_producer() -> void
{
    std::printf("multi producer\n");

    constexpr auto PRODUCERS = 4;
    constexpr auto ROUNDS = 50000;

    auto set = std::make_unique<Set>();
    auto running = std::atomic<int>{PRODUCERS};
    auto last = std::array<double, N>{};
    auto bad = 0ll;
    auto drained = 0ll;

    auto threads = std::vector<std::thread>{};
    for (auto p = 0; p < PRODUCERS; ++p) {
        threads.emplace_back([&, p] {
            for (auto round = 1; round <= ROUNDS; ++round) {
                const auto addr = static_cast<std::uint32_t>(p) + static_cast<std::uint32_t>(PRODUCERS) * (static_cast<std::uint32_t>(round) % (N / PRODUCERS));
                set->push(Event{addr, double(round)});
            }
            running.fetch_sub(1, std::memory_order_release);
        });
    }

    auto consume = [&] {
        set->consume([&](auto a, auto v) {
            ++drained;
            if (v < 1.0 || v > double(ROUNDS)) ++bad;
            last[a] = v;
        });
    };
    while (running.load(std::memory_order_acquire) != 0) consume();
    for (auto& t : threads) t.join();
    consume();

    // The final value per address is the last round that wrote it.
    auto lost = 0;
    for (auto a = std::uint32_t{}; a < N; ++a) {
        const auto slot = static_cast<int>(a / PRODUCERS);
        auto want = 0;
        for (auto round = 1; round <= ROUNDS; ++round) {
            if (round % static_cast<int>(N / PRODUCERS) == slot) want = round;
        }
        if (last[a] != double(want)) ++lost;
    }

    std::printf("  ....  %lld changes drained from %d producers\n", drained, PRODUCERS);
    expect(bad == 0, "no torn or out-of-range values under 4 producers");
    expect(lost == 0, "every address ends on the last value written to it");
}

// The consumer never waits: while a merge holds the pending batch, consume returns at once
// and delivers it on a later call.
auto test_consumer_never_waits() -> void
{
    std::printf("consumer never waits\n");

    auto set = std::make_unique<Set>();
    auto stop = std::atomic<bool>{false};
    auto skipped = 0ll;
    auto delivered = 0ll;

    auto producer = std::thread{[&] {
        auto batch = std::vector<Event>(N);
        for (auto round = 1; round <= 20000; ++round) {
            for (auto i = std::uint32_t{}; i < N; ++i) batch[i] = Event{i, double(round)};
            set->push_n(batch);
        }
        stop.store(true, std::memory_order_release);
    }};

    auto final_value = 0.0;
    while (!stop.load(std::memory_order_acquire)) {
        if (set->consume([&](auto, auto v) { final_value = v; })) ++delivered;
        else ++skipped;
    }
    producer.join();
    set->consume([&](auto, auto v) { final_value = v; });

    std::printf("  ....  %lld deliveries, %lld empty or deferred calls\n", delivered, skipped);
    expect(final_value == 20000.0, "the last batch is delivered once the producer stops");
}

} // namespace

auto main() -> int
{
    test_semantics<Set>("many producers");
    std::printf("\n"); test_semantics<Single>("one producer");
    std::printf("\n"); test_no_allocation();
    std::printf("\n"); test_reference_allocates();
    std::printf("\n"); test_batch_atomicity<Set>("many producers");
    std::printf("\n"); test_batch_atomicity<Single>("one producer");
    std::printf("\n"); test_multi_producer();
    std::printf("\n"); test_consumer_never_waits();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
