#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>

#include <tiny_core/tiny_meters.hpp>

namespace {

using tiny::meters::Policy;
using tiny::meters::Spec;
using tiny::meters::Transport;

struct Meters {
    static constexpr auto num_meters = std::uint32_t{3};

    enum : std::uint32_t { level = 0, peak = 1, trig = 2 };

    static auto make_spec(std::uint32_t addr) -> Spec
    {
        switch (addr) {
            case level: return {{0., 1.}, Policy::Stream};
            case peak:  return {{0., 1.}, Policy::Peak};
            default:    return {{0., 1.}, Policy::Trig};
        }
    }
};

template<Transport T>
using Box = tiny::meters::Mailbox<Meters, T>;

using Framework_box = Box<Transport::Framework>;
using Host_box = Box<Transport::Host>;
using Out = std::array<float, Meters::num_meters>;

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

auto expect_eq(float got, float want, const char* what) -> void
{
    const auto ok = std::fabs(got - want) < 1e-6f;
    if (!ok) ++failures;
    std::printf("  [%s] %s (got %g, want %g)\n", ok ? "pass" : "FAIL", what, got, want);
}

// MARK: - shared semantics

template<typename Box>
auto test_stream(const char* tag) -> void
{
    std::printf("stream, %s\n", tag);

    auto box = Box{};
    auto out = Out{};

    box.post(Meters::level, 0.25f);
    box.read(out);
    expect_eq(out[Meters::level], 0.25f, "delivered level is read back");

    box.read(out);
    expect_eq(out[Meters::level], 0.25f, "a level is retained: reading does not clear it");

    box.post(Meters::level, 0.5f);
    box.post(Meters::level, 0.75f);
    box.read(out);
    expect_eq(out[Meters::level], 0.75f, "last value wins within one read interval");
}

template<typename Box>
auto test_peak_max(const char* tag) -> void
{
    std::printf("peak max, %s\n", tag);

    auto box = Box{};
    auto out = Out{};

    box.post(Meters::peak, 0.2f);
    box.post(Meters::peak, 0.8f);
    box.post(Meters::peak, 0.4f);
    box.read(out);
    expect_eq(out[Meters::peak], 0.8f, "max over the read interval, not the last value");

    box.post(Meters::peak, 0.3f);
    box.read(out);
    expect_eq(out[Meters::peak], 0.3f, "the interval's max is taken, not carried forward");
}

template<typename Box>
auto test_peak_silence(const char* tag) -> void
{
    std::printf("peak silence, %s\n", tag);

    auto box = Box{};
    auto out = Out{};

    box.post(Meters::peak, 0.6f);
    box.read(out);
    expect_eq(out[Meters::peak], 0.6f, "measurement delivered");

    box.read(out);
    expect_eq(out[Meters::peak], 0.6f, "nothing arrived is not a measurement of zero");

    box.post(Meters::peak, 0.f);
    box.read(out);
    expect_eq(out[Meters::peak], 0.f, "a delivered zero is a measurement, and lands");
}

template<typename Box>
auto test_trig(const char* tag) -> void
{
    std::printf("trig, %s\n", tag);

    auto box = Box{};
    auto out = Out{};

    box.read(out);
    expect_eq(out[Meters::trig], 0.f, "quiet until something fires");

    box.post(Meters::trig, 0.9f);
    box.read(out);
    expect_eq(out[Meters::trig], 0.9f, "magnitude shows on the frame it fired");

    box.read(out);
    expect_eq(out[Meters::trig], 0.f, "and only on that frame");

    box.post(Meters::trig, 0.f);
    box.read(out);
    expect_eq(out[Meters::trig], 0.f, "a zero is not an event");

    box.post(Meters::trig, 0.3f);
    box.post(Meters::trig, 0.8f);
    box.read(out);
    expect_eq(out[Meters::trig], 0.8f, "two in one interval: the last magnitude is what shows");
    box.read(out);
    expect_eq(out[Meters::trig], 0.f, "and the interval is over");
}

// MARK: - the divergence

// Both transports see the producer restate a peak every block. Only the framework's own
// transport actually delivers those restatements; a host wire dedupes them away, so the
// two have to disagree about what a read with no arrivals means.
auto test_dropped_restatement() -> void
{
    std::printf("dropped restatement\n");

    auto framework = Framework_box{};
    auto host = Host_box{};
    auto out = Out{};

    // One interval carrying a transient and then the fall after it.
    framework.post(Meters::peak, 0.9f);
    framework.post(Meters::peak, 0.1f);
    host.post(Meters::peak, 0.9f);
    host.post(Meters::peak, 0.1f);

    framework.read(out);
    expect_eq(out[Meters::peak], 0.9f, "framework: transient survives the interval");
    host.read(out);
    expect_eq(out[Meters::peak], 0.9f, "host: transient survives the interval");

    // Now the producer keeps restating 0.1 and the wire delivers none of it.
    framework.read(out);
    expect_eq(out[Meters::peak], 0.9f, "framework: holds the max, because its wire does not drop");
    host.read(out);
    expect_eq(out[Meters::peak], 0.1f, "host: falls to the last value delivered");

    host.read(out);
    expect_eq(out[Meters::peak], 0.1f, "host: and stays there until something else arrives");
}

// MARK: - publisher round trip

template<typename Box>
auto test_publisher(const char* tag) -> void
{
    std::printf("publisher round trip, %s\n", tag);

    auto publisher = tiny::meters::Publisher<Meters>{};
    auto box = Box{};
    auto out = Out{};

    auto sends = 0;
    auto send = [&](std::uint32_t addr, float value) {
        ++sends;
        box.post(addr, value);
        return true;
    };

    auto scratch = publisher.scratch();

    scratch[Meters::level] = 0.5f;
    scratch[Meters::peak] = 0.7f;
    scratch[Meters::trig] = 1.f;
    publisher.publish(false, send);

    box.read(out);
    expect_eq(out[Meters::level], 0.5f, "level reaches the mailbox");
    expect_eq(out[Meters::peak], 0.7f, "peak reaches the mailbox");
    expect_eq(out[Meters::trig], 1.f, "trigger reaches the mailbox");

    expect_eq(scratch[Meters::peak], 0.f, "publisher reset the peak scratch");
    expect_eq(scratch[Meters::trig], 0.f, "publisher reset the trig scratch");
    expect_eq(scratch[Meters::level], 0.5f, "publisher left the level scratch standing");

    // A silent block: the peak is restated as zero, the level is deduplicated to nothing.
    sends = 0;
    publisher.publish(false, send);
    expect(sends == 1, "one send for the silent block: the peak restatement");
    box.read(out);
    expect_eq(out[Meters::peak], 0.f, "silence lands as a measurement of zero");

    // A suspended block transmits nothing but still clears the scratch.
    sends = 0;
    scratch[Meters::peak] = 0.9f;
    scratch[Meters::trig] = 0.9f;
    publisher.publish(true, send);
    expect(sends == 0, "suspended: nothing transmitted");
    expect_eq(scratch[Meters::peak], 0.f, "suspended: peak scratch still cleared");
    expect_eq(scratch[Meters::trig], 0.f, "suspended: trig scratch still cleared");
}

// MARK: - concurrency

// Value and count share one atomic so a reader can't take a peak and then miss its count.
auto test_concurrent_peak() -> void
{
    std::printf("concurrent peak, framework\n");

    constexpr auto trials = 50;
    constexpr auto posts = 20000;
    auto lost = 0;

    for (auto t = 0; t < trials; ++t) {
        auto box = Framework_box{};
        auto done = std::atomic<bool>{false};
        auto highest = 0.f;

        auto producer = std::thread{[&] {
            for (auto i = 0; i < posts; ++i) {
                box.post(Meters::peak, (i == posts / 2) ? 0.9f : 0.1f);
            }
            done.store(true, std::memory_order_release);
        }};

        auto out = Out{};
        while (!done.load(std::memory_order_acquire)) {
            box.read(out);
            highest = std::max(highest, out[Meters::peak]);
        }
        producer.join();
        box.read(out);
        highest = std::max(highest, out[Meters::peak]);

        if (highest < 0.9f) ++lost;
    }
    expect(lost == 0, "a posted peak is never dropped by a concurrent reader (50 trials)");
}

} // namespace

auto main() -> int
{
    test_stream<Framework_box>("framework");
    std::printf("\n"); test_stream<Host_box>("host");
    std::printf("\n"); test_peak_max<Framework_box>("framework");
    std::printf("\n"); test_peak_max<Host_box>("host");
    std::printf("\n"); test_peak_silence<Framework_box>("framework");
    std::printf("\n"); test_peak_silence<Host_box>("host");
    std::printf("\n"); test_trig<Framework_box>("framework");
    std::printf("\n"); test_trig<Host_box>("host");
    std::printf("\n"); test_dropped_restatement();
    std::printf("\n"); test_publisher<Framework_box>("framework");
    std::printf("\n"); test_publisher<Host_box>("host");
    std::printf("\n"); test_concurrent_peak();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
