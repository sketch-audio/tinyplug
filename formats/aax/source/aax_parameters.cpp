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
    const auto& params = User_params::param_specs(Param_order::Presentation);
    const auto aax_ids = tree_to_aax_ids(User_params::param_tree());
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
                const auto num_items = static_cast<int32_t>(l.items.size());
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<int32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    static_cast<int32_t>(l.def_val),
                    AAX_CStateTaperDelegate<int32_t>(0, num_items - 1),
                    AAX_CStateDisplayDelegate<int32_t>(num_items, const_cast<const char**>(l.items.data()), 0), // Yee haw.
                    param.policy == Host_policy::automation
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(static_cast<uint32_t>(num_items));
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
                aax_param->SetNumberOfSteps(static_cast<uint32_t>(i.max_val - i.min_val + 1));
                aax_param->SetType(AAX_eParameterType_Discrete);
                if (aax_param->Automatable()) {
                    AddSynchronizedParameter(*aax_param);
                }
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const Fixed_semantics& f) {
                using TaperDelegate = Fixed_semanticsTaperDelegate<double>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 2, space after: 1
                const auto units_str = units_string(f.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    aax_id.c_str(),
                    AAX_CString(param.name),
                    f.def_val,
                    TaperDelegate(f),
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    param.policy == Host_policy::automation
                ));
                if (std::strlen(param.short_name) > 0) {
                    aax_param->AddShortenedName(param.short_name);
                }
                aax_param->SetNumberOfSteps(static_cast<uint32_t>((f.max_val - f.min_val) / f.step_size + 1)); // Step count here is number of values.
                aax_param->SetType(AAX_eParameterType_Continuous);
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
    _processor->reset(sample_rate);
    const auto sl = static_cast<int32_t>(_processor->latency_samps());
    Controller()->SetSignalLatency(sl); // Nothing pending, assume success.

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
                _recording.store(info->mIsRecording, std::memory_order_relaxed);
                break;
            }
        }
        default: break;
    }
    return AAX_SUCCESS;
}

// MARK: - Chunk
// A lot of this was copied from AAX_CEffectParameters initially.

AAX_Result Aax_parameters::GetNumberOfChunks(int32_t* oNumChunks) const
{
    *oNumChunks = 1;
    return AAX_SUCCESS;
}

AAX_Result Aax_parameters::GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const
{
    if (iIndex != 0) {
		*oChunkID = AAX_CTypeID(0);
		return AAX_ERROR_INVALID_CHUNK_INDEX;
	}
	
	*oChunkID = State_rules::Aax::chunk_id;
    return AAX_SUCCESS;
}

AAX_Result Aax_parameters::GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const
{
    if (iChunkID != State_rules::Aax::chunk_id) {
		*oSize = 0;
		return AAX_ERROR_INVALID_CHUNK_ID;
	}
	
    this->_build_chunk();
    mChunkSize = mChunkParser.GetChunkDataSize();
	
	if (mChunkSize < 0) {
		return AAX_ERROR_INCORRECT_CHUNK_SIZE;
	}
	
	*oSize = static_cast<uint32_t>(mChunkSize);
	return AAX_SUCCESS;
}

AAX_Result Aax_parameters::GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const
{
    //Check the chunkID
    if (iChunkID != State_rules::Aax::chunk_id) {
        return AAX_ERROR_INVALID_CHUNK_ID;
    }
	
    this->_build_chunk();

    // Verify that the chunk data size hasn't changed since the last GetChunkSize call.
    // If mChunkSize doesn't match the currently built chunk, then its likely that the previous call to GetChunkSize() didn't return the correct size.
    const auto currentChunkSize = mChunkParser.GetChunkDataSize();
	if (mChunkSize != currentChunkSize || mChunkSize == 0) {
		return AAX_ERROR_INCORRECT_CHUNK_SIZE;
    }
    
    // Set the version on the chunk data structure. The other manID, prodID, PlugID, and fSize are populated already, coming from AAXCollection.
	oChunk->fVersion = mChunkParser.GetChunkVersion();
	memset(oChunk->fName, 0, 32); // Just in case, lets make sure unused chars are null.
	std::strncpy(reinterpret_cast<char *>(oChunk->fName), "AAX Plug-in State", 31);
	return mChunkParser.GetChunkData(oChunk);
}

// MARK: - Set Chunk

AAX_Result Aax_parameters::SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk)
{
    if (iChunkID != State_rules::Aax::chunk_id) {
        return AAX_ERROR_INVALID_CHUNK_ID;
    }

    mChunkParser.LoadChunk(iChunk);

    // Get number of params in the chunk.
    auto val = int32_t{};
    const auto found_num_params = mChunkParser.FindInt32(State_rules::Aax::num_params, &val);
    if (!found_num_params) return AAX_ERROR_MALFORMED_CHUNK;

    const auto num_chunk_params = static_cast<uint32_t>(val); // We need unsigned.

    // Get the edit keys and parse with tags.
    auto edit_keys = AAX_CString{};
    [[maybe_unused]] const auto found_edit_keys = mChunkParser.FindString(State_rules::Aax::edit_keys, &edit_keys);
    //if (!found_edit_keys) return AAX_ERROR_MALFORMED_CHUNK;

    const auto parsed_edit_keys = unjoin_keys(std::string{edit_keys.CString()});

    auto find_and_set = [&](auto* aax_param, const auto* id_cstr) {
        auto b_value = bool{};
        auto i_value = int32_t{};
        auto d_value = double{};

        auto f_value = float{};

        // Check the parameter type, pull it out of the chunk, and then set the value.
        if (aax_param->GetValueAsBool(&b_value)) {
            if (mChunkParser.FindFloat(id_cstr, &f_value) && f_value != State_rules::no_value) {
                aax_param->SetValueWithBool(f_value > 0);
            }
        }
        else if (aax_param->GetValueAsInt32(&i_value)) {
            if (mChunkParser.FindFloat(id_cstr, &f_value) && f_value != State_rules::no_value) {
                aax_param->SetValueWithInt32(static_cast<int32_t>(f_value));
            }
        }
        else if (aax_param->GetValueAsDouble(&d_value)) {
            if (mChunkParser.FindFloat(id_cstr, &f_value) && f_value != State_rules::no_value) {
                aax_param->SetValueWithDouble(static_cast<double>(f_value));
            }
        }
        else {
            assert(false && "Unexpected parameter value type.");
        }
    };

    if (num_params <= num_chunk_params) {
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            if (auto* aax_param = get_aax_param(&mParameterManager, i)) {
                const auto& param = User_params::param_spec(i);
                const auto* id_cstr = aax_param->Identifier();
                if (State_rules::is_persistent(param)) {
                    find_and_set(aax_param, id_cstr);
                }
            }
        }
    }
    else {
        // Set values stored in state.
        for (auto i = decltype(num_chunk_params){}; i < num_chunk_params; ++i) {
            if (auto* aax_param = get_aax_param(&mParameterManager, i)) {
                const auto& param = User_params::param_spec(i);
                const auto* id_cstr = aax_param->Identifier();
                if (State_rules::is_persistent(param)) {
                    find_and_set(aax_param, id_cstr);
                }
            }
        }

        // Set remaining parameters to defaults. 
        for (auto i = num_chunk_params; i < num_params; ++i) {
            if (auto* aax_param = get_aax_param(&mParameterManager, i)) {
                const auto& param = User_params::param_spec(i);
                if (State_rules::is_persistent(param)) {
                    const auto knob_value = get_knob_default(param);
                    aax_param->SetNormalizedValue(knob_value);
                }
            }
        }
    }

    // Editor state.
    auto state_map = State_map{};
    for (const auto& [key, raw_tag] : parsed_edit_keys) {
        const auto tag = static_cast<State_tag>(raw_tag);

        auto value = State_item{};
        switch (tag) {
            case State_tag::Bool: {
                auto v = int32_t{};
                if (mChunkParser.FindInt32(key.c_str(), &v)) {
                    value = v > 0;
                    break;
                }
                return AAX_ERROR_MALFORMED_CHUNK;
            }
            case State_tag::Int: {
                auto v = int32_t{};
                if (mChunkParser.FindInt32(key.c_str(), &v)) {
                    value = v;
                    break;
                }
                return AAX_ERROR_MALFORMED_CHUNK;
            }
            case State_tag::Double: {
                auto v = double{};
                if (mChunkParser.FindDouble(key.c_str(), &v)) {
                    value = v;
                    break;
                }
                return AAX_ERROR_MALFORMED_CHUNK;
            }
            case State_tag::String: {
                auto v = AAX_CString{};
                if (mChunkParser.FindString(key.c_str(), &v)) {
                    value = std::string{v.CString()};
                    break;
                }
                return AAX_ERROR_MALFORMED_CHUNK;
            }
        }

        state_map.emplace(std::move(key), std::move(value));
    }

    _editor->load_state(state_map);

    return AAX_SUCCESS;
}

// MARK: - Compare Chunk

AAX_Result Aax_parameters::CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const
{
    if (iChunkP->fChunkID != State_rules::Aax::chunk_id) {
		// If we don't know what the chunk is then we don't want to be turning on the compare light unnecessarily.
		*oIsEqual = true;
		return AAX_SUCCESS; 
    }

    *oIsEqual = false;
    mChunkParser.LoadChunk(iChunkP);

    // Compare the number of parameters.
    auto num_chunk_params = int32_t{};
    const auto found_num_params = mChunkParser.FindInt32(State_rules::Aax::num_params, &num_chunk_params);

    if (!found_num_params || (found_num_params && num_params != num_chunk_params))
        return AAX_SUCCESS;

    // Compare the parameter values (now we know `num_chunk_params` and `num_params` are equal).
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        if (const auto* aax_param = get_aax_param(&mParameterManager, i)) {
            const auto* id_cstr = aax_param->Identifier();

            auto chunk_b = bool{};
            auto b_value = bool{};
            auto i_value = int32_t{};
            auto chunk_i = int32_t{};
            auto d_value = double{};
            auto chunk_d = double{};

            auto chunk_f = float{};

            if (aax_param->GetValueAsBool(&b_value)) {
                const auto found = mChunkParser.FindFloat(id_cstr, &chunk_f);
                if (chunk_f == State_rules::no_value) continue;
                chunk_b = chunk_f > 0;
                if (!found || (found && b_value != chunk_b))
                    return AAX_SUCCESS;
            }
            else if (aax_param->GetValueAsInt32(&i_value)) {
                const auto found = mChunkParser.FindFloat(id_cstr, &chunk_f);
                if (chunk_f == State_rules::no_value) continue;
                chunk_i = static_cast<int32_t>(chunk_f);
                if (!found || (found && i_value != chunk_i))
                    return AAX_SUCCESS;
            }
            else if (aax_param->GetValueAsDouble(&d_value)) {
                const auto found = mChunkParser.FindFloat(id_cstr, &chunk_f);
                if (chunk_f == State_rules::no_value) continue;
                chunk_d = static_cast<double>(chunk_f);
                if (!found || (found && std::abs(d_value - chunk_d) > 1e-7))
                    return AAX_SUCCESS;
            }
            else {
                assert(false && "Unexpected parameter value type.");
                return AAX_SUCCESS;
            }
        }
    }

    // We don't care about the editor state here.
    *oIsEqual = true;
    return AAX_SUCCESS;
}

// MARK: - RenderAudio

void Aax_parameters::RenderAudio(AAX_SInstrumentRenderInfo* ioRenderInfo, int32_t channelCount, const TParamValPair* inSynchronizedParamValues[], int32_t inNumSynchronizedParamValues)
{
    // Accept latency.
    const auto accepted_latency = _accepted_latency.exchange(std::nullopt, std::memory_order_acq_rel);
    if (accepted_latency) {
        _processor->handle_event(Accepted_latency{*accepted_latency});
        assert(_processor->latency_samps() == *accepted_latency && "Kernel must apply the accepted latency!");
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
                _processor->handle_event(Set_param{.address = *tiny_id, .value = d_value});
            }
            else if (aax_param->GetValueAsInt32(&i_value)) {
                d_value = static_cast<double>(i_value);
                _processor->handle_event(Set_param{.address = *tiny_id, .value = d_value});
            }
            else if (aax_param->GetValueAsBool(&b_value)) {
                d_value = b_value ? 1 : 0;
                _processor->handle_event(Set_param{.address = *tiny_id, .value = d_value});
            }
        }
    }

    // Non-synchronized events come through here.
    auto action = User_action{};
    while (_to_processor.pop(action)) {
        std::visit(Inline_visitor{
            [&](const Set_param& p) {
                const auto param = User_params::param_spec(p.address);
                const auto plain_value = Value_conv::knob_to_plain(p.value, param.semantics);
                _processor->handle_event(Set_param{.address = p.address, .value = plain_value});
            },
            [](const auto&) {}
        }, action);
    }

    // Assign buffer ptrs.
    for (auto i = 0; i < channelCount; ++i) {
        const auto idx = static_cast<size_t>(i);
        _ibuffers[idx] = ioRenderInfo->mAudioInputs[idx];
    }
    for (auto i = 0; i < channelCount; ++i) {
        const auto idx = static_cast<size_t>(i);
        _obuffers[idx] = ioRenderInfo->mAudioOutputs[idx];
    }
    if constexpr (Plug_info::wants_sidechain) {
        if (const auto sc_idx = ioRenderInfo->mSidechainIndex; sc_idx != nullptr) {
            for (size_t i = 0; i < max_schannels; ++i) {
                const auto sc = static_cast<size_t>(*sc_idx);
                _sbuffers[i] = ioRenderInfo->mAudioInputs[sc + i];
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

    host_data.is_recording = _recording.load(std::memory_order_relaxed); // From notifications.

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
        .ibuffers = {_ibuffers.begin(), static_cast<size_t>(channelCount)},
        .sbuffers = {_sbuffers.begin(), Plug_info::wants_sidechain ? max_schannels : 0}, // Always mono sidechain.
        .obuffers = {_obuffers.begin(), static_cast<size_t>(channelCount)},
        .num_frames = num_frames,
        .meters = _meters
    };
    _processor->process(context);

    // Write exports to meters.
    for (size_t i = 0; i < num_meters; ++i) {
        if (context.meters[i] != _last_meters[i]) {
            // Send an output event.
            const auto value = context.meters[i];
            _to_editor.push(Set_meter{.address = static_cast<uint32_t>(i), .value = value});

            // Cache for next time.
            _last_meters[i] = value;
        }

        _meters[i] = 0; // Reset for peak meters.
    }

    // Has the kernel proposed a latency.
    const auto delay_comp = _delay_comp.load(std::memory_order_relaxed);
    if (const auto proposed_latency = context.propose_latency; proposed_latency && delay_comp) {
        // Notify controller and sit on the pending latency.
        const auto sl = static_cast<int32_t>(*proposed_latency);
        Controller()->SetSignalLatency(sl);
        _pending_latency.store(true, std::memory_order_release);
    }
}

// MARK: - private

void Aax_parameters::_build_chunk() const
{
    mChunkParser.Clear();

    const auto edit_state = _editor->save_state();
    const auto edit_keys = join_keys(edit_state);

    // Add the number of parameters and the edit keys.
    mChunkParser.AddInt32(State_rules::Aax::num_params, num_params);
    mChunkParser.AddString(State_rules::Aax::edit_keys, edit_keys.c_str());

    // Add the parameter values.
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        if (const auto* aax_param = get_aax_param(&mParameterManager, i)) {

            const auto& spec = User_params::param_spec(i);
            const auto* id_cstr = aax_param->Identifier();

            // Params should be either bool, int32_t, or double.
            auto b_value = bool{};
            auto i_value = int32_t{};
            auto d_value = double{};

            if (aax_param->GetValueAsBool(&b_value)) {
                const auto as_float = b_value ? 1.f : 0.f;
                const auto to_write = State_rules::is_persistent(spec) ? as_float : State_rules::no_value;
                mChunkParser.AddFloat(id_cstr, to_write);
            }
            else if (aax_param->GetValueAsInt32(&i_value)) {
                const auto as_float = static_cast<float>(i_value);
                const auto to_write = State_rules::is_persistent(spec) ? as_float : State_rules::no_value;
                mChunkParser.AddFloat(id_cstr, to_write);
            }
            else if (aax_param->GetValueAsDouble(&d_value)) {
                const auto as_float = static_cast<float>(d_value);
                const auto to_write = State_rules::is_persistent(spec) ? as_float : State_rules::no_value;
                mChunkParser.AddFloat(id_cstr, to_write);
            }
            else {
                assert(false && "Unexpected parameter value type.");
            }
        }
    }

    // Add the editor state.
    for (const auto& [key, val] : edit_state) {
        const auto tag = tag_for(val);

        switch (tag) {
            case State_tag::Bool: {
                if (const auto b = std::get_if<bool>(&val)) {
                    mChunkParser.AddInt32(key.c_str(), *b ? 1 : 0);
                }
                break;
            }
            case State_tag::Int: {
                if (const auto i = std::get_if<int32_t>(&val)) {
                    mChunkParser.AddInt32(key.c_str(), *i);
                }
                break;
            }
            case State_tag::Double: {
                if (const auto d = std::get_if<double>(&val)) {
                    mChunkParser.AddDouble(key.c_str(), *d);
                    break;
                }
            }
            case State_tag::String: {
                if (const auto s = std::get_if<std::string>(&val)) {
                    mChunkParser.AddString(key.c_str(), (*s).c_str());
                }
                break;
            }
            default:
                break;
        }
    }
}

} // namespace tiny