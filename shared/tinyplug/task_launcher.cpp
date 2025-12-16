#include "task_launcher.hpp"

namespace tiny {

Task_launcher::Task_launcher()
{
    for (size_t i = 0; i < _num_threads; ++i) {
        _threads.emplace_back([this, i]() { run(i); });
    }
}

Task_launcher::~Task_launcher()
{
    for (auto& q : _queues) {
        q.done();
    }
    for (auto& t : _threads) {
        if (t.joinable())
            t.join();
    }
}

auto Task_launcher::launch(Task task) -> void
{
    const auto idx = _idx.fetch_add(1, std::memory_order_relaxed);

    static constexpr auto max_tries = 4;
    for (size_t i = 0; i < max_tries * _num_threads; ++i) {
        if (_queues[(idx + i) % _num_threads].try_push(task)) {
            return;
        }
    }

    _queues[idx % _num_threads].push(std::move(task));
}

auto Task_launcher::actor() -> Actor
{
    return Actor{this};
}

// MARK: - Actor

auto Task_launcher::Actor::launch(Task task) const -> void
{
    if (_receiver) {
        _receiver->launch(std::move(task));
    }
}

// MARK: - Private

auto Task_launcher::run(size_t i) -> void
{
    while (true) {
        auto task = Task{};
        for (size_t j = 0; j < _num_threads; ++j) {
            if (_queues[(i + j) % _num_threads].try_pop(task)) {
                break;
            }
        }
        if (!task) {
            if (!_queues[i].pop(task)) break;
        }
        task();
    }
}

} // namespace tiny