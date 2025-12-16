#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "notification_queue.hpp"

namespace tiny {

// See: https://youtu.be/zULU6Hhp42w
class Task_launcher {
public:

    using Task = Notification_queue::Task;

    Task_launcher();
    ~Task_launcher();

    auto launch(Task task) -> void;

    // An interface to launch tasks on a receiver. Does nothing in case of null receiver!
    class Actor {
    public:
        explicit Actor(Task_launcher* receiver = nullptr) : _receiver{receiver} {}
        auto launch(Task task) const -> void;
    private:
        friend class Task_launcher;
        Task_launcher* _receiver{nullptr};
    };

    // Get an actor to launch tasks.
    auto actor() -> Actor;

private:

    size_t _num_threads{std::thread::hardware_concurrency()};
    std::vector<std::thread> _threads{};
    std::vector<Notification_queue> _queues{_num_threads};
    std::atomic<size_t> _idx{};

    auto run(size_t i) -> void;

    // No copy, no move.
    Task_launcher(const Task_launcher&) = delete;
    auto operator=(const Task_launcher&) = delete;
    Task_launcher(Task_launcher&&) = delete;
    auto operator=(Task_launcher&&) = delete;

};

} // namespace tiny