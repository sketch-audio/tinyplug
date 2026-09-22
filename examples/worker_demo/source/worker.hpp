#pragma once

#include <algorithm>

#include <tinyplug/tinyplug.hpp>

namespace tiny::work {

// Replies to each channel of `models::Work` (see models/work.hpp).
class Worker {
public:

    using From_processor = User_work::From_processor;
    using From_editor    = User_work::From_editor;

    explicit Worker(Worker_replies reply, Task_manager::Actor tasks)
        : _reply{reply}, _tasks{tasks} {}

    auto on_start(double /*sample_rate*/) -> void {}
    auto on_stop() -> void {}

    auto handle_from_processor(const From_processor& m) -> void
    {
        std::visit(Inline_visitor{
            [this](const models::Tick&) {
                ++_count;
                _reply.to_processor(models::Set_counter{.count = _count});
            }
        }, m);
    }

    auto handle_from_editor(const From_editor& m) -> void
    {
        std::visit(Inline_visitor{
            [this](const models::Set_session& s) {
                auto path = models::Session_path{};
                const auto* src = s.uuid.data();
                std::copy_n(src, std::min(s.uuid.size(), path.path.size()), path.path.begin());
                _reply.to_editor(path);
            }
        }, m);
    }

private:

    Worker_replies _reply{};
    Task_manager::Actor _tasks{};
    uint64_t _count{};

};

} // namespace tiny::work
