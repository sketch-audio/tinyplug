#pragma once

#include <functional>
#include <span>

#include <tiny_core/state_adapter.hpp>
#include <tiny_core/task_manager.hpp>

#include "action_queue.hpp"
#include "edit_store.hpp"
#include "edit_types.hpp"
#include "tiny_note_io.hpp"
#include "tiny_state_link.hpp"
#include "undo_history.hpp"

namespace tiny {

// We're gonna need to move this but just stuff it in here for now.
enum class Format {
    Aax, Auv2, Auv3, Clap, Vst3, Web
};

} // namespace tiny

namespace tiny::edit {

// What the editor is handed: one handle per piece of framework-owned state, which `undo_redo` joins into steps
// (a transaction makes everything inside it one step, whichever handle it went through).
struct Context {
    Action_queue::Actor params{};          // parameters: gestures and sets, knob space
    Format format{};
    State_adapter::Actor state_adapter{};
    Undo_history::Actor undo_redo{};
    Task_manager::Actor tasks{};
    Edit_store::Handle view{};             // the view model (`models/view.hpp`)
    Edit_store::Size size{};               // the window
#if TINY_HAS_STATE
    state::Editor_actor<models::Resolved::State> state{}; // the document
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
            const auto& spec = User_params::param_spec(static_cast<std::uint32_t>(addr));
            if (values[addr] && State_rules::is_persistent(spec)) params.set(static_cast<std::uint32_t>(addr), *values[addr]);
        }
#if TINY_HAS_STATE
        if constexpr (state::writers_of<models::Resolved::State> != state::Writers::Processor) {
            if (const auto record = state_adapter.state_record(preset); !record.empty()) (void)state.load_record(record);
        }
#endif
    }
};

// How the view loop reaches its wrapper: values in, actions out, and the frame's edges.
struct Connection {
    std::function<double(std::uint32_t)> get_param = [](auto) { return 0.; };
#if TINY_HAS_METERS
    // Fills one display value per meter address.
    std::function<void(std::span<float>)> read_meters = [](auto) {};
#endif
#if TINY_HAS_BLOCKS
    // Refreshes the editor's retained frames, marking this draw's arrivals fresh.
    std::function<void(blocks::Frames<models::Resolved::Blocks>&)> read_blocks = [](auto&) {};
#endif
    // Each resolved action, normalized: wrappers see only addressed starts and ends.
    std::function<void(const Action::Any&)> action_handler = [](auto&) {};
#if TINY_HAS_STATE
    // Once before and once after each draw: take in snapshots, send edits, close a waiting undo step.
    std::function<void()> sync_state = []() {};
#endif
    // At the end of each frame, after its actions resolved: the wrapper's `Edit_store::end_frame`.
    std::function<void()> end_frame = []() {};
};

// Reserved fallback for a plug-in without `editor.hpp`. Headless is not supported yet.
struct None {};

} // namespace tiny::edit
