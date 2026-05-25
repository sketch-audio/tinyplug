#pragma once

#include <atomic>
#include <charconv>
#include <optional>

#include "aax_adapters.h"
#include "aax_monolith.h"

#include "plug_info.h"

#include "plug_processor.h"
#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_editor.h"

#include "dsp/host_bypass.hpp"

namespace tiny {

class Aax_parameters : public AAX_CMonolithicParameters {
public:

    using Super = AAX_CMonolithicParameters;
    Aax_parameters() : Super{}
    {
        _editor = std::make_unique<Plug_editor>(_tasks.actor());

#if TINY_HAS_WORKER
        try_bind_worker(*_processor, Worker_processor_actor{
            [this](const auto& m) { return _worker_from_proc.push(m); }
        });
        try_bind_worker(*_editor, Worker_editor_actor{
            [this](const auto& m) { return _worker_from_edit.push(m); }
        });
#endif
    }
    ~Aax_parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create()
    {
        return new Aax_parameters;
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

    auto get_editor() -> Plug_editor* 
    {
        return _editor.get();
    }

    auto get_tasks() -> Task_manager*
    {
        return &_tasks;
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

    std::unique_ptr<Plug_processor> _processor = std::make_unique<Plug_processor>();
    std::unique_ptr<Plug_editor> _editor{};
    Task_manager _tasks{};

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

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

#if TINY_HAS_WORKER
    // Worker channel.
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

    // Last so its destructor (which joins the worker thread) runs first.
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif

};

} // namespace tiny