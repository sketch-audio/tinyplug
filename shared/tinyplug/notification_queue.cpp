#include "notification_queue.hpp"

namespace tiny {

auto Notification_queue::push(Task task) -> void
{
    {
        const auto lock = std::unique_lock{_mutex};
        _tasks.emplace_back(std::move(task));
    }
    _ready.notify_one();
}

auto Notification_queue::try_push(Task task) -> bool
{
    {
        const auto lock = std::unique_lock{_mutex,  std::try_to_lock};
        if (!lock) return false;
        _tasks.emplace_back(std::move(task));
    }
    _ready.notify_one();
    return true;
}

auto Notification_queue::pop(Task& task) -> bool
{
    auto lock = std::unique_lock{_mutex};
    while (_tasks.empty() && !_done) {
        _ready.wait(lock);
    }
    if (_tasks.empty()) return false;
    task = std::move(_tasks.front());
    _tasks.pop_front();
    return true;
}

auto Notification_queue::try_pop(Task& task) -> bool
{
    const auto lock = std::unique_lock{_mutex, std::try_to_lock};
    if (!lock || _tasks.empty()) return false;
    task = std::move(_tasks.front());
    _tasks.pop_front();
    return true;
}

auto Notification_queue::done() -> void
{
    {
        const auto lock = std::unique_lock{_mutex};
        _done = true;
    }
    _ready.notify_all();
}

} // namespace tiny