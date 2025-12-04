#pragma once

#include <functional>
#include <optional>

#include "tiny_queue.h"

namespace tiny {

class Task_queue {
public:

    using Task = std::function<void()>;

    // An interface to push tasks to a receiver. Does nothing in case of null receiver!
    class Actor {
    public:
        explicit Actor(Task_queue* receiver = nullptr) : _receiver{receiver} {}
        auto push(Task task) const -> void;
    private:
        friend class Task_queue;
        Task_queue* _receiver{nullptr};
    };

    // Execute all tasks in the queue.
    auto execute_all() -> void;

    // Get a view to push tasks.
    auto actor() -> Actor;

private:

    using Queue = Lock_free_queue<Task, 16, Queue_concurrency::mpsc>;
    Queue _queue{};

    auto push(Task task) -> void;

};

// A helper to easily push a lambda with some args into a Task_queue.
template<typename... Args>
struct Later {

    std::function<void(Args...)> callback{[](Args...) {}};
    std::optional<Task_queue::Actor> queue{};

    template<typename... Cargs>
    void operator()(Cargs&&... args) const {
        if (queue) {
            queue->push([cb = callback, ...a = std::forward<Cargs>(args)]() {
                cb(std::move(a)...);
            });
        } else {
            callback(std::forward<Cargs>(args)...);
        }
    }
};

} // namespace tiny