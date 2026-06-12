#pragma once

#include <optional>
#include <thread>

#include "serial_queue.hpp"
#include "notification_queue.hpp"
#include "task_launcher.hpp"

#include "lock_free_queue.hpp" // Lock free

namespace tiny {

class Task_manager {
public:

    using Task = Notification_queue::Task;

    Task_manager() = default;

    // Owner responsible for binding to main thread.
    auto bind_main(std::thread::id thread_id) -> void;
    auto is_main_thread() const -> bool;

    // Owner responsible for running the main thread tasks at an appropriate time.
    auto run_main() -> void;

    class Actor {
    public:
        explicit Actor(Task_manager* receiver = nullptr) : _receiver{receiver} {}
        auto is_main_thread() const -> bool;
        auto on_background(Task task) const -> void;
        auto on_main(Task task) const -> void;
        auto on_serial(Task task) const -> void;
    private:
        friend class Task_manager;
        Task_manager* _receiver{nullptr};
    };

    auto actor() -> Actor;

private:

    std::optional<std::thread::id> _main_id{};

    using Main_queue = Lock_free_queue<Task, 16, Queue_concurrency::mpsc>;

    Task_launcher _background{};
    Main_queue _main{};
    Serial_queue _serial{};

    // No copy, no move.
    Task_manager(const Task_manager&) = delete;
    auto operator=(const Task_manager&) = delete;
    Task_manager(Task_manager&&) = delete;
    auto operator=(Task_manager&&) = delete;

};

} // namespace tiny