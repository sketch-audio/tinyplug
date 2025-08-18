#include "aax_parameters.h"

#include <cstring>

#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CStateTaperDelegate.h"
#include "AAX_CStateDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"
#include "AAX_TransportTypes.h"

#include "aax_adapters.h"
#include "aax_taper_delegate.h"

namespace tiny {

// MARK: - EffectInit

AAX_Result Aax_parameters::EffectInit()
{
    const auto& params = _param_infos.presentation_specs();
    const auto aax_ids = tree_to_aax_ids(_param_infos.tree());
    TINY_ASSERT(params.size() == aax_ids.size(), "AAX IDs must have same size as param specs.");

    for (size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto& aax_id = aax_ids[i];

        std::visit(Inline_visitor{
            [&](const Bool_semantics& b) {
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    b.def_val,
                    AAX_CBinaryTaperDelegate<bool>(),
                    AAX_CBinaryDisplayDelegate<bool>("False", "True"),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(2);
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const List_semantics& l) {
                const auto num_items = l.labels.size();
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<uint32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    static_cast<uint32_t>(l.def_val),
                    AAX_CStateTaperDelegate<uint32_t>(0, num_items - 1),
                    AAX_CStateDisplayDelegate<uint32_t>(num_items, const_cast<const char**>(l.labels.data()), 0), // Yee haw.
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(num_items);
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const Float_semantics& f) {
                using TaperDelegate = Float_semanticsTaperDelegate<double>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 1, space after: 1
                const auto units_str = units_string(f.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    f.def_val,
                    TaperDelegate(f), // So we can use our own control adapter.
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(2048); // Is this the most we can have?
                aax_param->SetType(AAX_eParameterType_Continuous);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const Int_semantics& i) {
                using DisplayDelegate = AAX_CNumberDisplayDelegate<int32_t, 0, 1>; // precision: 0, space after: 1
                const auto units_str = units_string(i.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<int32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    i.def_val,
                    AAX_CStateTaperDelegate<int32_t>(i.min_val, i.max_val),
                    AAX_CUnitDisplayDelegateDecorator<int32_t>(DisplayDelegate(), units_str.c_str()),
                    !param.hidden
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(i.max_val - i.min_val + 1);
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            }
        }, param.semantics);
    }

    auto sample_rate = AAX_CSampleRate{};
    Controller()->GetSampleRate(&sample_rate);
    _kernel->reset(sample_rate);
    Controller()->SetSignalLatency(_kernel->latency_samps()); // Nothing pending, assume success.

    // Pro Tool Bypass
    // const auto bypass_id = AAX_CString{cDefaultMasterBypassID};
    // auto bypass_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
    //     bypass_id.CString(),
    //     AAX_CString{"Bypass (Pro Tools)"},
    //     false,
    //     AAX_CBinaryTaperDelegate<bool>(),
    //     AAX_CBinaryDisplayDelegate<bool>("Bypass", "On"),
    //     true
    // ));
    // bypass_param->AddShortenedName("Bypass");
    // bypass_param->SetNumberOfSteps(2);
    // bypass_param->SetType(AAX_eParameterType_Discrete);
    // mParameterManager.AddParameter(bypass_param.release());
    // mPacketDispatcher.RegisterPacket(bypass_id.CString(), AAX_FIELD_INDEX(Aax_context, bypass));

    return AAX_SUCCESS;
}

// MARK: - NotificationReceived

AAX_Result Aax_parameters::NotificationReceived(AAX_CTypeID inNotificationType, const void* inNotificationData, uint32_t /*inNotificationDataSize*/)
{
    switch (inNotificationType) {
        case AAX_eNotificationEvent_SignalLatencyChanged: {
            // Check that there is a pending latency request.
            const auto pending_latency = _pending_latency.exchange(std::nullopt, std::memory_order_acq_rel);
            if (pending_latency) {
                auto accepted_latency = int32_t{}; // In AAX, the controller owns the plug-in latency.
                if (Controller()->GetSignalLatency(&accepted_latency) == AAX_SUCCESS) {
                    _accepted_latency.store(accepted_latency, std::memory_order_release);
                }
            }
            break;
        }
        case AAX_eNotificationEvent_DelayCompensationState: {
            if (const auto* data = inNotificationData) {
                const auto info = static_cast<const int32_t*>(data);
                _delay_comp.store(*info > 0, std::memory_order_relaxed);
            }
            break;
        }
        case AAX_eNotificationEvent_TransportStateChanged: {
            if (const auto* data = inNotificationData) {
                const auto* info = static_cast<const AAX_TransportStateInfo_V1*>(data);
                recording.store(info->mIsRecording, std::memory_order_relaxed);
                break;
            }
        }
        default: break;
    }
    return AAX_SUCCESS;
}

// MARK: - RenderAudio

void Aax_parameters::RenderAudio(AAX_SInstrumentRenderInfo* ioRenderInfo, const TParamValPair* inSynchronizedParamValues[], int32_t inNumSynchronizedParamValues)
{
    // Accept latency.
    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        _kernel->handle_event(Accepted_latency{*accepted_latency});
        assert(_kernel->latency_samps() == *accepted_latency && "Kernel must apply the accepted latency!");
    }

    // Handle synchronized events.
    for (auto i = decltype(inNumSynchronizedParamValues){}; i < inNumSynchronizedParamValues; ++i) {
        const auto* sync_value = inSynchronizedParamValues[i];
        const auto aax_id = sync_value->first;
        const auto aax_param = sync_value->second;

        if (const auto tiny_id = aax_id_to_tiny(aax_id); *tiny_id < num_params) {
            auto value = double{};
            if (aax_param->GetValueAsDouble(&value)) {
                _kernel->handle_event(Set_param{.id = *tiny_id, .value = value});
            }
        }
    }

    auto action = User_action{};
    while (_from_ui.pop(action)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                const auto param = _param_infos.param_for(p.id);
                const auto denorm = Value_conv::knob_to_plain(p.value, param.semantics);
                _kernel->handle_event(Set_param{.id = p.id, .value = denorm});
            },
            [](const auto&) {}
        }, action);
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
    auto context = Dsp_context{
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
            const auto value = context.exports[i];
            _oqueue.push(Set_export{.id = static_cast<uint32_t>(i), .value = value});

            // Cache for next time.
            _lexports[i] = value;
        }

        _exports[i] = 0; // Reset for peak meters.
    }

    // Has the kernel proposed a latency.
    const auto delay_comp = _delay_comp.load(std::memory_order_relaxed);
    if (const auto proposed_latency = context.propose_latency; proposed_latency && delay_comp) {
        // Notify controller and sit on the pending latency.
        Controller()->SetSignalLatency(*proposed_latency);
        _pending_latency.store(true, std::memory_order_release);
    }
}

} // namespace tiny