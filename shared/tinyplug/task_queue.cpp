#include "task_queue.hpp"

#include <cassert>

namespace tiny {

auto Task_queue::execute_all() -> void
{
    auto task = Task{};
    while (_queue.pop(task)) task();
}

auto Task_queue::actor() -> Task_queue::Actor
{
    return Actor{this};
}

// MARK: - Actor

auto Task_queue::Actor::push(Task task) const -> void
{
    if (_receiver) {
        _receiver->push(std::move(task));
    }
}

// MARK: - Private

auto Task_queue::push(Task task) -> void
{
    [[maybe_unused]] const auto success = _queue.push(std::move(task));
    assert(success && "Queue not big enough!");
}

} // namespace tiny