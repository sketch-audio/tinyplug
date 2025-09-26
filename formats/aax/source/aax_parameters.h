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

namespace tiny {

class Aax_parameters : public AAX_CMonolithicParameters {
public:

    using Super = AAX_CMonolithicParameters;
    Aax_parameters() : Super() {}
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

    auto pop_export(Ui_event& event) -> bool
    {
        return _to_editor.pop(event);
    }

    auto dump_exports() -> void
    {
        enumerate<uint32_t>(_lexports, [this](auto i, const auto& e) {
            _to_editor.push(Set_export{i, e});
        });
    }

    auto push_action(const User_action& action) -> void
    {
        [[maybe_unused]] const auto success = _to_processor.push(action);
        assert(success && "Push to processor queue failed! Increase queue size.");
    }

    auto get_editor() -> std::shared_ptr<Plug_editor>
    {
        return _editor;
    }

private:

    auto _build_chunk() const -> void;

    // Chunk
    static constexpr auto tinyplug_chunk_id = AAX_CTypeID{'tiny'};
    static constexpr auto tinyplug_num_params = "tinyplug-num-params";
    static constexpr auto tinyplug_edit_keys = "tinyplug-edit-keys";

    std::shared_ptr<Plug_editor> _editor = std::make_shared<Plug_editor>();
    std::unique_ptr<Plug_processor> _processor = std::make_unique<Plug_processor>();

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    User_params _param_infos{};

    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{1};
    static constexpr auto max_ochannels = size_t{2};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};
    
    std::array<float, num_meters> _exports{};
    std::array<double, num_meters> _lexports{};

    static constexpr auto to_processor_size = 2 * num_params + 1;
    using To_processor_queue = Lock_free_queue<User_action, to_processor_size>;
    To_processor_queue _to_processor{}; // In AAX, this is actually only for non-automatable parameters.

    static constexpr auto to_editor_size = num_params + num_meters + 1;
    using To_editor_queue = Overwrite_queue<Ui_event, to_editor_size>;
    To_editor_queue _to_editor{};

    // Latency
    // ---
    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);
    Latency_flag _pending_latency{};
    Latency_flag _accepted_latency{};
    // ---

    // Notifications
    std::atomic<bool> _delay_comp{true}; // Track Pro Tools delay compensation mode.
    std::atomic<bool> _recording{false}; // Track recording state.

};

} // namespace tiny