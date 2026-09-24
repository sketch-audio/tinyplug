#pragma once

#include <functional>
#include <memory>
#include <thread>

namespace tiny {

// GCD-style dispatch for the editor and worker: a main lane the view pumps, a background
// lane and a serial lane, each with its own thread.
//
// The host owns us and may destroy us at any time, so work posted from an async context
// (a dialog, a network completion) can outlive the owner. The queues live in a shared core
// that actors hold weakly, and `shutdown()` closes it: after it returns nothing new is
// accepted, nothing queued runs, and nothing is still running. Capturing a raw `this` in a
// task is therefore safe as long as the owner of `this` shuts the manager down before it
// dies. Every wrapper does that first in its destructor.
class Task_manager {
public:

    using Task = std::function<void()>;

    Task_manager();
    ~Task_manager(); // Shuts down.

    // Refuse new work, discard queued work, wait for running work, join the lanes.
    // Idempotent, and callable from any thread, including from inside a task.
    auto shutdown() -> void;

    // Owner responsible for binding to main thread. The first bind wins.
    auto bind_main(std::thread::id thread_id) -> void;
    auto is_main_thread() const -> bool;

    // Owner responsible for running the main thread tasks at an appropriate time.
    auto run_main() -> void;

    struct Core;

    // Posts work. Safe to copy into anything, including contexts that outlive the manager:
    // once it has shut down (or been destroyed) every post is refused and returns false.
    class Actor {
    public:
        Actor() = default;
        explicit Actor(Task_manager* receiver); // Null gives a no-op actor.
        auto is_main_thread() const -> bool;
        auto is_open() const -> bool; // False once shut down: a long task can return early.
        auto on_background(Task task) const -> bool;
        auto on_main(Task task) const -> bool;
        auto on_serial(Task task) const -> bool;
    private:
        std::weak_ptr<Core> _core{};
    };

    auto actor() -> Actor;

private:

    std::shared_ptr<Core> _core;
    std::thread _background{};
    std::thread _serial{};

    // No copy, no move.
    Task_manager(const Task_manager&) = delete;
    auto operator=(const Task_manager&) = delete;
    Task_manager(Task_manager&&) = delete;
    auto operator=(Task_manager&&) = delete;

};

} // namespace tiny
