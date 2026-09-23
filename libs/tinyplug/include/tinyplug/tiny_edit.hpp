#pragma once

#include <tiny_core/state_adapter.hpp>
#include <tiny_core/task_manager.hpp>

#include "action_queue.hpp"
#include "tiny_note_io.hpp"
#include "tiny_state_link.hpp"
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
#if TINY_HAS_STATE
    state::Editor_actor<models::Resolved::State> state{};
#endif
#if TINY_HAS_NOTES_IN
    Note_sender notes{};
#endif
};

} // namespace tiny

namespace tiny::edit {

// Reserved fallback for a plug-in without `editor.hpp`. Headless is not supported yet.
struct None {};

} // namespace tiny::edit
