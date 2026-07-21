#pragma once

#include <array>
#include <atomic>
#include <charconv>
#include <optional>

#include "AAX_CEffectParameters.h"

#include "adapters.hpp"
#include "alg_context.hpp"

#include "plug_info.hpp"

#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

namespace tiny::aax {

/*
    The data model half of the two-component design.

    It owns everything that is not audio: the AAX parameter objects, the editor, the
    worker, undo history, and state chunks. It has no processor and no audio buffers —
    the DSP kernel lives in the algorithm's private data and is reachable only through
    coefficient packets out and the Direct Data return channel in.
*/
class Parameters : public AAX_CEffectParameters {
public:

    using Super = AAX_CEffectParameters;
    Parameters() : Super{}
    {
        _editor = std::make_unique<plugin::Editor>(Edit_context{
            .actions = _actions.actor(),
            .format = Format::Aax,
            .state_adapter = _state_adapter.actor(),
            .undo_redo = _undo_history.actor(),
            .tasks = _tasks.actor(),
        });

#if TINY_HAS_WORKER
        try_bind_worker(*_editor, Worker_editor_actor{
            [this](const auto& m) { return _worker_from_edit.push(m); }
        });
#endif
    }
    ~Parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create()
    {
        return new Parameters;
    }

    AAX_Result EffectInit() override;
    AAX_Result NotificationReceived(AAX_CTypeID inNotificationType, const void* inNotificationData, uint32_t inNotificationDataSize) override;

    // Packet generation. The host calls GenerateCoefficients after every parameter
    // update, and timestamps every packet posted within it — which is what gives the
    // decoupled design its automation accuracy.
    AAX_Result UpdateParameterNormalizedValue(AAX_CParamID iParamID, double aValue, AAX_EUpdateSource inSource) override;
    AAX_Result GenerateCoefficients() override;
    AAX_Result TimerWakeup() override;

    // The Direct Data module's channel to us. Nothing here dereferences algorithm
    // memory; both directions carry framed bytes.
    AAX_Result SetCustomData(AAX_CTypeID iDataBlockID, uint32_t inDataSize, const void* iData) override;
    AAX_Result GetCustomData(AAX_CTypeID iDataBlockID, uint32_t inDataSize, void* oData, uint32_t* oDataWritten) const override;

    AAX_Result GetNumberOfChunks(int32_t* oNumChunks) const AAX_OVERRIDE;
	AAX_Result GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const AAX_OVERRIDE;
    AAX_Result GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const AAX_OVERRIDE;
	AAX_Result GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const AAX_OVERRIDE;
	AAX_Result SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk) AAX_OVERRIDE;
	AAX_Result CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const AAX_OVERRIDE;

    auto pop_meter(Set_meter& set_meter) -> bool
    {
        return _meter_queue.pop(set_meter);
    }

    auto dump_meters() -> void
    {
        enumerate<uint32_t>(_last_meters, [this](auto i, const auto& e) {
            _meter_queue.push(Set_meter{i, e});
        });
    }

    auto get_editor() -> plugin::Editor*
    {
        return _editor.get();
    }

    auto get_tasks() -> Task_manager*
    {
        return &_tasks;
    }

    // Framework-owned editor window-size cache (Parameters lifetime, survives view
    // recreation). Primed from the persisted chunk in SetChunk; read by the Gui to open
    // pre-sized; updated on every editor-initiated resize.
    auto resized(Rect_size size) -> void
    {
        _last_size = size;
    }

    auto get_last_size() const -> std::optional<Rect_size>
    {
        return _last_size;
    }

    // Undo history and action queue live on Parameters (plug-in lifetime), not in the
    // Gui, so the editor's Edit_context (built once at construction) stays valid across
    // window open/close and host preset loads are captured with the window closed.
    auto undo_history() -> Undo_history*
    {
        return &_undo_history;
    }

    auto actions() -> Action_queue*
    {
        return &_actions;
    }

    auto state_adapter() -> State_adapter*
    {
        return &_state_adapter;
    }

    auto drain_worker_to_editor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
    }

private:

    auto _build_chunk() const -> void;

    // Coefficient staging. One Coef_segment per port; a segment is rebuilt and posted
    // only when one of the parameters packed into it has changed.
    auto _mark_dirty(uint32_t address) -> void;
    auto _fill_segment(size_t segment) -> void;
    auto _post_segment(size_t segment) -> void;
    auto _post_config() -> void;
    auto _post_runtime() -> void;
    auto _plain_value(uint32_t address) const -> double;

    std::unique_ptr<plugin::Editor> _editor{};
    std::optional<Rect_size> _last_size{};
    Task_manager _tasks{};

    Undo_history _undo_history{};
    Action_queue _actions{};

    // Snapshot all current param values in knob space (AAX normalized == knob).
    // Defined in the .cpp where the AAX parameter manager / adapters are visible.
    auto _snapshot_knob_params() -> std::array<double, num_params>;

    // State adapter lives on Parameters too (the editor's Edit_context references it
    // for life). save_model reads the current params via _snapshot_knob_params.
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

    // Packets out.
    std::array<Coef_segment, num_segments> _segments{};
    std::array<bool, num_segments> _segment_dirty{};
    uint64_t _seq{1};                   // 0 is the algorithm's "never seen" sentinel.
    Config_packet _config{};
    Runtime_packet _runtime{.latency_seq = 0, .accepted_latency = 0, .offline = 0, .recording = 0, .delay_comp = 1, .pad = 0};
    bool _config_dirty{true};
    std::atomic<bool> _runtime_dirty{true};

    // Meters in. Mirrors the last value seen per meter so the Gui can be re-primed
    // when a window opens (dump_meters).
    std::array<double, num_meters> _last_meters{};

    static constexpr auto to_editor_size = 25 * num_meters + 1;
    using Meter_queue = Lock_free_queue<Set_meter, to_editor_size>;
    Meter_queue _meter_queue{};

    // Latency. The kernel proposes from the algorithm; the host owns the accepted
    // value and hands it back through a notification.
    std::atomic<bool> _pending_latency{false};

#if TINY_HAS_WORKER
    // Worker channel. Editor <-> worker is direct (both live here). Processor <-> worker
    // traverses the Direct Data rings.
    using Worker_from_proc_q = Lock_free_queue<typename User_worker::Model::From_processor, User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_worker::Model::From_editor,    User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_worker::Model::To_processor,   User_worker::Model::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_worker::Model::To_editor,     User_worker::Model::outbound_capacity>;

    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    mutable Worker_to_proc_q _worker_to_proc{};   // Drained from the const GetCustomData.
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
#endif

};

} // namespace tiny::aax
