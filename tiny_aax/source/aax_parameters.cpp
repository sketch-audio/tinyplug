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
    assert(params.size() == aax_ids.size() && "AAX IDs must have same size as param specs.");

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
                    param.policy == Host_policy::automation
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
                const auto num_items = l.items.size();
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<uint32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    static_cast<uint32_t>(l.def_val),
                    AAX_CStateTaperDelegate<uint32_t>(0, num_items - 1),
                    AAX_CStateDisplayDelegate<uint32_t>(num_items, const_cast<const char**>(l.items.data()), 0), // Yee haw.
                    param.policy == Host_policy::automation
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
            [&](const Int_semantics& i) {
                using DisplayDelegate = AAX_CNumberDisplayDelegate<int32_t, 0, 1>; // precision: 0, space after: 1
                const auto units_str = units_string(i.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<int32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    i.def_val,
                    AAX_CStateTaperDelegate<int32_t>(i.min_val, i.max_val),
                    AAX_CUnitDisplayDelegateDecorator<int32_t>(DisplayDelegate(), units_str.c_str()),
                    param.policy == Host_policy::automation
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
            },
            [&](const Real_semantics& r) {
                using TaperDelegate = Real_semanticsTaperDelegate<double>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 1, space after: 1
                const auto units_str = units_string(r.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    r.def_val,
                    TaperDelegate(r), // So we can use our own control adapter.
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    param.policy == Host_policy::automation
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

// MARK: - Chunk

AAX_Result Aax_parameters::GetNumberOfChunks(int32_t* oNumChunks) const
{
    *oNumChunks = 1;
    return AAX_SUCCESS;
}

AAX_Result Aax_parameters::GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const
{
    if (iIndex != 0)
	{
		*oChunkID = AAX_CTypeID(0);
		return AAX_ERROR_INVALID_CHUNK_INDEX;
	}
	
	*oChunkID = CHUNK_ID;
    return AAX_SUCCESS;
}

AAX_Result Aax_parameters::GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const
{
    if (iChunkID != CHUNK_ID)
	{
		*oSize = 0;
		return AAX_ERROR_INVALID_CHUNK_ID;
	}
	
    this->build_tiny_chunk(); // Our own chunk includes the tree version.
    mChunkSize = mChunkParser.GetChunkDataSize();
	
	if (mChunkSize < 0)
	{
		return AAX_ERROR_INCORRECT_CHUNK_SIZE;
	}
	
	*oSize = static_cast<uint32_t>(mChunkSize);
	return AAX_SUCCESS;
}

AAX_Result Aax_parameters::GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const
{
    //Check the chunkID
	if (iChunkID != CHUNK_ID)
		return AAX_ERROR_INVALID_CHUNK_ID;
	
    this->build_tiny_chunk(); // Our own chunk includes the tree version.
    int32_t currentChunkSize = mChunkParser.GetChunkDataSize();		//Verify that the chunk data size hasn't changed since the last GetChunkSize call.
	if (mChunkSize != currentChunkSize || mChunkSize == 0)
	{
		return AAX_ERROR_INCORRECT_CHUNK_SIZE;	//If mChunkSize doesn't match the currently built chunk, then its likely that the previous call to GetChunkSize() didn't return the correct size.
	}
	
	//Set the version on the chunk data structure.  The other manID, prodID, PlugID, and fSize are populated already, coming from AAXCollection.
	oChunk->fVersion = mChunkParser.GetChunkVersion();
	memset(oChunk->fName, 0, 32);		//Just in case, lets make sure unused chars are null.
	strncpy(reinterpret_cast<char *>(oChunk->fName), "AAX Plug-in State", 31);
	return mChunkParser.GetChunkData(oChunk);
}

AAX_Result Aax_parameters::SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk)
{
    if (iChunkID != CHUNK_ID)
        return AAX_ERROR_INVALID_CHUNK_ID;

    mChunkParser.LoadChunk(iChunk);

    const auto tree_version = static_cast<int32_t>(max_tree_version(_param_infos.tree()));
    auto state_version = int32_t{};
    if (!mChunkParser.FindInt32(tinyplug_tree_version, &state_version))
        return AAX_ERROR_MALFORMED_CHUNK;

    auto do_set = [&](const auto* id_cstr, auto* aax_param) {
        if (aax_param) {
            // ...
            auto d_value = double{};
            auto i_value = int32_t{};
            auto b_value = bool{};
            if (aax_param->GetValueAsDouble(&d_value)) {
                if (mChunkParser.FindDouble(id_cstr, &d_value))
                    aax_param->SetValueWithDouble(d_value);
            }
            else if (aax_param->GetValueAsInt32(&i_value)) {
                if (mChunkParser.FindInt32(id_cstr, &i_value))
                    aax_param->SetValueWithInt32(i_value);
            }
            else if (aax_param->GetValueAsBool(&b_value)) {
                if (mChunkParser.FindInt32(id_cstr, &i_value))
                    aax_param->SetValueWithBool(i_value > 0);
            }
        }
    };

    if (tree_version <= state_version) {
        // Implies "num params in tree" <= "num params in state"
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& param = _param_infos.param_for(i);
            if (const auto aax_id = tiny_id_to_aax(i)) {
                const auto* id_cstr = (*aax_id).c_str();
                AAX_IParameter* aax_param = nullptr;
                if (GetParameter(id_cstr, &aax_param) == AAX_SUCCESS && param.policy != Host_policy::interface) {
                    do_set(id_cstr, aax_param);
                }
            }
        }
    }
    else {
        // Implies "num params in tree" > "num params in state"
        const auto num_state = num_params_with_version(_param_infos.tree(), state_version);

        // Set values stored in state.
        for (auto i = decltype(num_state){}; i < num_state; ++i) {
            const auto& param = _param_infos.param_for(i);
            if (const auto aax_id = tiny_id_to_aax(i)) {
                const auto* id_cstr = (*aax_id).c_str();
                AAX_IParameter* aax_param = nullptr;
                if (GetParameter(id_cstr, &aax_param) == AAX_SUCCESS && param.policy != Host_policy::interface) {
                    do_set(id_cstr, aax_param);
                }
            }
        }

        // Set remaining parameters to defaults. 
        for (auto i = num_state; i < num_params; ++i) {
            const auto& param = _param_infos.param_for(i);
            if (const auto aax_id = tiny_id_to_aax(i)) {
                const auto* id_cstr = (*aax_id).c_str();
                const auto knob_value = get_knob_default(param);
                // Is there an AAX parameter?
                AAX_IParameter* aax_param = nullptr;
                if (GetParameter(id_cstr, &aax_param) == AAX_SUCCESS && param.policy != Host_policy::interface) {
                    aax_param->SetNormalizedValue(knob_value);
                }
            }
        }
    }

    return AAX_SUCCESS;
}

AAX_Result Aax_parameters::CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const
{
    if (iChunkP->fChunkID != CHUNK_ID) 
	{
		// If we don't know what the chunk is then we don't want to be turning on the compare light unnecessarily.
		*oIsEqual = true;
		return AAX_SUCCESS; 
    }

    *oIsEqual = false;
    mChunkParser.LoadChunk(iChunkP);

    const auto tree_version = static_cast<int32_t>(max_tree_version(_param_infos.tree()));
    auto chunk_version = int32_t{};
    const auto found = mChunkParser.FindInt32(tinyplug_tree_version, &chunk_version);
    if (!found || (found && tree_version != chunk_version))
        return AAX_SUCCESS;

    

    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        if (const auto aax_id = tiny_id_to_aax(i)) {
            const auto* id_cstr = (*aax_id).c_str();
            if (const auto* aax_param = mParameterManager.GetParameterByID(id_cstr); aax_param) {
                // ...
                auto d_value = double{}; auto chunk_d = double{};
                auto i_value = int32_t{}; auto chunk_i = int32_t{};
                auto b_value = bool{};
                if (aax_param->GetValueAsDouble(&d_value)) {
                    const auto found = mChunkParser.FindDouble(id_cstr, &chunk_d);
                    if (!found || (found && d_value != chunk_d))
                        return AAX_SUCCESS;
                }
                else if (aax_param->GetValueAsInt32(&i_value)) {
                    const auto found = mChunkParser.FindInt32(id_cstr, &chunk_i);
                    if (!found || (found && i_value != chunk_i))
                        return AAX_SUCCESS;
                }
                else if (aax_param->GetValueAsBool(&b_value)) {
                    const auto found = mChunkParser.FindInt32(id_cstr, &chunk_i);
                    const auto chunk_b = chunk_i > 0;
                    if (!found || (found && b_value != chunk_b))
                        return AAX_SUCCESS;
                }
            }
            else {
                return AAX_SUCCESS;
            }
        }
    }

    *oIsEqual = true;
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
            // ...
            auto d_value = double{};
            auto i_value = int32_t{};
            auto b_value = bool{};
            if (aax_param->GetValueAsDouble(&d_value)) {
                _kernel->handle_event(Set_param{.id = *tiny_id, .value = d_value});
            }
            else if (aax_param->GetValueAsInt32(&i_value)) {
                d_value = static_cast<double>(i_value);
                _kernel->handle_event(Set_param{.id = *tiny_id, .value = d_value});
            }
            else if (aax_param->GetValueAsBool(&b_value)) {
                d_value = b_value ? 1 : 0;
                _kernel->handle_event(Set_param{.id = *tiny_id, .value = d_value});
            }
        }
    }

    // Non-synchronized events come through here.
    auto action = User_action{};
    while (_from_ui.pop(action)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                const auto param = _param_infos.param_for(p.id);
                const auto plain_value = Value_conv::knob_to_plain(p.value, param.semantics);
                _kernel->handle_event(Set_param{.id = p.id, .value = plain_value});
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

// MARK: - private

void Aax_parameters::build_tiny_chunk() const
{
    mChunkParser.Clear();

    const auto max_version = static_cast<int32_t>(max_tree_version(_param_infos.tree()));
    mChunkParser.AddInt32(tinyplug_tree_version, max_version);

    // Not all parameters are registered to the parameter manager so we have to add them to the chunk ourselves.
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        if (const auto aax_id = tiny_id_to_aax(i)) {
            const auto* id_cstr = (*aax_id).c_str();
            const auto* aax_param = mParameterManager.GetParameterByID(id_cstr);
            if (aax_param) {
                // ...
                auto d_value = double{};
                auto i_value = int32_t{};
                auto b_value = bool{};
                if (aax_param->GetValueAsDouble(&d_value)) {
                    mChunkParser.AddDouble(id_cstr, d_value);
                }
                else if (aax_param->GetValueAsInt32(&i_value)) {
                    mChunkParser.AddInt32(id_cstr, i_value);
                }
                else if (aax_param->GetValueAsBool(&b_value)) {
                    mChunkParser.AddInt32(id_cstr, b_value ? 1 : 0);
                }
            }
        }
    }
}

} // namespace tiny