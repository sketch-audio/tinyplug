#pragma once

#include <functional>
#include <thread>
#include <vector>

namespace tiny {

class Task_launcher {
public:

    using Task = std::function<void()>;

    Task_launcher() = default;
    ~Task_launcher(); // Join the threads.

    // An interface to launch tasks on a receiver. Does nothing in case of null receiver!
    class Actor {
    public:
        explicit Actor(Task_launcher* receiver = nullptr) : _receiver{receiver} {}
        auto launch(Task task) const -> void;
    private:
        friend class Task_launcher;
        Task_launcher* _receiver{nullptr};
    };

    // Get a view to launch tasks.
    auto actor() -> Actor;

private:
    // So we can join.
    std::vector<std::thread> _threads{};

    // Execute the task in a new thread. Joins on destruction.
    auto launch(Task task) -> void;    

    // No copy, no move.
    Task_launcher(const Task_launcher&) = delete;
    auto operator=(const Task_launcher&) = delete;
    Task_launcher(Task_launcher&&) = delete;
    auto operator=(Task_launcher&&) = delete;

};

} // namespace tiny