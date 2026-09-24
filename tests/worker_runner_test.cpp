// Worker_runner: delivery from both inbound queues on the worker thread, the post-cycle and update
// hooks, and start/stop cycles with traffic in flight. Run under TSan too (the `tsan` preset).

#include <atomic>
#include <chrono>
#include <format>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>
#include <tinyplug/tiny_worker.hpp>

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;
using Work = tiny::User_work;

// Records everything the runner does to it. Only the worker thread writes the plain members;
// the test reads them after stop(), which joins.
struct Probe_worker {
    std::thread::id thread{};
    double sample_rate{};
    std::vector<uint32_t> jobs{};
    std::vector<uint32_t> requests{};
    int starts{}, stops{};
    bool wrong_thread{};
    bool handled_while_stopped{};
    std::atomic<int> updates{};
    std::atomic<uint32_t> handled{}; // What the test waits on; the vectors are read after stop().
    std::atomic<bool> running{};

    auto on_start(double sr) -> void
    {
        thread = std::this_thread::get_id();
        sample_rate = sr;
        ++starts;
        running = true;
    }
    auto on_stop() -> void { running = false; ++stops; }
    auto on_update() -> void { updates.fetch_add(1); }

    auto handle_from_processor(const Work::From_processor& m) -> void
    {
        _check();
        jobs.push_back(std::get<Work::Job>(m).seq);
        handled.fetch_add(1);
    }
    auto handle_from_editor(const Work::From_editor& m) -> void
    {
        _check();
        requests.push_back(std::get<Work::Request>(m).seq);
        handled.fetch_add(1);
    }

private:
    auto _check() -> void
    {
        if (std::this_thread::get_id() != thread) wrong_thread = true;
        if (!running) handled_while_stopped = true;
    }
};

using Runner = tiny::Worker_runner<Probe_worker>;

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

auto in_sequence(const std::vector<uint32_t>& got, uint32_t n) -> bool
{
    if (got.size() != n) return false;
    for (auto i = uint32_t{}; i < n; ++i) if (got[i] != i) return false;
    return true;
}

// Producers on their own threads, as the audio thread and the editor are; retry on full.
auto produce(Runner::From_proc_queue& from_proc, Runner::From_edit_queue& from_edit, uint32_t n) -> void
{
    auto audio = std::thread{[&] { for (auto i = uint32_t{}; i < n; ++i) while (!from_proc.push(Work::Job{i})) std::this_thread::yield(); }};
    auto editor = std::thread{[&] { for (auto i = uint32_t{}; i < n; ++i) while (!from_edit.push(Work::Request{i})) std::this_thread::yield(); }};
    audio.join();
    editor.join();
}

auto add_tests() -> void
{
    Tests::add("delivers both channels in order, on the worker thread, between on_start and on_stop", [] {
        auto worker = Probe_worker{};
        auto from_proc = Runner::From_proc_queue{};
        auto from_edit = Runner::From_edit_queue{};
        auto runner = Runner{&worker, &from_proc, &from_edit};
        runner.start(48000.);
        produce(from_proc, from_edit, 5000);
        expect_true(wait_until([&] { return worker.handled.load() == 10'000; }), "never drained");
        runner.stop();
        expect_true(worker.starts == 1 && worker.stops == 1, std::format("starts {} stops {}", worker.starts, worker.stops));
        expect_true(worker.sample_rate == 48000., "on_start got the wrong sample rate");
        expect_true(worker.thread != std::this_thread::get_id(), "ran on the caller's thread");
        expect_true(!worker.wrong_thread && !worker.handled_while_stopped, "handled off the worker thread or outside start/stop");
        expect_true(in_sequence(worker.jobs, 5000), std::format("{} jobs, not 0..4999 in order", worker.jobs.size()));
        expect_true(in_sequence(worker.requests, 5000), std::format("{} requests, not 0..4999 in order", worker.requests.size()));
    });

    Tests::add("the post-cycle and update hooks run on the worker thread with no traffic", [] {
        auto worker = Probe_worker{};
        auto from_proc = Runner::From_proc_queue{};
        auto from_edit = Runner::From_edit_queue{};
        auto runner = Runner{&worker, &from_proc, &from_edit};
        auto cycles = std::atomic<int>{};
        auto cycle_thread = std::atomic<bool>{true};
        runner.set_post_cycle([&] {
            cycles.fetch_add(1);
            if (!worker.running.load()) cycle_thread = false;
        });
        runner.start(44100.);
        expect_true(wait_until([&] { return cycles.load() >= 5 && worker.updates.load() >= 5; }), "hooks didn't run");
        runner.stop();
        expect_true(cycle_thread.load(), "a post-cycle ran outside start/stop");
    });

    Tests::add("start and stop are idempotent, and the destructor stops a running worker", [] {
        auto worker = Probe_worker{};
        auto from_proc = Runner::From_proc_queue{};
        auto from_edit = Runner::From_edit_queue{};
        {
            auto runner = Runner{&worker, &from_proc, &from_edit};
            runner.stop(); // Never started.
            runner.start(48000.);
            runner.start(96000.);
        }
        expect_true(worker.starts == 1 && worker.stops == 1, std::format("starts {} stops {}", worker.starts, worker.stops));
        expect_true(worker.sample_rate == 48000., "a second start restarted the worker");
    });

    // Hosts activate and deactivate freely; messages sent while stopped wait for the next start.
    Tests::add("start/stop cycles with traffic in flight lose nothing", [] {
        auto worker = Probe_worker{};
        auto from_proc = Runner::From_proc_queue{};
        auto from_edit = Runner::From_edit_queue{};
        auto runner = Runner{&worker, &from_proc, &from_edit};
        constexpr auto n = uint32_t{20'000};
        auto producers = std::thread{[&] { produce(from_proc, from_edit, n); }};
        for (auto cycle = 0; cycle < 100; ++cycle) {
            runner.start(48000.);
            std::this_thread::sleep_for(200us);
            runner.stop();
        }
        producers.join();
        runner.start(48000.);
        expect_true(wait_until([&] { return worker.handled.load() == 2 * n; }), "never drained");
        runner.stop();
        expect_true(worker.starts == worker.stops, "unbalanced on_start/on_stop");
        expect_true(!worker.wrong_thread && !worker.handled_while_stopped, "handled outside start/stop");
        expect_true(in_sequence(worker.jobs, n), std::format("{} jobs, not 0..{} in order", worker.jobs.size(), n - 1));
        expect_true(in_sequence(worker.requests, n), std::format("{} requests, not 0..{} in order", worker.requests.size(), n - 1));
    });
}

} // namespace

auto main() -> int
{
    add_tests();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
