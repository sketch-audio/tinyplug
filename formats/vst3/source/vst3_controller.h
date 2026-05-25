#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"

#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"

#include "vst3_messaging.h"
#include "vst3_view.h"

#include "tinyplug/change_list.hpp"
#include "tinyplug/task_manager.hpp"

namespace tiny {

class Vst3_controller : public Steinberg::Vst::EditControllerEx1 {
public:

    using Super = Steinberg::Vst::EditControllerEx1;
    Vst3_controller() : Super{}
    {
        _editor.emplace(_tasks.actor());
#if TINY_HAS_WORKER
        _setup_worker();
#endif
    }
    ~Vst3_controller() SMTG_OVERRIDE = default;

#if TINY_HAS_WORKER
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) SMTG_OVERRIDE;
#endif

    // Create function
    static Steinberg::FUnknown* createInstance(void* /*context*/)
    {
        return (Steinberg::Vst::IEditController*)new Vst3_controller;
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

    std::optional<Plug_editor> _editor{};
    Task_manager _tasks{};

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    static constexpr auto meter_size = 25 * num_meters + 1;
    using Meter_queue = Lock_free_queue<Set_meter, meter_size>;
    Meter_queue _meter_queue{};
    Change_list _state_queue{};
    std::array<double, num_meters> _last_meters{};

    std::unordered_set<uint32_t> _gestured{};
    std::optional<Rect_size> _last_size{};

#if TINY_HAS_WORKER
    // Worker channel. The worker lives on the controller side and uses the
    // editor's Task_manager. Editor↔worker is direct in-process; processor↔
    // worker crosses the IPC boundary (shuttle + IMessage in both directions).
    using Worker_from_proc_q = Lock_free_queue<typename User_worker::From_processor, User_worker::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_worker::From_editor,    User_worker::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_worker::To_processor,   User_worker::reply_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_worker::To_editor,     User_worker::reply_capacity>;

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

} // namespace tiny
