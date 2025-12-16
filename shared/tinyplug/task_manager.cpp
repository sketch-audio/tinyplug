#include "task_manager.hpp"

#include <cassert>

namespace tiny {

auto Task_manager::bind_main(std::thread::id thread_id) -> void
{
    if (!_main_id) {
        _main_id = thread_id;
    }
}

auto Task_manager::is_main_thread() const -> bool
{
    return _main_id && (std::this_thread::get_id() == *_main_id);
}

auto Task_manager::run_main() -> void
{
    assert(is_main_thread() && "run_main must be called from the main thread!");
    auto task = Task{};
    while (_main.pop(task)) task();
}

auto Task_manager::actor() -> Actor
{
    return Actor{this};
}

// MARK: - Actor

auto Task_manager::Actor::is_main_thread() const -> bool
{
    return _receiver && _receiver->is_main_thread();
}

auto Task_manager::Actor::on_background(Task task) const -> void
{
    if (_receiver) {
        _receiver->_background.launch(std::move(task));
    }
}

auto Task_manager::Actor::on_main(Task task) const -> void
{
    if (_receiver) {
        [[maybe_unused]] const auto success = _receiver->_main.push(std::move(task));
        assert(success && "Main queue not big enough!");
    }
}

auto Task_manager::Actor::on_serial(Task task) const -> void
{
    if (_receiver) {
        _receiver->_serial.push(std::move(task));
    }
}

} // namespace tiny