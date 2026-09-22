#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <tiny_core/data_port.hpp>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define TINY_TSAN 1
#  endif
#endif

namespace {

auto failures = 0;

auto expect(bool condition, const char* what) -> void
{
    if (!condition)
        ++failures;

    std::printf("    [%s] %s\n", condition ? "pass" : "FAIL", what);
}

// ---------------------------------------------------------------- correctness

// Wide enough that a copy cannot complete in one instruction. Every lane carries
// the same sequence number, so a reader that lands on a slot the writer is still
// filling will see lanes disagree.
struct Packet {
    static constexpr auto LANES = std::size_t{16};

    std::array<std::uint64_t, LANES> lanes{};

    static auto of(std::uint64_t seq) -> Packet
    {
        auto packet = Packet{};
        packet.lanes.fill(seq);
        return packet;
    }

    auto seq() const -> std::uint64_t { return lanes[0]; }

    auto intact() const -> bool
    {
        for (const auto lane : lanes)
            if (lane != lanes[0])
                return false;

        return true;
    }
};

template<tiny::Port_direction D>
auto test_read_returns_the_value_written() -> void
{
    std::printf("  read_returns_the_value_written\n");

    auto port = tiny::Data_port<int, D>{};
    port.write(42);

    expect(port.read() == 42, "read returns the value just written");
}

template<tiny::Port_direction D>
auto test_read_without_a_write_repeats() -> void
{
    std::printf("  read_without_a_write_repeats\n");

    auto port = tiny::Data_port<int, D>{};
    port.write(7);

    expect(port.read() == 7, "first read sees the new value");
    expect(port.read() == 7, "second read repeats it rather than reverting");
    expect(port.read() == 7, "and keeps repeating it");
}

template<tiny::Port_direction D>
auto test_writes_coalesce() -> void
{
    std::printf("  writes_coalesce\n");

    auto port = tiny::Data_port<int, D>{};
    port.write(1);
    port.write(2);
    port.write(3);

    expect(port.read() == 3, "three writes without a read collapse to the newest");
}

template<tiny::Port_direction D>
auto test_alternating_round_trips() -> void
{
    std::printf("  alternating_round_trips\n");

    constexpr auto ROUNDS = 1000;

    auto port = tiny::Data_port<int, D>{};
    auto mismatches = 0;

    // Each round flips the index bit, so this covers both slot parities.
    for (auto round = 0; round < ROUNDS; ++round) {
        port.write(round);

        const auto got = port.read();
        if (got != round)
            ++mismatches;
    }

    expect(mismatches == 0, "1000 alternating write/read round trips all match");
}

template<tiny::Port_direction D>
auto test_spsc_stream() -> void
{
    std::printf("  spsc_stream\n");

    constexpr auto COUNT = std::uint64_t{1'000'000};

    auto port = tiny::Data_port<Packet, D>{};
    auto done = std::atomic<bool>{false};

    auto writer = std::thread{[&] {
        for (auto seq = std::uint64_t{1}; seq <= COUNT; ++seq)
            port.write(Packet::of(seq));

        done.store(true, std::memory_order_release);
    }};

    auto reads = std::uint64_t{0};
    auto torn = std::uint64_t{0};
    auto regressions = std::uint64_t{0};
    auto highest = std::uint64_t{0};
    auto distinct = std::uint64_t{0};   // values the reader actually observed

    while (!done.load(std::memory_order_acquire)) {
        const auto packet = port.read();
        ++reads;

        if (!packet.intact())
            ++torn;
        else if (packet.seq() < highest)
            ++regressions;
        else {
            if (packet.seq() != highest)
                ++distinct;

            highest = packet.seq();
        }
    }

    writer.join();

    // The writer has stopped, so the port must have settled on its last word.
    const auto last = port.read();

    expect(torn == 0, "reader never landed on a slot the writer was filling");
    expect(regressions == 0, "observed sequence numbers never went backwards");
    expect(reads > 0, "reader made progress while the writer was running");
    expect(last.intact(), "final packet is self-consistent");
    expect(last.seq() == COUNT, "final read settles on the last value written");

    std::printf("    ....  %llu reads observed %llu of %llu written values (%.1f%% dropped)\n",
                static_cast<unsigned long long>(reads),
                static_cast<unsigned long long>(distinct),
                static_cast<unsigned long long>(COUNT),
                100.0 * (1.0 - double(distinct) / double(COUNT)));
}

// ------------------------------------------------------------- handoff probe

volatile std::uint64_t sink = 0;   // keeps copy-outs from being elided

// Checks that the reader's copy-out of a slot is ordered before the index flip
// that hands that slot to the writer. The assertions above cannot see that bug:
// it is a missing release, not a wrong value, so every observable value stays
// correct. ThreadSanitizer is the only oracle, and a clean exit is the pass.
//
// The payload must stay a single scalar. TSan does precise race detection only
// on instrumented scalar loads and stores; as soon as a copy lowers to a memcpy
// or a vector block move it stops checking, and this probe silently passes no
// matter how broken the handoff is. Measured on arm64: a u64 is caught, the same
// 8 bytes wrapped in a std::array is caught, 16 bytes as a std::array is not.
template<tiny::Port_direction D>
auto probe_ownership_handoff() -> void
{
    std::printf("  ownership_handoff_probe\n");

    constexpr auto COUNT = 200'000;

    auto port = tiny::Data_port<std::uint64_t, D>{};
    auto done = std::atomic<bool>{false};

    auto writer = std::thread{[&] {
        for (auto i = 0; i < COUNT; ++i)
            port.write(static_cast<std::uint64_t>(i));

        done.store(true, std::memory_order_release);
    }};

    while (!done.load(std::memory_order_acquire))
        sink = port.read();

    writer.join();

#ifdef TINY_TSAN
    std::printf("    [pass] no data race on the slot handoff (ThreadSanitizer clean)\n");
#else
    std::printf("    ....  ran, but has no oracle here -- rebuild with -fsanitize=thread\n");
#endif
}

// read_fresh is Main-only: the reader owns the dirty bit there.
auto test_read_fresh() -> void
{
    std::printf("  read_fresh\n");

    auto port = tiny::Data_port<Packet, tiny::Port_direction::Main>{};
    auto out = Packet{};

    expect(!port.read_fresh(out), "nothing fresh before the first write");

    port.write(Packet::of(7));
    expect(port.read_fresh(out) && out.seq() == 7, "a write is delivered once");
    expect(!port.read_fresh(out) && out.seq() == 7, "a second read reports nothing and leaves out alone");
    expect(port.read().seq() == 7, "read still answers after read_fresh consumed the write");

    port.write(Packet::of(8));
    port.write(Packet::of(9));
    expect(port.read_fresh(out) && out.seq() == 9, "writes coalesce to the newest");
}

auto test_read_fresh_stream() -> void
{
    std::printf("  read_fresh_stream\n");

    constexpr auto COUNT = std::uint64_t{1'000'000};

    auto port = tiny::Data_port<Packet, tiny::Port_direction::Main>{};
    auto done = std::atomic<bool>{false};

    auto writer = std::thread{[&] {
        for (auto seq = std::uint64_t{1}; seq <= COUNT; ++seq)
            port.write(Packet::of(seq));

        done.store(true, std::memory_order_release);
    }};

    auto torn = std::uint64_t{0};
    auto stale = std::uint64_t{0};
    auto highest = std::uint64_t{0};
    auto out = Packet{};

    while (!done.load(std::memory_order_acquire)) {
        if (!port.read_fresh(out))
            continue;

        if (!out.intact())
            ++torn;
        else if (out.seq() <= highest)
            ++stale;
        else
            highest = out.seq();
    }

    writer.join();
    if (port.read_fresh(out) && out.intact())
        highest = std::max(highest, out.seq());

    expect(torn == 0, "a fresh read never lands on a slot the writer was filling");
    expect(stale == 0, "every fresh read is strictly newer than the last");
    expect(highest == COUNT, "the last write is always delivered");
}

// Scalar payload for the same reason as probe_ownership_handoff: TSan only sees scalar copies.
auto probe_read_fresh_handoff() -> void
{
    std::printf("  read_fresh_handoff_probe\n");

    constexpr auto COUNT = 200'000;

    auto port = tiny::Data_port<std::uint64_t, tiny::Port_direction::Main>{};
    auto done = std::atomic<bool>{false};

    auto writer = std::thread{[&] {
        for (auto i = 0; i < COUNT; ++i)
            port.write(static_cast<std::uint64_t>(i));

        done.store(true, std::memory_order_release);
    }};

    auto out = std::uint64_t{};
    while (!done.load(std::memory_order_acquire))
        if (port.read_fresh(out))
            sink = out;

    writer.join();

#ifdef TINY_TSAN
    std::printf("    [pass] no data race on the read_fresh handoff (ThreadSanitizer clean)\n");
#else
    std::printf("    ....  ran, but has no oracle here -- rebuild with -fsanitize=thread\n");
#endif
}

template<tiny::Port_direction D>
auto run_suite(const char* variant) -> void
{
    std::printf("%s\n", variant);

    test_read_returns_the_value_written<D>();
    test_read_without_a_write_repeats<D>();
    test_writes_coalesce<D>();
    test_alternating_round_trips<D>();
    test_spsc_stream<D>();
    probe_ownership_handoff<D>();

    std::printf("\n");
}

// ----------------------------------------------------------------- benchmark

// Small and realistic: the smaller the payload, the more the control word and
// the slots compete for the same line, which is exactly what padding fixes.
struct Params {
    double gain{};
    double cutoff{};
    double resonance{};
    double mix{};
};

template<typename Port>
auto bench_ns_per_write(std::size_t count) -> double
{
    auto port = Port{};
    auto stop = std::atomic<bool>{false};
    auto ready = std::atomic<bool>{false};

    auto reader = std::thread{[&] {
        ready.store(true, std::memory_order_release);

        while (!stop.load(std::memory_order_acquire))
            sink = static_cast<std::uint64_t>(port.read().gain);
    }};

    while (!ready.load(std::memory_order_acquire))
        ;

    auto params = Params{};

    const auto start = std::chrono::steady_clock::now();
    for (auto i = std::size_t{0}; i < count; ++i) {
        params.gain = static_cast<double>(i);
        port.write(params);
    }
    const auto finish = std::chrono::steady_clock::now();

    stop.store(true, std::memory_order_release);
    reader.join();

    return std::chrono::duration<double, std::nano>(finish - start).count()
         / static_cast<double>(count);
}

template<typename Port>
auto bench(const char* label) -> void
{
    constexpr auto TRIALS = std::size_t{9};
    constexpr auto COUNT = std::size_t{2'000'000};

    (void) bench_ns_per_write<Port>(COUNT / 10);   // warm up

    auto trials = std::array<double, TRIALS>{};
    for (auto& trial : trials)
        trial = bench_ns_per_write<Port>(COUNT);

    std::sort(trials.begin(), trials.end());

    const auto best = trials.front();
    const auto median = trials[TRIALS / 2];

    std::printf("  %-18s sizeof %4zu  align %3zu   %6.2f ns/write (median %6.2f)   %7.1f M/s\n",
                label, sizeof(Port), alignof(Port), best, median, 1000.0 / best);
}

auto run_benchmarks() -> void
{
    std::printf("benchmark  (writer throughput under a spinning reader, best of 9 x 2M writes)\n");

#ifdef TINY_TSAN
    std::printf("  ....  skipped: timings are meaningless under a sanitizer\n");
#else
    // Padding is no longer a knob, so this tracks throughput rather than
    // comparing layouts. Measured when it was a knob: padding to the 128-byte
    // hardware line was worth 1.5-1.9x over a packed layout, and 64 bytes was
    // worth nothing at all.
    bench<tiny::Data_port<Params, tiny::Port_direction::Main>>("wait-free writer");
    bench<tiny::Data_port<Params, tiny::Port_direction::Audio>>("wait-free reader");
#endif
}

} // namespace

auto main() -> int
{
    run_suite<tiny::Port_direction::Main>("Data_port<T, Port_direction::Main>   -- writer never blocks");
    test_read_fresh();
    test_read_fresh_stream();
    probe_read_fresh_handoff();
    std::printf("\n");
    run_suite<tiny::Port_direction::Audio>("Data_port<T, Port_direction::Audio>  -- reader never blocks");

    std::printf("%s\n\n", failures == 0 ? "all tests passed" : "TESTS FAILED");

    run_benchmarks();

    return failures == 0 ? 0 : 1;
}
