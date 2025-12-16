#include "serial_queue.hpp"

namespace tiny {

Serial_queue::~Serial_queue()
{
    _queue.done();
    if (_thread.joinable()) {
        _thread.join();
    }
}

auto Serial_queue::push(Task task) -> void
{
    _queue.push(std::move(task));
}

auto Serial_queue::actor() -> Actor
{
    return Actor{this};
}

// MARK: - Actor

auto Serial_queue::Actor::push(Task task) const -> void
{
    if (_receiver) {
        _receiver->push(std::move(task));
    }
}

// MARK: - Private

auto Serial_queue::run() -> void
{
    while (true) {
        auto task = Task{};
        if (!_queue.pop(task)) break;
        task();
    }
}

} // namespace tiny