// Task_manager without threads (TINY_PLATFORM_THREADS 0, the web format's nonisolated variant): every lane runs on
// main, from run_main. Built only in that tree; tasks_test covers the threaded manager.

#include <chrono>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tiny_core/platform_defs.hpp>
#include <tiny_core/task_manager.hpp>

static_assert(!TINY_PLATFORM_THREADS, "Built for the no-threads tree only.");

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;

auto add_tests() -> void
{
    Tests::add("no threads: background and serial run on main, from run_main, serial in order", [] {
        auto tasks = tiny::Task_manager{};
        tasks.bind_main(std::this_thread::get_id());
        const auto actor = tasks.actor();
        auto serial = std::vector<int>{};
        auto background = 0;
        auto off_main = 0;
        for (auto i = 0; i < 5; ++i) {
            actor.on_serial([&, i] { serial.push_back(i); off_main += actor.is_main_thread() ? 0 : 1; });
            actor.on_background([&] { ++background; });
        }
        expect_true(serial.empty() && background == 0, "a lane ran before run_main");
        for (auto i = 0; i < 100 && (serial.size() < 5 || background < 5); ++i) tasks.run_main();
        expect_true(serial == std::vector<int>{0, 1, 2, 3, 4}, "serial lane out of order or incomplete");
        expect_true(background == 5, "background lane incomplete");
        expect_true(off_main == 0, "a task ran off main");
    });

    Tests::add("no threads: a slice stops at its budget and the rest runs next time", [] {
        auto tasks = tiny::Task_manager{};
        tasks.bind_main(std::this_thread::get_id());
        auto ran = 0;
        for (auto i = 0; i < 20; ++i) {
            tasks.actor().on_background([&] {
                const auto until = std::chrono::steady_clock::now() + 1ms;
                while (std::chrono::steady_clock::now() < until) {}
                ++ran;
            });
        }
        tasks.run_main();
        expect_true(ran > 0 && ran < 20, "one slice ran everything or nothing");
        for (auto i = 0; i < 100 && ran < 20; ++i) tasks.run_main();
        expect_true(ran == 20, "later slices didn't finish the lane");
    });

    Tests::add("no threads: tasks posted from tasks run, and shutdown from inside a task refuses the rest", [] {
        auto tasks = tiny::Task_manager{};
        tasks.bind_main(std::this_thread::get_id());
        const auto actor = tasks.actor();
        auto chain = 0;
        actor.on_background([&] { ++chain; actor.on_serial([&] { ++chain; actor.on_main([&] { ++chain; }); }); });
        for (auto i = 0; i < 10; ++i) tasks.run_main();
        expect_true(chain == 3, "a task posted from a task didn't run");

        auto after = 0;
        actor.on_background([&] { tasks.shutdown(); });
        actor.on_background([&] { ++after; });
        tasks.run_main();
        expect_true(after == 0, "a task queued behind shutdown ran");
        expect_true(!actor.is_open() && !actor.on_main([] {}), "posting after shutdown was accepted");
        tasks.run_main(); // Nothing left, and nothing to wait for.
    });
}

} // namespace

auto main() -> int
{
    add_tests();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
