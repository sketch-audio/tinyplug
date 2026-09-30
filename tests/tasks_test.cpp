// The task and timer primitives: Notification_queue, Serial_queue, Task_launcher, Task_manager
// and Relay. Mostly lifetime and threading, so run under TSan too (the `tsan` preset).

#include <atomic>
#include <chrono>
#include <format>
#include <latch>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tiny_core/notification_queue.hpp>
#include <tiny_core/platform_defs.hpp>
#include <tiny_core/relay.hpp>
#include <tiny_core/serial_queue.hpp>
#include <tiny_core/task_launcher.hpp>
#include <tiny_core/task_manager.hpp>

#if TINY_PLATFORM_APPLE
#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#elif TINY_PLATFORM_WINDOWS
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;

// Waits for `done` up to a generous deadline, so a slow sanitizer run doesn't flake.
template<typename Fn>
auto wait_until(Fn done, std::chrono::milliseconds limit = 5000ms) -> bool
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

// MARK: - Notification_queue

auto add_notification_queue() -> void
{
    Tests::add("Notification_queue: FIFO, try_pop on empty fails", [] {
        auto q = tiny::Notification_queue{};
        auto order = std::vector<int>{};
        for (auto i = 0; i < 5; ++i) q.push([&order, i] { order.push_back(i); });
        auto task = tiny::Notification_queue::Task{};
        while (q.try_pop(task)) task();
        expect_true(order == std::vector<int>{0, 1, 2, 3, 4}, "tasks ran out of order");
        expect_true(!q.try_pop(task), "try_pop on an empty queue");
    });

    Tests::add("Notification_queue: pop blocks until a push, done() releases a blocked pop", [] {
        auto q = tiny::Notification_queue{};
        auto got = std::atomic<int>{};
        auto consumer = std::thread{[&] {
            auto task = tiny::Notification_queue::Task{};
            while (q.pop(task)) task();
            got.fetch_add(100); // Reached only once done() is seen with the queue empty.
        }};
        q.push([&] { got.fetch_add(1); });
        expect_true(wait_until([&] { return got.load() == 1; }), "a blocked pop must wake for a push");
        q.done();
        consumer.join();
        expect_true(got.load() == 101, "done() must release the consumer");
    });

    Tests::add("Notification_queue: tasks pushed before done() still drain", [] {
        auto q = tiny::Notification_queue{};
        auto ran = 0;
        for (auto i = 0; i < 3; ++i) q.push([&] { ++ran; });
        q.done();
        auto task = tiny::Notification_queue::Task{};
        while (q.pop(task)) task();
        expect_true(ran == 3, "pending tasks were dropped at done()");
    });
}

// MARK: - Serial_queue

auto add_serial_queue() -> void
{
    Tests::add("Serial_queue: in order, on one thread that isn't the caller's, drained at destruction", [] {
        constexpr auto n = 1000;
        auto order = std::vector<int>{};
        auto threads = std::vector<std::thread::id>{};
        {
            auto q = tiny::Serial_queue{};
            for (auto i = 0; i < n; ++i) {
                q.push([&order, &threads, i] { order.push_back(i); threads.push_back(std::this_thread::get_id()); });
            }
        } // Destructor: done, then join.
        expect_true(static_cast<int>(order.size()) == n, std::format("{} of {} tasks ran", order.size(), n));
        for (auto i = 0; i < static_cast<int>(order.size()); ++i) expect_true(order[static_cast<size_t>(i)] == i, "out of order");
        for (const auto& id : threads) expect_true(id == threads.front() && id != std::this_thread::get_id(), "not one background thread");
    });

    Tests::add("Serial_queue: pushes from many threads all run, one at a time", [] {
        auto running = std::atomic<int>{}, ran = std::atomic<int>{};
        auto overlap = std::atomic<bool>{};
        {
            auto q = tiny::Serial_queue{};
            auto pushers = std::vector<std::thread>{};
            for (auto t = 0; t < 4; ++t) {
                pushers.emplace_back([&] {
                    for (auto i = 0; i < 250; ++i) {
                        q.push([&] {
                            if (running.fetch_add(1) != 0) overlap = true;
                            ran.fetch_add(1);
                            running.fetch_sub(1);
                        });
                    }
                });
            }
            for (auto& p : pushers) p.join();
        }
        expect_true(ran.load() == 1000, std::format("{} of 1000 ran", ran.load()));
        expect_true(!overlap.load(), "two serial tasks overlapped");
    });
}

// MARK: - Task_launcher

auto add_task_launcher() -> void
{
    Tests::add("Task_launcher: launches from many threads all run before destruction returns", [] {
        auto ran = std::atomic<int>{};
        {
            auto launcher = tiny::Task_launcher{};
            auto actor = launcher.actor();
            auto launchers = std::vector<std::thread>{};
            for (auto t = 0; t < 4; ++t) {
                launchers.emplace_back([&] { for (auto i = 0; i < 250; ++i) actor.launch([&] { ran.fetch_add(1); }); });
            }
            for (auto& l : launchers) l.join();
        }
        expect_true(ran.load() == 1000, std::format("{} of 1000 ran", ran.load()));
    });

    Tests::add("Task_launcher: a null actor does nothing", [] {
        auto ran = false;
        tiny::Task_launcher::Actor{}.launch([&] { ran = true; });
        expect_true(!ran, "a null actor ran a task");
    });
}

// MARK: - Task_manager

auto add_task_manager() -> void
{
    Tests::add("Task_manager: is_main_thread follows the first bind only", [] {
        auto tm = tiny::Task_manager{};
        expect_true(!tm.is_main_thread(), "unbound must not report main");
        tm.bind_main(std::this_thread::get_id());
        expect_true(tm.is_main_thread(), "the bound thread is main");
        auto other = std::thread::id{};
        auto other_is_main = true;
        std::thread{[&] { other = std::this_thread::get_id(); other_is_main = tm.is_main_thread(); }}.join();
        expect_true(!other_is_main, "another thread is not main");
        tm.bind_main(other);
        expect_true(tm.is_main_thread(), "a second bind must not move main");
    });

    // run_frame binds every frame while any thread may ask; this was a plain optional.
    Tests::add("Task_manager: is_main_thread may be asked from any thread while main binds", [] {
        auto tm = tiny::Task_manager{};
        auto stop = std::atomic<bool>{};
        auto askers = std::vector<std::thread>{};
        auto wrong = std::atomic<bool>{};
        for (auto t = 0; t < 3; ++t) {
            askers.emplace_back([&] {
                while (!stop.load()) if (tm.actor().is_main_thread()) wrong = true;
            });
        }
        for (auto i = 0; i < 10'000; ++i) tm.bind_main(std::this_thread::get_id());
        stop = true;
        for (auto& a : askers) a.join();
        expect_true(!wrong.load(), "a background thread was told it is main");
    });

    Tests::add("Task_manager: on_main from other threads runs on main, in order per thread", [] {
        auto tm = tiny::Task_manager{};
        tm.bind_main(std::this_thread::get_id());
        auto actor = tm.actor();
        auto order = std::vector<std::vector<int>>(4);
        auto on_main = true;
        auto posters = std::vector<std::thread>{};
        for (auto t = 0; t < 4; ++t) {
            posters.emplace_back([&, t] {
                for (auto i = 0; i < 250; ++i) { // More than the old 16-slot queue held: no cap now.
                    actor.on_main([&, t, i] { order[static_cast<size_t>(t)].push_back(i); on_main = on_main && tm.is_main_thread(); });
                }
            });
        }
        for (auto& p : posters) p.join();
        tm.run_main();
        auto expected = std::vector<int>(250);
        for (auto i = 0; i < 250; ++i) expected[static_cast<size_t>(i)] = i;
        for (const auto& list : order) expect_true(list == expected, "a thread's tasks ran out of order or went missing");
        expect_true(on_main, "an on_main task ran off main");
    });

    Tests::add("Task_manager: on_background and on_serial run off main", [] {
        auto tm = tiny::Task_manager{};
        tm.bind_main(std::this_thread::get_id());
        auto actor = tm.actor();
        auto background = std::atomic<int>{}, serial = std::atomic<int>{};
        for (auto i = 0; i < 100; ++i) {
            actor.on_background([&] { if (!tm.is_main_thread()) background.fetch_add(1); });
            actor.on_serial([&] { if (!tm.is_main_thread()) serial.fetch_add(1); });
        }
        expect_true(wait_until([&] { return background.load() == 100 && serial.load() == 100; }),
                    std::format("background {} serial {} of 100", background.load(), serial.load()));
    });
}

// MARK: - Task_manager shutdown

// Until the lane is closed: a refused post is how a test sees shutdown has begun.
auto wait_closed(const tiny::Task_manager::Actor& actor) -> bool
{
    return wait_until([&] { return !actor.on_serial([] {}); });
}

auto add_task_manager_shutdown() -> void
{
    Tests::add("shutdown: posts are refused afterwards, and queued work never runs", [] {
        auto tm = tiny::Task_manager{};
        tm.bind_main(std::this_thread::get_id());
        auto actor = tm.actor();
        auto ran = std::atomic<int>{};
        for (auto i = 0; i < 10; ++i) actor.on_main([&] { ran.fetch_add(1); });
        expect_true(actor.is_open(), "open before shutdown");
        tm.shutdown();
        expect_true(!actor.is_open(), "closed after shutdown");
        tm.run_main();
        expect_true(!actor.on_main([&] { ran.fetch_add(1); }), "on_main after shutdown");
        expect_true(!actor.on_background([&] { ran.fetch_add(1); }), "on_background after shutdown");
        expect_true(!actor.on_serial([&] { ran.fetch_add(1); }), "on_serial after shutdown");
        tm.shutdown(); // Idempotent.
        std::this_thread::sleep_for(20ms);
        expect_true(ran.load() == 0, std::format("{} tasks ran after shutdown", ran.load()));
    });

    // What a late NSURLSession completion or dialog sheet holds.
    Tests::add("shutdown: an actor that outlives its manager is a safe no-op", [] {
        auto actor = tiny::Task_manager::Actor{};
        {
            auto tm = tiny::Task_manager{};
            tm.bind_main(std::this_thread::get_id());
            actor = tm.actor();
            expect_true(actor.is_main_thread(), "live actor");
        }
        auto ran = false;
        expect_true(!actor.on_main([&] { ran = true; }) && !actor.on_background([&] { ran = true; }) && !actor.on_serial([&] { ran = true; }), "posts to a destroyed manager");
        expect_true(!actor.is_main_thread() && !ran, "a destroyed manager has no main thread and runs nothing");
        expect_true(!tiny::Task_manager::Actor{nullptr}.on_main([] {}), "a null actor refuses");
        expect_true(!actor.is_open() && !tiny::Task_manager::Actor{}.is_open(), "neither is open");
    });

    Tests::add("shutdown: waits for the running task, discards the queue behind it", [] {
        auto tm = tiny::Task_manager{};
        auto actor = tm.actor();
        auto started = std::latch{1}, go = std::latch{1};
        auto finished = std::atomic<bool>{};
        auto behind = std::atomic<int>{};
        actor.on_background([&] { started.count_down(); go.wait(); finished = true; });
        for (auto i = 0; i < 5; ++i) actor.on_background([&] { behind.fetch_add(1); });
        started.wait();

        auto returned = std::atomic<bool>{};
        auto closer = std::thread{[&] { tm.shutdown(); returned = true; }};
        expect_true(wait_closed(actor), "shutdown never closed the lanes");
        std::this_thread::sleep_for(20ms);
        expect_true(!returned.load(), "shutdown returned while a task was still running");
        go.count_down();
        closer.join();
        expect_true(finished.load(), "the running task was cut short");
        expect_true(behind.load() == 0, std::format("{} queued tasks ran after shutdown began", behind.load()));
    });

    // Hosts destroy instances off main (Ableton's AUv3), so shutdown must wait for main too.
    Tests::add("shutdown: from another thread, waits for a main task that is running", [] {
        auto tm = tiny::Task_manager{};
        auto actor = tm.actor();
        auto started = std::latch{1}, go = std::latch{1};
        auto finished = std::atomic<bool>{};
        auto main = std::thread{[&] {
            tm.bind_main(std::this_thread::get_id());
            actor.on_main([&] { started.count_down(); go.wait(); finished = true; });
            tm.run_main();
        }};
        started.wait();
        auto returned = std::atomic<bool>{};
        auto closer = std::thread{[&] { tm.shutdown(); returned = true; }};
        expect_true(wait_closed(actor), "shutdown never closed the lanes");
        std::this_thread::sleep_for(20ms);
        expect_true(!returned.load(), "shutdown returned while a main task was still running");
        go.count_down();
        closer.join();
        main.join();
        expect_true(finished.load(), "the main task was cut short");
    });

    Tests::add("shutdown: a task posting while shutdown waits for it is refused", [] {
        auto tm = tiny::Task_manager{};
        auto actor = tm.actor();
        auto started = std::latch{1}, go = std::latch{1};
        auto accepted = std::atomic<int>{-1};
        actor.on_background([&] {
            started.count_down();
            go.wait();
            accepted = static_cast<int>(actor.on_main([] {})) + static_cast<int>(actor.on_serial([] {})) + static_cast<int>(actor.on_background([] {}));
        });
        started.wait();
        auto closer = std::thread{[&] { tm.shutdown(); }};
        expect_true(wait_closed(actor), "shutdown never closed the lanes");
        go.count_down();
        closer.join();
        expect_true(accepted.load() == 0, std::format("{} posts from inside shutdown were accepted", accepted.load()));
    });

    Tests::add("shutdown: a task may shut down, or destroy, its own manager", [] {
        auto tm = tiny::Task_manager{};
        auto done = std::atomic<bool>{};
        tm.actor().on_serial([&] { tm.shutdown(); done = true; });
        expect_true(wait_until([&] { return done.load(); }), "shutdown from its own task deadlocked");

        auto* owned = new tiny::Task_manager{};
        auto destroyed = std::atomic<bool>{};
        owned->actor().on_background([owned, &destroyed] { delete owned; destroyed = true; });
        expect_true(wait_until([&] { return destroyed.load(); }), "destruction from its own task deadlocked");
    });

    // The shape of a real teardown: async posts on every lane while the owner shuts down.
    Tests::add("shutdown: posters on every lane racing it, nothing runs once it returns", [] {
        for (auto round = 0; round < 10; ++round) {
            auto tm = std::make_unique<tiny::Task_manager>();
            tm->bind_main(std::this_thread::get_id());
            auto actor = tm->actor();
            auto ran = std::atomic<int>{};
            auto posters = std::vector<std::thread>{};
            for (auto t = 0; t < 3; ++t) {
                posters.emplace_back([&, actor] {
                    // Bounded and yielding: the lanes are unbounded, so a tight loop only measures memory.
                    for (auto i = 0; i < 2000; ++i) {
                        const auto background = actor.on_background([&] { ran.fetch_add(1); });
                        const auto serial = actor.on_serial([&] { ran.fetch_add(1); });
                        const auto main = actor.on_main([&] { ran.fetch_add(1); });
                        if (!background && !serial && !main) break;
                        std::this_thread::yield();
                    }
                });
            }
            expect_true(wait_until([&] { return ran.load() > 100; }), "posters never got going");
            tm->run_main();
            tm->shutdown();
            const auto at_shutdown = ran.load();
            tm->run_main();
            for (auto& p : posters) p.join();
            std::this_thread::sleep_for(2ms);
            expect_true(ran.load() == at_shutdown, std::format("round {}: {} tasks ran after shutdown returned", round, ran.load() - at_shutdown));
            tm.reset();
        }
    });
}

// MARK: - Relay

#if TINY_PLATFORM_APPLE || TINY_PLATFORM_WINDOWS

#if TINY_PLATFORM_APPLE
// The callback hops to the main dispatch queue, which a CLI only services while its run loop runs.
auto pump_main(std::chrono::milliseconds duration) -> void
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(duration).count(), false);
}

auto on_main() -> bool { return pthread_main_np() != 0; }
#else
// The callback arrives as a message to the relay's window, created on this thread by the first relay.
auto pump_main(std::chrono::milliseconds duration) -> void
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    for (;;) {
        auto msg = MSG{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left.count()), QS_ALLINPUT);
    }
}

const auto g_main_thread = std::this_thread::get_id();
auto on_main() -> bool { return std::this_thread::get_id() == g_main_thread; }
#endif

auto add_relay() -> void
{
    Tests::add("Relay: idle never fires; posts coalesce and run on main", [] {
        auto calls = std::atomic<int>{};
        auto off_main = std::atomic<bool>{};
        auto relay = tiny::Relay{{.execute = [&] { calls.fetch_add(1); if (!on_main()) off_main = true; }, .interval = 0.01}};

        pump_main(100ms);
        expect_true(calls.load() == 0, "an idle relay fired");

        auto poster = std::thread{[&] { for (auto i = 0; i < 10'000; ++i) relay.post(); }};
        poster.join();
        pump_main(200ms);
        expect_true(calls.load() >= 1, "a post never fired");
        expect_true(calls.load() <= 3, std::format("10000 posts in one burst fired {} times", calls.load()));
        expect_true(!off_main.load(), "the callback ran off main");
    });

    Tests::add("Relay: nothing fires after destruction, even with a post pending", [] {
        auto calls = std::make_shared<std::atomic<int>>();
        {
            auto relay = tiny::Relay{{.execute = [calls] { calls->fetch_add(1); }, .interval = 0.01}};
            relay.post();
        } // Destroyed on main before the timer can deliver.
        pump_main(100ms);
        expect_true(calls->load() == 0, "a destroyed relay fired");
    });

    Tests::add("Relay: repeating fires every interval without a post", [] {
        auto calls = std::atomic<int>{};
        auto relay = tiny::Relay{{.execute = [&] { calls.fetch_add(1); }, .interval = 0.01, .repeating = true}};
        pump_main(150ms);
        expect_true(calls.load() >= 3, std::format("a repeating relay fired {} times in 150 ms", calls.load()));
    });

    Tests::add("Relay: a stop off main waits out a delivery in flight", [] {
        auto inside = std::atomic<bool>{};
        auto finished = std::atomic<bool>{};
        auto finished_at_stop = std::atomic<bool>{};
        auto relay = std::make_unique<tiny::Relay>(tiny::Relay::Spec{
            .execute = [&] {
                if (finished.load()) return;
                inside = true;
                std::this_thread::sleep_for(50ms); // The owner is mid-use when the stop arrives.
                finished = true;
            },
            .interval = 0.005,
            .repeating = true,
        });
        auto stopper = std::thread{[&] {
            while (!inside.load()) std::this_thread::yield();
            relay.reset(); // Off main, while main is inside `execute`.
            finished_at_stop = finished.load();
        }};
        pump_main(300ms);
        stopper.join();
        expect_true(finished_at_stop.load(), "the stop returned while a delivery was still running");
    });

    Tests::add("Relay: posting from the audio side while main destroys it", [] {
        for (auto round = 0; round < 50; ++round) {
            auto calls = std::make_shared<std::atomic<int>>();
            auto relay = std::make_unique<tiny::Relay>(tiny::Relay::Spec{.execute = [calls] { calls->fetch_add(1); }, .interval = 0.001});
            auto stop = std::atomic<bool>{};
            auto poster = std::thread{[&] { while (!stop.load()) relay->post(); }};
            pump_main(5ms);
            stop = true;
            poster.join();
            relay.reset();
            const auto at_destruction = calls->load();
            pump_main(5ms);
            expect_true(calls->load() == at_destruction, "fired after destruction");
        }
    });

#if TINY_PLATFORM_WINDOWS
    // Why a window and not a thread message: modal loops (message boxes, host dialogs) drop thread
    // messages, and a host can sit in one for as long as the user likes.
    Tests::add("Relay: delivers on main during a modal loop", [] {
        auto calls = std::atomic<int>{};
        auto relay = tiny::Relay{{.execute = [&] { calls.fetch_add(1); }, .interval = 0.01}};
        auto during = std::atomic<int>{-1};
        const auto main_thread = GetCurrentThreadId();
        auto closer = std::thread{[&] {
            // The box is the dialog window main owns. Bounded, so a miss fails instead of hanging.
            auto box = HWND{};
            for (auto tries = 0; !box && tries < 1000; ++tries) {
                EnumThreadWindows(main_thread, [](HWND w, LPARAM out) -> BOOL {
                    wchar_t name[16]{};
                    if (GetClassNameW(w, name, 16) && !wcscmp(name, L"#32770")) { *reinterpret_cast<HWND*>(out) = w; return FALSE; }
                    return TRUE;
                }, reinterpret_cast<LPARAM>(&box));
                if (!box) std::this_thread::sleep_for(5ms);
            }
            relay.post();
            std::this_thread::sleep_for(150ms);
            during = calls.load(); // Before the box closes.
            for (auto tries = 0; box && IsWindow(box) && tries < 100; ++tries) {
                PostMessageW(box, WM_CLOSE, 0, 0);
                std::this_thread::sleep_for(20ms);
            }
        }};
        MessageBoxW(nullptr, L"Closes itself.", L"tinyplug relay test", MB_OK);
        closer.join();
        expect_true(during.load() >= 1, "a post didn't fire while a modal loop ran");
    });

    Tests::add("Relay: a window lost with a short-lived thread is rebuilt by the next relay", [] {
        auto first_calls = std::atomic<int>{};
        auto first = std::unique_ptr<tiny::Relay>{};
        std::thread{[&] { first = std::make_unique<tiny::Relay>(tiny::Relay::Spec{.execute = [&] { first_calls.fetch_add(1); }, .interval = 0.01}); }}.join();
        // That thread's exit destroyed the window. The next relay, built here, rebuilds it on main.
        auto calls = std::atomic<int>{};
        auto off_main = std::atomic<bool>{};
        auto second = tiny::Relay{{.execute = [&] { calls.fetch_add(1); if (!on_main()) off_main = true; }, .interval = 0.01}};
        first->post();
        second.post();
        pump_main(100ms);
        expect_true(calls.load() >= 1 && first_calls.load() >= 1, "relays went silent after their window's thread exited");
        expect_true(!off_main.load(), "the rebuilt window delivers off main");
        first.reset();
    });

    Tests::add("Relay: the last stop off main leaves no window behind, and a new relay works", [] {
        for (auto round = 0; round < 3; ++round) {
            auto relay = std::make_unique<tiny::Relay>(tiny::Relay::Spec{.execute = [] {}, .interval = 0.01});
            auto stopper = std::thread{[&] { relay.reset(); }}; // The window's destruction is deferred to main.
            stopper.join();
            pump_main(20ms);
            auto calls = std::atomic<int>{};
            auto again = tiny::Relay{{.execute = [&] { calls.fetch_add(1); }, .interval = 0.01}};
            again.post();
            pump_main(100ms);
            expect_true(calls.load() >= 1, "a relay after an off-main teardown never fired");
        }
    });
#endif
}

#endif

} // namespace

auto main() -> int
{
    add_notification_queue();
    add_serial_queue();
    add_task_launcher();
    add_task_manager();
    add_task_manager_shutdown();
#if TINY_PLATFORM_APPLE || TINY_PLATFORM_WINDOWS
    add_relay();
#endif
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
