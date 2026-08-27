#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

#include "messaging.hpp"
#include "view.hpp"

#include "tinyplug/change_list.hpp"
#include "tinyplug/task_manager.hpp"

namespace tiny::vst3 {

class Controller : public Steinberg::Vst::EditControllerEx1 {
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
        });
#if TINY_HAS_WORKER
        _setup_worker();
#endif
    }
    ~Controller() SMTG_OVERRIDE = default;

#if TINY_HAS_WORKER
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;
#endif

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

    //---Interface---------
    DEFINE_INTERFACES
        // Here you can add more supported VST3 interfaces
        // DEF_INTERFACE (Vst::IXXX)
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

    std::optional<plugin::Editor> _editor{};
    Task_manager _tasks{};

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;
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
                .editor_state = _editor ? _editor->save_state() : State_map{}
            };
        },
    }};

    static constexpr auto meter_size = 25 * num_meters + 1;
    using Meter_queue = Lock_free_queue<Set_meter, meter_size>;
    Meter_queue _meter_queue{};
    Change_list<Set_param> _state_queue{}; // Knob space, to the editor.
    std::array<double, num_meters> _last_meters{};

    std::unordered_set<uint32_t> _gestured{};
    std::optional<Rect_size> _last_size{};

    // VST3 delivers a preset as two calls: setComponentState (params + undo step)
    // then setState (editor state). We stash the load here so the editor notify()
    // can fire at the end of setState, with both halves present and the undo step
    // still open for marker folding.
    bool _host_load_pending{};
    std::vector<Set_param> _host_load_changes{};
    std::array<double, num_params> _host_load_after{};

#if TINY_HAS_WORKER
    // Worker channel. The worker lives on the controller side and uses the
    // editor's Task_manager. Editor↔worker is direct in-process; processor↔
    // worker crosses the IPC boundary (shuttle + IMessage in both directions).
    using Worker_from_proc_q = Lock_free_queue<typename User_worker::Model::From_processor, User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_worker::Model::From_editor,    User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_worker::Model::To_processor,   User_worker::Model::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_worker::Model::To_editor,     User_worker::Model::outbound_capacity>;

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

    vst3::Message_router _router{};
    vst3::Message_sender _to_proc{this};

    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};

    auto _setup_worker() -> void;
#endif

    auto _drain_worker_to_editor() -> void;

};

} // namespace tiny::vst3
