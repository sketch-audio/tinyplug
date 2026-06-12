#pragma once

#include <thread>

#include "notification_queue.hpp"

namespace tiny {

class Serial_queue {
public:

    using Task = Notification_queue::Task;

    Serial_queue() = default;
    ~Serial_queue();

    auto push(Task task) -> void;

    class Actor {
    public:
        explicit Actor(Serial_queue* receiver = nullptr) : _receiver{receiver} {}
        auto push(Task task) const -> void;
    private:
        friend class Serial_queue;
        Serial_queue* _receiver{nullptr};
    };

    auto actor() -> Actor;

private:

    Notification_queue _queue{};
    std::thread _thread{[this]() { run(); }};

    auto run() -> void;

    // No copy, no move.
    Serial_queue(const Serial_queue&) = delete;
    auto operator=(const Serial_queue&) = delete;
    Serial_queue(Serial_queue&&) = delete;
    auto operator=(Serial_queue&&) = delete;

};

} // namespace tiny