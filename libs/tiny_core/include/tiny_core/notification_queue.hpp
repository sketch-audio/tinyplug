#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

namespace tiny {

// See: https://youtu.be/zULU6Hhp42w
class Notification_queue {
public:

    using Task = std::function<void()>;

    // Obtains lock, pushes a task, and notifies condition variable.
    auto push(Task task) -> void;

    // Tries lock. If successful, pushes a task and notifies condition variable. Returns success.
    auto try_push(Task task) -> bool;

    // Waits on condition variable! Returns false if done and no tasks remain.
    auto pop(Task& task) -> bool;

    // Doesn't wait on condition variable (tries lock).
    auto try_pop(Task& task) -> bool;

    // Call done before joining your threads!
    auto done() -> void;

private:
    
    std::deque<Task> _tasks{};
    bool _done{};
    std::mutex _mutex{};
    std::condition_variable _ready{};

};

} // namespace tiny