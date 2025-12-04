#include "task_launcher.hpp"

namespace tiny {

Task_launcher::~Task_launcher()
{
    for (auto& t : _threads) {
        if (t.joinable())
            t.join();
    }
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

auto Task_launcher::launch(Task task) -> void
{
    _threads.emplace_back([t = std::move(task)]() {
        t();
    });
}

} // namespace tiny