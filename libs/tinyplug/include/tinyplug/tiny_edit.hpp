#pragma once

#include <tiny_core/state_adapter.hpp>
#include <tiny_core/task_manager.hpp>

#include "action_queue.hpp"
#include "edit_store.hpp"
#include "tiny_note_io.hpp"
#include "tiny_state_link.hpp"
#include "undo_history.hpp"

namespace tiny {

// We're gonna need to move this but just stuff it in here for now.
enum class Format {
    Aax, Auv2, Auv3, Clap, Vst3, Web
};

struct Edit_context {
    Action_queue::Actor actions{};
    Format format{};
    State_adapter::Actor state_adapter{};
    Undo_history::Actor undo_redo{};
    Task_manager::Actor tasks{};
    Edit_store::Handle view{};
#if TINY_HAS_STATE
    state::Editor_actor<models::Resolved::State> state{};
#endif
#if TINY_HAS_NOTES_IN
    Note_sender notes{};
#endif

    // A preset's parameters and document, from the editor: each persistent parameter as a set, the
    // document as one Overwrite edit. Inside the caller's transaction, beside its own view edits, it
    // undoes as one step, as a host load does.
    auto apply_preset(const nlohmann::ordered_json& preset) const -> void
    {
        const auto values = state_adapter.param_values(preset);
        for (auto addr = std::size_t{}; addr < values.size() && addr < User_params::num_params; ++addr) {
            const auto& spec = User_params::param_spec(static_cast<uint32_t>(addr));
            if (values[addr] && State_rules::is_persistent(spec)) actions.push(Set_param{static_cast<uint32_t>(addr), *values[addr]});
        }
#if TINY_HAS_STATE
        if constexpr (state::writers_of<models::Resolved::State> != state::Writers::Processor) {
            if (const auto record = state_adapter.state_record(preset); !record.empty()) (void)state.load_record(record);
        }
#endif
    }
};

} // namespace tiny

namespace tiny::edit {

// Reserved fallback for a plug-in without `editor.hpp`. Headless is not supported yet.
struct None {};

} // namespace tiny::edit
