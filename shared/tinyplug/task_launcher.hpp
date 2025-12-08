#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace tiny {

// See: https://youtu.be/zULU6Hhp42w
class Task_launcher {
public:

    using Task = std::function<void()>;

    Task_launcher();
    ~Task_launcher();

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

    struct Delayed_task {
        using Time_point = std::chrono::steady_clock::time_point;
        Task task{};
        Time_point time_point{};
        auto operator>(const Delayed_task& other) const -> bool {
            return time_point > other.time_point;
        }
    };

    class Queue {
    public:
        auto push(Task task) -> void;
        auto try_push(Task task) -> bool;
        auto pop(Task& task) -> bool;
        auto try_pop(Task& task) -> bool;
        auto done() -> void;
    private:
        std::deque<Task> _tasks{};
        bool _done{};
        std::mutex _mutex{};
        std::condition_variable _ready{};
    };

    size_t _num_threads{std::thread::hardware_concurrency()};
    std::vector<std::thread> _threads{};
    std::vector<Queue> _queues{_num_threads};
    std::atomic<size_t> _idx{};

    auto launch(Task task) -> void;
    auto run(size_t i) -> void;

    // No copy, no move.
    Task_launcher(const Task_launcher&) = delete;
    auto operator=(const Task_launcher&) = delete;
    Task_launcher(Task_launcher&&) = delete;
    auto operator=(Task_launcher&&) = delete;

};

} // namespace tiny