#pragma once

#include "action_queue.hpp"
#include "undo_history.hpp"

namespace tiny {

struct Edit_context {
    Action_queue::Actor actions{};
    Undo_history::Actor undo_redo{};
};

} // namespace tiny