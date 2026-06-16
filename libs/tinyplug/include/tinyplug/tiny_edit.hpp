#pragma once

#include "action_queue.hpp"
#include "state_adapter.hpp"
#include "task_manager.hpp"
#include "undo_history.hpp"

namespace tiny {

// We're gonna need to move this but just stuff it in here for now.
enum class Format {
    Aax, Auv2, Auv3, Clap, Vst3
};

struct Edit_context {
    Action_queue::Actor actions{};
    Format format{};
    State_adapter::Actor state_adapter{};
    Undo_history::Actor undo_redo{};
    Task_manager::Actor tasks{};
};

} // namespace tiny