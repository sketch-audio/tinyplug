#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <tiny_core/tiny_blocks.hpp>

#if defined(__has_feature)
#  if __has_feature(thread_sanitizer)
#    define TINY_TSAN 1
#  endif
#endif

namespace {

namespace blocks = tiny::blocks;

struct Spectrum {
    std::array<float, 256> bins{};
    std::uint32_t used{};
};

struct Scope {
    std::array<float, 64> samples{};
    std::uint32_t trigger{};
};

// Every lane carries the same sequence number, so a torn copy shows as lanes disagreeing.
struct Packet {
    std::array<std::uint64_t, 16> lanes{};

    static auto of(std::uint64_t seq) -> Packet
    {
        auto p = Packet{};
        p.lanes.fill(seq);
        return p;
    }

    auto seq() const -> std::uint64_t { return lanes[0]; }

    auto intact() const -> bool
    {
        for (const auto lane : lanes)
            if (lane != lanes[0]) return false;
        return true;
    }
};

struct Model {
    using Types = std::variant<Spectrum, Scope, Packet, std::uint64_t>;

    enum class Address : std::uint32_t { Spectrum, Scope, Packet, Scalar };
    static constexpr auto num_blocks = std::uint32_t{4};

    static constexpr auto make_spec(std::uint32_t address) -> blocks::Spec
    {
        switch (static_cast<Address>(address)) {
            case Address::Spectrum: return {blocks::kind_of<::Spectrum, Types>};
            case Address::Scope:    return {blocks::kind_of<::Scope, Types>};
            case Address::Packet:   return {blocks::kind_of<::Packet, Types>};
            case Address::Scalar:   return {blocks::kind_of<std::uint64_t, Types>};
            default:                return {};
        }
    }
};
static_assert(blocks::Model<Model>);
static_assert(blocks::Model<blocks::None>);

using A = Model::Address;
static_assert(std::is_same_v<blocks::Frame_at<Model, 0>, Spectrum>);
static_assert(std::is_same_v<blocks::Frames<Model>::Frame<A::Scope>, Scope>);
static_assert(std::is_same_v<blocks::Frames<Model>::Frame<3>, std::uint64_t>, "a plain integer address works too");

auto failures = 0;

auto expect(bool ok, const char* what) -> void
{
    if (!ok) ++failures;
    std::printf("  [%s] %s\n", ok ? "pass" : "FAIL", what);
}

// A publisher wired straight to a mailbox, as the in-process formats do.
struct Rig {
    blocks::Publisher<Model> publisher{};
    blocks::Mailbox<Model> mailbox{};
    blocks::Frames<Model> frames{};
    blocks::Writer<Model> writer{&publisher};
    int sends{};

    auto block(bool suspend = false) -> void
    {
        publisher.transmit(suspend, [&](auto i, const auto& frame) {
            ++sends;
            return mailbox.post(i, frame);
        });
    }

    auto draw() -> blocks::View<Model>
    {
        mailbox.read(frames);
        return blocks::View<Model>{&frames};
    }
};

// MARK: - semantics

auto test_round_trip() -> void
{
    std::printf("round trip\n");
    auto rig = Rig{};

    auto view = rig.draw();
    expect(view.fresh<A::Spectrum>() == nullptr, "nothing fresh before any publish");
    expect(view.latest<A::Spectrum>().used == 0, "latest answers with a default frame before any publish");

    rig.writer.write<A::Spectrum>().used = 42;
    rig.writer.publish<A::Spectrum>();
    rig.block();
    view = rig.draw();
    expect(view.fresh<A::Spectrum>() != nullptr && view.fresh<A::Spectrum>()->used == 42, "a publish arrives fresh");
    expect(view.fresh<A::Scope>() == nullptr, "an unpublished address stays stale");

    view = rig.draw();
    expect(view.fresh<A::Spectrum>() == nullptr, "fresh clears on the next draw");
    expect(view.latest<A::Spectrum>().used == 42, "latest retains the frame");
}

auto test_write_without_publish() -> void
{
    std::printf("write without publish\n");
    auto rig = Rig{};

    rig.writer.write<A::Scope>().trigger = 7;
    rig.block();
    auto view = rig.draw();
    expect(rig.sends == 0, "an abandoned write sends nothing");
    expect(view.latest<A::Scope>().trigger == 0, "the editor never sees an unpublished frame");

    rig.writer.publish<A::Scope>();
    rig.block();
    view = rig.draw();
    expect(view.latest<A::Scope>().trigger == 7, "staging persists until published");
}

auto test_coalescing() -> void
{
    std::printf("coalescing\n");
    auto rig = Rig{};

    rig.writer.write<A::Scope>().trigger = 1;
    rig.writer.publish<A::Scope>();
    rig.writer.write<A::Scope>().trigger = 2;
    rig.writer.publish<A::Scope>();
    rig.block();
    expect(rig.sends == 1, "two publishes in one block send once");

    for (auto n : {1, 5, 20, 64}) {
        for (auto k = 1; k <= n; ++k) {
            rig.writer.write<A::Scope>().trigger = static_cast<std::uint32_t>(100 * n + k);
            rig.writer.publish<A::Scope>();
            rig.block();
        }
        const auto view = rig.draw();
        const auto* fresh = view.fresh<A::Scope>();
        const auto want = static_cast<std::uint32_t>(100 * n + n);
        std::printf("    reader %d publishes behind:", n);
        expect(fresh != nullptr && fresh->trigger == want, " sees exactly the newest");
    }
}

auto test_suspend_and_refusal() -> void
{
    std::printf("suspend and refusal\n");
    auto rig = Rig{};

    rig.writer.write<A::Spectrum>().used = 3;
    rig.writer.publish<A::Spectrum>();
    rig.block(true);
    expect(rig.sends == 0, "suspended: nothing sent");
    rig.block();
    expect(rig.sends == 0, "suspended publish is dropped, not deferred");

    auto refuse = true;
    rig.writer.publish<A::Spectrum>();
    rig.publisher.transmit(false, [&](auto, const auto&) { return !refuse; });
    refuse = false;
    auto sent = 0;
    rig.publisher.transmit(false, [&](auto, const auto&) { ++sent; return true; });
    expect(sent == 1, "a refused frame retries on the next block");
}

auto test_none() -> void
{
    std::printf("no blocks\n");
    auto publisher = blocks::Publisher<blocks::None>{};
    auto mailbox = blocks::Mailbox<blocks::None>{};
    auto frames = blocks::Frames<blocks::None>{};
    auto sends = 0;
    publisher.transmit(false, [&](auto, const auto&) { ++sends; return true; });
    mailbox.read(frames);
    expect(sends == 0, "the empty model compiles and does nothing");
}

// MARK: - concurrency

auto test_concurrent_stream() -> void
{
    std::printf("concurrent stream\n");

    constexpr auto count = std::uint64_t{200'000};
    auto publisher = blocks::Publisher<Model>{};
    auto mailbox = blocks::Mailbox<Model>{};
    auto frames = blocks::Frames<Model>{};
    auto done = std::atomic<bool>{false};

    auto audio = std::thread{[&] {
        for (auto seq = std::uint64_t{1}; seq <= count; ++seq) {
            publisher.write<A::Packet>() = Packet::of(seq);
            publisher.publish<A::Packet>();
            publisher.transmit(false, [&](auto i, const auto& frame) { return mailbox.post(i, frame); });
        }
        done.store(true, std::memory_order_release);
    }};

    auto torn = 0;
    auto stale = 0;
    auto highest = std::uint64_t{};
    auto check = [&] {
        mailbox.read(frames);
        const auto view = blocks::View<Model>{&frames};
        if (const auto* p = view.fresh<A::Packet>()) {
            if (!p->intact()) ++torn;
            else if (p->seq() <= highest) ++stale;
            else highest = p->seq();
        }
    };
    while (!done.load(std::memory_order_acquire)) check();
    audio.join();
    check();

    expect(torn == 0, "the editor never sees a torn frame");
    expect(stale == 0, "every fresh frame is newer than the last");
    expect(highest == count, "the last publish always arrives");
}

// Scalar frame: TSan only checks scalar copies, not memcpy-lowered ones.
auto probe_handoff() -> void
{
    std::printf("handoff probe\n");

    auto publisher = blocks::Publisher<Model>{};
    auto mailbox = blocks::Mailbox<Model>{};
    auto frames = blocks::Frames<Model>{};
    auto done = std::atomic<bool>{false};

    auto audio = std::thread{[&] {
        for (auto seq = std::uint64_t{1}; seq <= 100'000; ++seq) {
            publisher.write<A::Scalar>() = seq;
            publisher.publish<A::Scalar>();
            publisher.transmit(false, [&](auto i, const auto& frame) { return mailbox.post(i, frame); });
        }
        done.store(true, std::memory_order_release);
    }};

    volatile auto sink = std::uint64_t{};
    while (!done.load(std::memory_order_acquire)) {
        mailbox.read(frames);
        sink = frames.latest<A::Scalar>();
    }
    audio.join();
    (void)sink;

#ifdef TINY_TSAN
    std::printf("  [pass] no data race on the publisher to editor handoff (ThreadSanitizer clean)\n");
#else
    std::printf("  ....  ran, but has no oracle here -- rebuild with -fsanitize=thread\n");
#endif
}

} // namespace

auto main() -> int
{
    test_round_trip();
    std::printf("\n"); test_write_without_publish();
    std::printf("\n"); test_coalescing();
    std::printf("\n"); test_suspend_and_refusal();
    std::printf("\n"); test_none();
    std::printf("\n"); test_concurrent_stream();
    std::printf("\n"); probe_handoff();

    std::printf("\n%s\n", failures == 0 ? "all tests passed" : "TESTS FAILED");
    return failures == 0 ? 0 : 1;
}
