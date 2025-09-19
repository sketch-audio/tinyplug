#pragma once

#include <atomic>
#include <charconv>
#include <optional>

#include "aax_adapters.h"
#include "aax_monolith.h"

#include "plug_info.h"

#include "dsp_kernel.h"
#include "param_model.h"
#include "plug_editor.h"

namespace tiny {

class Aax_parameters : public AAX_CMonolithicParameters {
public:

    using Super = AAX_CMonolithicParameters;
    Aax_parameters() : Super() {}
    ~Aax_parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create() { return new Aax_parameters; }

    AAX_Result EffectInit() override;
    AAX_Result NotificationReceived(AAX_CTypeID inNotificationType, const void* inNotificationData, uint32_t inNotificationDataSize) override;

    AAX_Result GetNumberOfChunks(int32_t* oNumChunks) const AAX_OVERRIDE;
	AAX_Result GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const AAX_OVERRIDE;
    AAX_Result GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const AAX_OVERRIDE;
	AAX_Result GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const AAX_OVERRIDE;
	AAX_Result SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk) AAX_OVERRIDE;
	AAX_Result CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const AAX_OVERRIDE;

    void RenderAudio(AAX_SInstrumentRenderInfo* ioRenderInfo, const TParamValPair* inSynchronizedParamValues[], int32_t inNumSynchronizedParamValues) override;

    auto pop_export(Ui_event& event) -> bool
    {
        return _oqueue.pop(event);
    }

    auto dump_exports() -> void
    {
        enumerate<uint32_t>(_lexports, [this](auto i, const auto& e) {
            _oqueue.push(Set_export{i, e});
        });
    }

    auto push_action(const User_action& action) -> void
    {
        _from_ui.push(action);
    }

    auto get_editor() -> std::shared_ptr<Plug_editor>
    {
        return _editor;
    }

private:

    const AAX_CTypeID CHUNK_ID = 'tiny'; // ...
    static constexpr auto tinyplug_tree_version = "tinyplug-tree-version";

    std::shared_ptr<Plug_editor> _editor = std::make_shared<Plug_editor>();

    using User_params = Param_infos<Param_model>;
    using User_exports = Exports<Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{1}; // mono sidechain? verify.
    static constexpr auto num_ochannels = size_t{2};

    using From_ui_queue = Lock_free_queue<User_action, 256>;
    From_ui_queue _from_ui{};

    std::atomic<bool> recording{false}; // 

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_schannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    std::array<double, num_exports> _lexports{};

    User_params _param_infos{};

    using To_ui_queue = Lock_free_queue<Ui_event, 256, Queue_concurrency::mpsc>;
    To_ui_queue _oqueue{};

    std::unique_ptr<Dsp_kernel> _kernel = std::make_unique<Dsp_kernel>();

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    std::atomic<bool> _delay_comp{true}; // Track Pro Tools delay compensation mode.

    auto build_tiny_chunk() const -> void;

};

} // namespace tiny