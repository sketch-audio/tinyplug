#include "tiny_core/task_manager.hpp"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <utility>

namespace tiny {

// Everything an async context can reach. Shared so a late post finds it closed rather than
// freed; the threads belong to the manager, so the core's destructor never joins anything.
struct Task_manager::Core {
    std::mutex mutex{};
    std::condition_variable wake{}; // Lanes: work arrived or closed.
    std::condition_variable idle{}; // Shutdown: a running task finished.
    bool closed{};
    int running{};
    std::deque<Task> main{}, background{}, serial{};
    std::atomic<std::thread::id> main_id{}; // Default id: not bound yet.

    auto post(std::deque<Task>& lane, Task task) -> bool
    {
        {
            const auto lock = std::lock_guard{mutex};
            if (closed) return false;
            lane.push_back(std::move(task));
        }
        wake.notify_all();
        return true;
    }
};

namespace {

// The core whose task this thread is running, so a task may shut its own manager down
// without waiting for itself.
thread_local const Task_manager::Core* t_running = nullptr;

// Pops and runs one task from `lane` unless closed. With `wait`, blocks for work.
auto run_one(Task_manager::Core& core, std::deque<Task_manager::Task>& lane, bool wait) -> bool
{
    auto task = Task_manager::Task{};
    {
        auto lock = std::unique_lock{core.mutex};
        if (wait) core.wake.wait(lock, [&] { return core.closed || !lane.empty(); });
        if (core.closed || lane.empty()) return false;
        task = std::move(lane.front());
        lane.pop_front();
        ++core.running;
    }

    const auto* outer = std::exchange(t_running, &core);
    task();
    task = nullptr; // Captures die before shutdown can see this task as finished.
    t_running = outer;

    {
        const auto lock = std::lock_guard{core.mutex};
        --core.running;
    }
    core.idle.notify_all();
    return true;
}

auto lane_thread(std::shared_ptr<Task_manager::Core> core, std::deque<Task_manager::Task> Task_manager::Core::* lane) -> std::thread
{
    return std::thread{[core = std::move(core), lane] {
        while (run_one(*core, (*core).*lane, true)) {}
    }};
}

} // namespace

Task_manager::Task_manager()
    : _core{std::make_shared<Core>()},
      _background{lane_thread(_core, &Core::background)},
      _serial{lane_thread(_core, &Core::serial)}
{
}

Task_manager::~Task_manager()
{
    shutdown();
}

auto Task_manager::shutdown() -> void
{
    auto discarded = std::deque<Task>{};
    {
        auto lock = std::unique_lock{_core->mutex};
        _core->closed = true;
        for (auto* lane : {&_core->main, &_core->background, &_core->serial}) {
            std::move(lane->begin(), lane->end(), std::back_inserter(discarded));
            lane->clear();
        }
        _core->wake.notify_all();

        // Wait for tasks already running on any lane, main included (hosts destroy us off main).
        const auto self = (t_running == _core.get()) ? 1 : 0;
        _core->idle.wait(lock, [&] { return _core->running == self; });
    }
    discarded.clear(); // Outside the lock: a capture's destructor may post, and is refused.

    for (auto* lane : {&_background, &_serial}) {
        if (!lane->joinable()) continue;
        if (lane->get_id() == std::this_thread::get_id()) lane->detach(); // Shut down from its own task; it exits on return.
        else lane->join();
    }
}

auto Task_manager::bind_main(std::thread::id thread_id) -> void
{
    auto unbound = std::thread::id{};
    _core->main_id.compare_exchange_strong(unbound, thread_id, std::memory_order_acq_rel);
}

auto Task_manager::is_main_thread() const -> bool
{
    const auto main_id = _core->main_id.load(std::memory_order_acquire);
    return main_id != std::thread::id{} && std::this_thread::get_id() == main_id;
}

auto Task_manager::run_main() -> void
{
    assert(is_main_thread() && "run_main must be called from the main thread!");
    while (run_one(*_core, _core->main, false)) {}
}

auto Task_manager::actor() -> Actor
{
    return Actor{this};
}

// MARK: - Actor

Task_manager::Actor::Actor(Task_manager* receiver)
{
    if (receiver) _core = receiver->_core;
}

auto Task_manager::Actor::is_main_thread() const -> bool
{
    const auto core = _core.lock();
    if (!core) return false;
    const auto main_id = core->main_id.load(std::memory_order_acquire);
    return main_id != std::thread::id{} && std::this_thread::get_id() == main_id;
}

auto Task_manager::Actor::is_open() const -> bool
{
    const auto core = _core.lock();
    if (!core) return false;
    const auto lock = std::lock_guard{core->mutex};
    return !core->closed;
}

auto Task_manager::Actor::on_background(Task task) const -> bool
{
    const auto core = _core.lock();
    return core && core->post(core->background, std::move(task));
}

auto Task_manager::Actor::on_main(Task task) const -> bool
{
    const auto core = _core.lock();
    return core && core->post(core->main, std::move(task));
}

auto Task_manager::Actor::on_serial(Task task) const -> bool
{
    const auto core = _core.lock();
    return core && core->post(core->serial, std::move(task));
}

} // namespace tiny
