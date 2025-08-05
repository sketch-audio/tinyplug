#pragma once

#include <atomic>
#include <charconv>

#include "aax_adapters.h"
#include "aax_monolith.h"

#include "plug_info.h"

#include "user/dsp_kernel.h"
#include "user/param_model.h"

class Aax_parameters : public tiny::AAX_CMonolithicParameters {
public:

    using Super = tiny::AAX_CMonolithicParameters;
    Aax_parameters() : Super() {}
    ~Aax_parameters() override = default;

    static AAX_CEffectParameters* AAX_CALLBACK Create() { return new Aax_parameters; }

    AAX_Result EffectInit() override;
    AAX_Result NotificationReceived(AAX_CTypeID inNotificationType, const void* inNotificationData, uint32_t inNotificationDataSize) override;

    using RenderInfo = tiny::AAX_SInstrumentRenderInfo;
    void RenderAudio(RenderInfo* ioRenderInfo, const TParamValPair* inSynchronizedParamValues[], int32_t inNumSynchronizedParamValues) override
    {
        using namespace tiny;

        // Handle parameter events.
        for (auto i = decltype(inNumSynchronizedParamValues){}; i < inNumSynchronizedParamValues; ++i) {
            const auto* sync_value = inSynchronizedParamValues[i];
            const auto aax_id = sync_value->first;
            const auto aax_param = sync_value->second;

            if (const auto tiny_id = aax_id_to_tiny(aax_id); *tiny_id < User_params::num_params) {
                auto value = double{};
                aax_param->GetValueAsDouble(&value);
                _kernel->handle_event(Set_param{.id = *tiny_id, .value = value});
            }
        }

        // Assign buffer ptrs.
        for (size_t i = 0; i < num_ichannels; ++i) {
            _ibuffers[i] = ioRenderInfo->mAudioInputs[i];
        }
        for (size_t i = 0; i < num_ochannels; ++i) {
            _obuffers[i] = ioRenderInfo->mAudioOutputs[i];
        }
        if constexpr (Plug_info::wants_sidechain) {
            if (const auto sc_idx = ioRenderInfo->mSidechainIndex; sc_idx != nullptr) {
                for (size_t i = 0; i < num_schannels; ++i) {
                    _sbuffers[i] = ioRenderInfo->mAudioInputs[*sc_idx + i];
                }
            }
        }

        // Read host data.
        struct {
            double tempo{};
            int32_t time_sig_numer{};
            int32_t time_sig_denom{};
            bool is_playing{};
            int64_t tick_pos{};
            bool is_looping{};
            int64_t loop_start_tick{};
            int64_t loop_end_tick{};
            int64_t sample_pos{};
            uint32_t ticks_per_beat{};
            bool is_recording{};
        } host_data{};

        auto* transport = ioRenderInfo->mTransportNode->GetTransport();

        // TODO: - Optionals 
        [[maybe_unused]] auto result = AAX_Result{AAX_SUCCESS};
        result = transport->GetCurrentTempo(&host_data.tempo);
        result = transport->GetCurrentMeter(&host_data.time_sig_numer, &host_data.time_sig_denom);
        result = transport->IsTransportPlaying(&host_data.is_playing);
        result = transport->GetCurrentTickPosition(&host_data.tick_pos);
        result = transport->GetCurrentLoopPosition(&host_data.is_looping, &host_data.loop_start_tick, &host_data.loop_end_tick);
        result = transport->GetCurrentNativeSampleLocation(&host_data.sample_pos);
        result = transport->GetCurrentTicksPerBeat(&host_data.ticks_per_beat);

        host_data.is_recording = recording.load(std::memory_order_relaxed); // From notifications.

        const auto sample_pos = host_data.sample_pos;
        const auto beat_pos = static_cast<double>(host_data.tick_pos) / host_data.ticks_per_beat;
        const auto cycle_start = static_cast<double>(host_data.loop_start_tick) / host_data.ticks_per_beat;
        const auto cycle_end = static_cast<double>(host_data.loop_end_tick) / host_data.ticks_per_beat;
        const auto tempo = host_data.tempo;
        const auto ts_numer = host_data.time_sig_numer;
        const auto ts_denom = host_data.time_sig_denom;

        const auto num_frames = static_cast<size_t>(*ioRenderInfo->mNumSamples);

        // Process kernel.
        auto context = tiny::Dsp_context{
            .musical_context = {
                .sample_pos = sample_pos,
                .beat_pos = beat_pos,
                .cycle_start = cycle_start,
                .cycle_end = cycle_end,
                .tempo_ideal = tempo,
                .time_sig = {ts_numer, ts_denom},
                .transport_state = {
                    .moving = host_data.is_playing,
                    .cycling = host_data.is_looping,
                    .recording = host_data.is_recording
                }
            },
            .ibuffers = _ibuffers,
            .sbuffers = _sbuffers,
            .obuffers = _obuffers,
            .num_frames = num_frames,
            .exports = _exports
        };
        _kernel->process(context);

        // Write exports to meters.
        for (size_t i = 0; i < num_exports; ++i) {
            if (context.exports[i] != _lexports[i]) {
                // Send an output event.
                _oqueue.push({
                    .id = static_cast<uint32_t>(i),
                    .value = context.exports[i]
                });

                // Cache for next time.
                _lexports[i] = context.exports[i];
            }

            _exports[i] = 0; // Reset for peak meters.
        }
    }

    auto pop_export(tiny::Export_event& event) -> bool
    {
        return _oqueue.pop(event);
    }

private:

    using User_params = tiny::Param_infos<tiny::Param_model>;
    using User_exports = tiny::Exports<tiny::Param_model>;

    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_exports = User_exports::num_exports;

    static constexpr auto num_ichannels = size_t{2};
    static constexpr auto num_schannels = size_t{1}; // mono sidechain? verify.
    static constexpr auto num_ochannels = size_t{2};

    std::atomic<bool> recording{false}; // 

    // Pointers to host io buffers.
    std::array<const float*, num_ichannels> _ibuffers{};
    std::array<const float*, num_schannels> _sbuffers{};
    std::array<float*, num_ochannels> _obuffers{};
    std::array<float, num_exports> _exports{};

    std::array<double, num_exports> _lexports{};

    User_params _params{};

    using Export_queue = tiny::Lock_free_queue<tiny::Export_event, 256>;
    Export_queue _oqueue{};

    std::unique_ptr<tiny::Dsp_kernel> _kernel = std::make_unique<tiny::Dsp_kernel>();

};