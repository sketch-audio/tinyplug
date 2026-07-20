#pragma once

#include <atomic>
#include <charconv>
#include <optional>

#include "adapters.hpp"
#include "monolith.hpp"

#include "plug_info.hpp"

#include "processor.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"
#include "editor.hpp"

#include <tiny_dsp/host_bypass.hpp>

namespace tiny::aax {

class Parameters : public AAX_CMonolithicParameters {
public:

    using Super = AAX_CMonolithicParameters;
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
        try_bind_worker(*_processor, Worker_processor_actor{
            [this](const auto& m) { return _worker_from_proc.push(m); }
        });
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

    AAX_Result GetNumberOfChunks(int32_t* oNumChunks) const AAX_OVERRIDE;
	AAX_Result GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const AAX_OVERRIDE;
    AAX_Result GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const AAX_OVERRIDE;
	AAX_Result GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const AAX_OVERRIDE;
	AAX_Result SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk) AAX_OVERRIDE;
	AAX_Result CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const AAX_OVERRIDE;

    void RenderAudio(AAX_SInstrumentRenderInfo* ioRenderInfo, int32_t channelCount, const TParamValPair* inSynchronizedParamValues[], int32_t inNumSynchronizedParamValues) override;

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

    auto push_action(const User_action& action) -> void
    {
        [[maybe_unused]] const auto success = _to_processor.push(action);
        assert(success && "Push to processor queue failed! Increase queue size.");
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

    auto _drain_worker_to_processor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_processor(*_processor, _worker_to_proc);
#endif
    }


    auto _build_chunk() const -> void;

    std::unique_ptr<plugin::Processor> _processor = std::make_unique<plugin::Processor>();
    std::unique_ptr<plugin::Editor> _editor{};
    std::optional<Rect_size> _last_size{};
    Task_manager _tasks{};

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

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

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{1};
    static constexpr auto max_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};
    
    std::array<float, num_meters> _meters{};
    std::array<double, num_meters> _last_meters{};

    static constexpr auto to_processor_size = 4 * num_params + 1;
    using To_processor_queue = Lock_free_queue<User_action, to_processor_size>;
    To_processor_queue _to_processor{}; // In AAX, this is actually only for non-automatable parameters.

    static constexpr auto to_editor_size = 25 * num_meters + 1;
    using Meter_queue = Lock_free_queue<Set_meter, to_editor_size>;
    Meter_queue _meter_queue{};

    // Latency
    // ---
    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);
    Latency_flag _pending_latency{};
    Latency_flag _accepted_latency{};
    // ---

    Host_bypass _bypass{};

    // Notifications
    std::atomic<bool> _delay_comp{true}; // Track Pro Tools delay compensation mode.
    std::atomic<bool> _recording{false}; // Track recording state.
    std::atomic<bool> _offline{false};   // Track offline (bounce) mode.

#if TINY_HAS_WORKER
    // Worker channel.
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

    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif

};

} // namespace tiny::aax