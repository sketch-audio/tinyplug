#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstphysicalui.h"


#include <tiny_plugin.hpp>

#include "messaging.hpp"
#include "view.hpp"

#include "tiny_core/change_set.hpp"
#include "tiny_core/task_manager.hpp"

namespace tiny::vst3 {

class Controller : public Steinberg::Vst::EditControllerEx1
#if TINY_HAS_NOTES_IN
    , public Steinberg::Vst::IMidiMapping
#endif
#if TINY_HAS_NOTE_EXPRESSION
    , public Steinberg::Vst::INoteExpressionController
    , public Steinberg::Vst::INoteExpressionPhysicalUIMapping
#endif
{
public:

    using Super = Steinberg::Vst::EditControllerEx1;
    Controller() : Super{}
    {
        _editor.emplace(Edit_context{
            .actions = _actions.actor(),
            .format = Format::Vst3,
            .state_adapter = _state_adapter.actor(),
            .undo_redo = _undo_history.actor(),
            .tasks = _tasks.actor(),
#if TINY_HAS_STATE
            .state = _state_link.actor(),
#endif
#if TINY_HAS_NOTES_IN
            // To the processor over IMessage: the controller holds no processor.
            .notes = Note_sender{[this](const midi::Performance& e) { return _to_proc.send_pod(k_notes_id, e); }},
#endif
        });
        _setup_router();
#if TINY_HAS_STATE
        _setup_state();
#endif
#if TINY_HAS_WORKER
        _setup_worker();
#endif
    }
    ~Controller() SMTG_OVERRIDE { _tasks.shutdown(); } // First: no task may outlive the editor or worker it captures.

    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;

    // Create function
    static Steinberg::FUnknown* createInstance(void* /*context*/)
    {
        return (Steinberg::Vst::IEditController*)new Controller;
    }

    // IPluginBase
    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API terminate() SMTG_OVERRIDE;

    // IEditController
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getParamStringByValue(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized, Steinberg::Vst::String128 string) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getParamValueByString(Steinberg::Vst::ParamID tag, Steinberg::Vst::TChar* string, Steinberg::Vst::ParamValue& valueNormalized) SMTG_OVERRIDE;
    Steinberg::Vst::ParamValue PLUGIN_API normalizedParamToPlain(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue valueNormalized) SMTG_OVERRIDE;
    Steinberg::Vst::ParamValue PLUGIN_API plainParamToNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue plainValue) SMTG_OVERRIDE;
    Steinberg::Vst::ParamValue PLUGIN_API getParamNormalized(Steinberg::Vst::ParamID tag) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setParamNormalized(Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API setComponentHandler(Steinberg::Vst::IComponentHandler* handler) SMTG_OVERRIDE;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) SMTG_OVERRIDE;

#if TINY_HAS_NOTES_IN
    // IMidiMapping: the only way VST3 delivers bend, pressure and pedals.
    Steinberg::tresult PLUGIN_API getMidiControllerAssignment(Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::CtrlNumber midiControllerNumber, Steinberg::Vst::ParamID& id) SMTG_OVERRIDE;
#endif

#if TINY_HAS_NOTE_EXPRESSION
    // INoteExpressionController and the physical mapping: what hosts read to send MPE as expressions.
    Steinberg::int32 PLUGIN_API getNoteExpressionCount(Steinberg::int32 busIndex, Steinberg::int16 channel) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getNoteExpressionInfo(Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::int32 noteExpressionIndex, Steinberg::Vst::NoteExpressionTypeInfo& info) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getNoteExpressionStringByValue(Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::NoteExpressionTypeID id, Steinberg::Vst::NoteExpressionValue valueNormalized, Steinberg::Vst::String128 string) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getNoteExpressionValueByString(Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::NoteExpressionTypeID id, const Steinberg::Vst::TChar* string, Steinberg::Vst::NoteExpressionValue& valueNormalized) SMTG_OVERRIDE;
    Steinberg::tresult PLUGIN_API getPhysicalUIMapping(Steinberg::int32 busIndex, Steinberg::int16 channel,
        Steinberg::Vst::PhysicalUIMapList& list) SMTG_OVERRIDE;
#endif

    //---Interface---------
    DEFINE_INTERFACES
#if TINY_HAS_NOTES_IN
        DEF_INTERFACE(Steinberg::Vst::IMidiMapping)
#endif
#if TINY_HAS_NOTE_EXPRESSION
        DEF_INTERFACE(Steinberg::Vst::INoteExpressionController)
        DEF_INTERFACE(Steinberg::Vst::INoteExpressionPhysicalUIMapping)
#endif
    END_DEFINE_INTERFACES(Steinberg::Vst::EditControllerEx1)
    DELEGATE_REFCOUNT(Steinberg::Vst::EditControllerEx1)

    auto resized(Rect_size size) -> void
    {
        _last_size = size;
    }

    auto get_last_size() const -> std::optional<Rect_size>
    {
        return _last_size;
    }

    template<typename F>
    auto consume_changes(F&& f) -> void
    {
        _state_queue.consume(std::forward<F>(f));
    }

protected:

    std::optional<User_editor> _editor{};
    Task_manager _tasks{};

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    // Undo history and action queue live on the controller (plug-in lifetime), not in
    // the view, so the editor's Edit_context (built once at construction) stays valid
    // across window open/close and host preset loads are captured with the window closed.
    Undo_history _undo_history{};
    Action_queue _actions{};

    // Snapshot all current param values in knob space. VST3 normalized values are
    // already knob space (0…1), so this mirrors the view's get_param.
    auto _snapshot_knob_params() -> std::array<double, num_params>
    {
        auto out = std::array<double, num_params>{};
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            out[i] = getParamNormalized(i);
        }
        return out;
    }

    // State adapter lives on the controller too (the editor's Edit_context references
    // it for life). save_model reads the controller's current normalized params.
    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params
            };
        },
        .save_model = [this]() {
            const auto knob = _snapshot_knob_params();
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(knob.begin(), knob.end()),
                .editor_state = _editor ? _editor->save_state() : State_map{},
#if TINY_HAS_STATE
                .state_record = state::encode_record(_state_link.view()),
#endif
            };
        },
    }};

#if TINY_HAS_METERS
    // The host owns the meter wire (output parameters), and delivers on the UI thread.
    meters::Mailbox<tiny::models::Resolved::Meters, meters::Transport::Host> _mailbox{};
#endif
#if TINY_HAS_BLOCKS
    // Lock-free: `notify` may run on whatever thread the processor's relay sent from.
    blocks::Mailbox<models::Resolved::Blocks> _block_mailbox{};
#endif
    Change_set<Set_param, User_params::num_params> _state_queue{}; // Knob space, to the editor.

    std::unordered_set<uint32_t> _gestured{};
    std::optional<Rect_size> _last_size{};

    // VST3 delivers a preset as two calls: setComponentState (params + undo step)
    // then setState (editor state). We stash the load here so the editor notify()
    // can fire at the end of setState, with both halves present and the undo step
    // still open for marker folding.
    bool _host_load_pending{};
    std::vector<Set_param> _host_load_changes{};
    std::array<double, num_params> _host_load_after{};

    // Peer link. Not worker-gated: the processor's latency notification arrives through
    // the router, so a plug-in with no worker still needs one.
    vst3::Message_router _router{};
    vst3::Message_sender _to_proc{this};

    auto _setup_router() -> void;

#if TINY_HAS_WORKER
    // Worker channel. The worker lives on the controller side and uses the
    // editor's Task_manager. Editor↔worker is direct in-process; processor↔
    // worker crosses the IPC boundary (shuttle + IMessage in both directions).
    using Worker_from_proc_q = Lock_free_queue<typename User_work::From_processor, User_work::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_work::From_editor,    User_work::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_work::To_processor,   User_work::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_work::To_editor,     User_work::outbound_capacity>;

    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    Worker_to_proc_q   _worker_to_proc{};
    Worker_to_edit_q   _worker_to_edit{};

    User_worker _worker{
        Worker_replies{
            [this](const auto& m) { return _worker_to_proc.push(m); },
            [this](const auto& m) { return _worker_to_edit.push(m); }
        },
        _tasks.actor()
    };

    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};

    auto _setup_worker() -> void;
#endif

#if TINY_HAS_STATE
    // The editor's half of the document. Edits go down over IMessage from the UI thread;
    // snapshots arrive on whatever thread the host delivers `notify` on, so they wait in a
    // port and `sync` takes them on the UI thread.
    state::Editor_link<models::Resolved::State> _state_link{};
    state::Snapshot_inbox<models::Resolved::State> _state_inbox{};

    auto _setup_state() -> void;
#endif

    auto _drain_worker_to_editor() -> void;

};

} // namespace tiny::vst3
