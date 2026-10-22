// process::Stream_sync: which blocks restart the processor, and which an offline render holds back.
// Facts in, plans out, no host.

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tinyplug/stream_sync.hpp>

namespace {

using audio_bench::Tests;
using audio_bench::expect_true;
using namespace tiny::process;
using Restart = Stream_sync::Restart;

constexpr auto sr = 48000.;
constexpr auto offline = Render_mode::Offline;
constexpr auto realtime = Render_mode::Realtime;

auto no_content = [] { return false; };

auto block(Render_mode mode, std::optional<std::int64_t> position, std::uint32_t frames = 384) -> Stream_sync::Facts
{
    return {.render_mode = mode, .position = position, .frames = frames};
}

auto add_tests() -> void
{
    Tests::add("a fresh configure restarts at the first block, then nothing until a discontinuity", [] {
        auto sync = Stream_sync{};
        sync.configured(sr);
        expect_true(sync.next(block(realtime, 0), no_content).restart == Restart::Stream, "first block");
        for (auto n = 0; n < 4; ++n) expect_true(sync.next(block(realtime, 384 * (n + 1)), no_content).restart == Restart::None, "steady");
    });

    Tests::add("a host reset and the render-mode edge each restart once", [] {
        auto sync = Stream_sync{};
        sync.configured(sr);
        (void)sync.next(block(realtime, 0), no_content);
        auto reset = block(realtime, 384);
        reset.host_reset = true;
        expect_true(sync.next(reset, no_content).restart == Restart::Stream, "host reset");
        expect_true(sync.next(block(offline, 0), no_content).restart == Restart::Stream, "into offline");
        expect_true(sync.next(block(offline, 384), no_content).restart == Restart::None, "steady offline");
        expect_true(sync.next(block(realtime, 768), no_content).restart == Restart::Stream, "back to realtime");
    });

    Tests::add("resuming after the skip restarts the processor only", [] {
        auto sync = Stream_sync{};
        sync.configured(sr);
        (void)sync.next(block(realtime, 0), no_content);
        auto skipped = block(realtime, 384);
        skipped.skipping = true;
        expect_true(sync.next(skipped, no_content).restart == Restart::None, "skipping");
        expect_true(sync.next(skipped, no_content).restart == Restart::None, "still skipping");
        expect_true(sync.next(block(realtime, 1152), no_content).restart == Restart::Resume, "resume");
        expect_true(sync.next(block(realtime, 1536), no_content).restart == Restart::None, "then steady");
    });

    Tests::add("a stream restart while skipping isn't followed by a resume", [] {
        auto sync = Stream_sync{};
        sync.configured(sr);
        auto skipped = block(realtime, 0);
        skipped.skipping = true; // Before the restart snaps the bypass; after it, the block processes.
        expect_true(sync.next(skipped, no_content).restart == Restart::Stream, "stream restart");
        expect_true(sync.next(block(realtime, 384), no_content).restart == Restart::None, "no second restart");
    });

    Tests::add("without the hold, an offline pre-roll restarts at once", [] {
        auto sync = Stream_sync{false};
        sync.configured(sr);
        (void)sync.next(block(realtime, 0), no_content);
        const auto plan = sync.next(block(offline, -288, 288), no_content);
        expect_true(plan.restart == Restart::Stream && !plan.hold, "restart in the pre-roll");
    });

    Tests::add("offline, a discontinuity holds until the first block reaching position 0", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        (void)sync.next(block(realtime, 123456), no_content);
        auto plan = sync.next(block(offline, -672), no_content); // -672 + 384 = -288: still before 0.
        expect_true(plan.hold && plan.restart == Restart::None, "held");
        plan = sync.next(block(offline, -288, 288), no_content); // Ends exactly at 0: still pre-roll.
        expect_true(plan.hold, "a block ending at 0 is held");
        plan = sync.next(block(offline, 0), no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "restarted at the timeline");
        expect_true(sync.next(block(offline, 384), no_content).restart == Restart::None, "then steady");
    });

    Tests::add("a block straddling position 0 ends the hold", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        const auto plan = sync.next(block(offline, -100), no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "restarts at the straddling block");
    });

    Tests::add("a midstream render restarts at block 0", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        (void)sync.next(block(realtime, 0), no_content);
        const auto plan = sync.next(block(offline, 48000 * 4), no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "no hold");
    });

    Tests::add("real content, a missing position, leaving offline and the cap each end a hold", [] {
        const auto start = [] {
            auto sync = Stream_sync{true};
            sync.configured(sr);
            (void)sync.next(block(offline, -4000), no_content);
            expect_true(sync.holding(), "holding");
            return sync;
        };

        auto content = start();
        auto probed = false;
        auto plan = content.next(block(offline, -3616), [&] { probed = true; return true; });
        expect_true(probed && !plan.hold && plan.restart == Restart::Stream, "content restarts at once");

        auto missing = start();
        plan = missing.next(block(offline, std::nullopt), no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "no position: restart, today's behaviour");

        auto leaving = start();
        plan = leaving.next(block(realtime, -3616), no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "leaving offline restarts");

        auto stuck = Stream_sync{true};
        stuck.configured(sr);
        auto held = std::int64_t{};
        for (auto n = 0; n < 1000 && stuck.next(block(offline, -1'000'000'000), no_content).hold; ++n) held += 384;
        expect_true(!stuck.holding() && held >= static_cast<std::int64_t>(sr) && held < static_cast<std::int64_t>(sr) + 384, "capped at a second");
    });

    Tests::add("resets inside a hold don't restart its cap", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        auto held = std::int64_t{};
        for (auto n = 0; n < 1000; ++n) {
            auto reset = block(offline, -1'000'000'000);
            reset.host_reset = true;
            if (!sync.next(reset, no_content).hold) break;
            held += 384;
        }
        expect_true(held >= static_cast<std::int64_t>(sr) && held < static_cast<std::int64_t>(sr) + 384, "still capped at a second");
    });

    Tests::add("configure arriving during a hold starts afresh", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        (void)sync.next(block(offline, -4000), no_content);
        expect_true(sync.holding(), "holding");
        sync.configured(sr);
        expect_true(!sync.holding(), "a configure drops the hold");
        auto plan = sync.next(block(offline, -3616), no_content);
        expect_true(plan.hold, "a fresh processor offline in the pre-roll holds again");
        plan = sync.next(block(offline, 0), no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "and restarts once at the timeline");
    });

    Tests::add("a flush block can take the configure edge", [] {
        auto sync = Stream_sync{};
        sync.configured(sr);
        expect_true(sync.next(block(realtime, std::nullopt, 0), no_content).restart == Restart::Stream, "restart in the flush");
        expect_true(sync.next(block(realtime, 0), no_content).restart == Restart::None, "not again in the next block");
    });

    Tests::add("content is only probed while holding", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        auto probes = 0;
        const auto probe = [&] { ++probes; return false; };
        for (auto n = 0; n < 8; ++n) (void)sync.next(block(realtime, 384 * n), probe);
        (void)sync.next(block(offline, 4800), probe);
        expect_true(probes == 0, "never probed outside a hold");
    });

    Tests::add("realtime never holds, whatever the position", [] {
        auto sync = Stream_sync{true};
        sync.configured(sr);
        auto reset = block(realtime, -428, 428);
        reset.host_reset = true;
        const auto plan = sync.next(reset, no_content);
        expect_true(!plan.hold && plan.restart == Restart::Stream, "restart at once");
    });

    Tests::add("restart_values: the store, with offset-0 Sets folded in and everything else left", [] {
        const auto events = std::vector<Event::Ordered>{
            {.event = Event::Param{Event::Set{.value = 0.25, .address = 0}}, .offset = 0},
            {.event = Event::Param{Event::Set{.value = 0.75, .address = 1}}, .offset = 64},
            {.event = Event::Param{Event::Ramp{.value = 0.5, .address = 2, .samples = 32}}, .offset = 0},
            {.event = Event::Param{Event::Set{.value = 0.125, .address = 0}}, .offset = 0}, // Later at the same offset wins.
            {.event = Event::Param{Event::Set{.value = 9., .address = 7}}, .offset = 0},    // Out of range: ignored.
        };
        const auto values = restart_values<3>([](std::uint32_t address) { return 10. + address; }, events);
        expect_true(values[0] == 0.125, "offset-0 Set folded, last wins");
        expect_true(values[1] == 11., "a later Set waits for its offset");
        expect_true(values[2] == 12., "a Ramp stays an event");
    });

    Tests::add("any_signal and any_note", [] {
        auto silent = std::array<float, 8>{};
        auto loud = std::array<float, 8>{};
        loud[7] = 1e-30f;
        const auto both = std::array<const float*, 2>{silent.data(), loud.data()};
        expect_true(!any_signal({both.data(), 1}, 8) && any_signal(both, 8) && !any_signal(both, 7), "any non-zero sample");
        const auto notes = std::vector<Event::Ordered>{{.event = Note::Any{Note::On{}}, .offset = 3}};
        expect_true(any_note(notes) && !any_note({}), "a note");
    });
}

} // namespace

auto main() -> int
{
    add_tests();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
