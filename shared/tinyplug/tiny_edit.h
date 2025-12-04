#pragma once

#include "action_queue.hpp"
#include "task_queue.hpp"
#include "task_launcher.hpp"
#include "undo_history.hpp"

namespace tiny {

struct Edit_context {
    Action_queue::Actor actions{};
    Undo_history::Actor undo_redo{};
};

struct Execution_context {
    Task_launcher::Actor launcher{};
    Task_queue::Actor queue{};
};

} // namespace tiny