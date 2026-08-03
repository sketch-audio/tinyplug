#include "parameters.hpp"

#include <cassert>
#include <cstring>

#include "AAX_CBinaryTaperDelegate.h"
#include "AAX_CBinaryDisplayDelegate.h"
#include "AAX_CNumberDisplayDelegate.h"
#include "AAX_CStateTaperDelegate.h"
#include "AAX_CStateDisplayDelegate.h"
#include "AAX_CUnitDisplayDelegateDecorator.h"
#include "AAX_TransportTypes.h"

#include "adapters.hpp"
#include "taper_delegate.hpp"

namespace tiny::aax {

// MARK: - EffectInit

AAX_Result Parameters::EffectInit()
{
    using namespace params;

    const auto& params = User_params::param_specs(Param_order::Presentation);
    const auto aax_ids = tree_to_aax_ids(User_params::param_tree());
    assert(params.size() == aax_ids.size() && "AAX IDs must have same size as param specs.");

    for (size_t i = 0; i < params.size(); ++i) {
        const auto& param = params[i];
        const auto& aax_id = aax_ids[i];

        std::visit(Inline_visitor{
            [&](const params::Semantics::Bool& b) {
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
                    aax_id.c_str(),
                    AAX_CString(param.name.c_str()),
                    b.def_val,
                    AAX_CBinaryTaperDelegate<bool>(),
                    AAX_CBinaryDisplayDelegate<bool>("False", "True"),
                    param.policy == params::Policy::Automation
                ));
                if (!param.short_name.empty()) {
                    aax_param->AddShortenedName(param.short_name.c_str());
                }
                aax_param->SetNumberOfSteps(2);
                aax_param->SetType(AAX_eParameterType_Discrete);
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const params::Semantics::List& l) {
                const auto num_items = static_cast<int32_t>(l.items.size());
                // The delegate copies each NULL-terminated C string during construction,
                // but string_view::data() isn't guaranteed NULL-terminated — so materialize
                // owning std::strings (these outlive the constructor call) and pass their c_str().
                auto item_storage = std::vector<std::string>{};
                item_storage.reserve(l.items.size());
                for (const auto& it : l.items) item_storage.emplace_back(it);
                auto item_cstrs = std::vector<const char*>{};
                item_cstrs.reserve(item_storage.size());
                for (const auto& s : item_storage) item_cstrs.push_back(s.c_str());
                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<int32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name.c_str()),
                    static_cast<int32_t>(l.def_val),
                    AAX_CStateTaperDelegate<int32_t>(0, num_items - 1),
                    AAX_CStateDisplayDelegate<int32_t>(num_items, item_cstrs.data(), 0), // Yee haw.
                    param.policy == params::Policy::Automation
                ));
                if (!param.short_name.empty()) {
                    aax_param->AddShortenedName(param.short_name.c_str());
                }
                // AAX validator reports errors for parameters with more than 2048 steps.
                // See: https://dev.avid.com/MP_DeveloperForumSupport?filterId=a9T310000004FCnEAM#!/feedtype=SINGLE_QUESTION_DETAIL&dc=Developer_Community_Q_A&criteria=ALLQUESTIONS&id=9065A000000oScuQAE
                const auto steps = std::min(num_items, 2048);
                aax_param->SetNumberOfSteps(static_cast<uint32_t>(steps));
                aax_param->SetType(AAX_eParameterType_Discrete);
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const params::Semantics::Int& i) {
                using DisplayDelegate = AAX_CNumberDisplayDelegate<int32_t, 0, 1>; // precision: 0, space after: 1
                const auto units_str = Value_helper::units_label(i.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<int32_t>(
                    aax_id.c_str(),
                    AAX_CString(param.name.c_str()),
                    i.def_val,
                    AAX_CStateTaperDelegate<int32_t>(i.min_val, i.max_val),
                    AAX_CUnitDisplayDelegateDecorator<int32_t>(DisplayDelegate(), units_str.c_str()),
                    param.policy == params::Policy::Automation
                ));
                if (!param.short_name.empty()) {
                    aax_param->AddShortenedName(param.short_name.c_str());
                }
                // AAX validator reports errors for parameters with more than 2048 steps.
                // See: https://dev.avid.com/MP_DeveloperForumSupport?filterId=a9T310000004FCnEAM#!/feedtype=SINGLE_QUESTION_DETAIL&dc=Developer_Community_Q_A&criteria=ALLQUESTIONS&id=9065A000000oScuQAE
                const auto steps = std::min(i.max_val - i.min_val + 1, 2048);
                assert(steps >= 0 && "params::Semantics::Int must have max_val >= min_val."); // Otherwise we're gonna have an issue with the cast.
                aax_param->SetNumberOfSteps(static_cast<uint32_t>(steps));
                aax_param->SetType(AAX_eParameterType_Discrete);
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const params::Semantics::Fixed& f) {
                using TaperDelegate = Fixed_semanticsTaperDelegate<double>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 2, space after: 1
                const auto units_str = Value_helper::units_label(f.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    aax_id.c_str(),
                    AAX_CString(param.name.c_str()),
                    f.def_val,
                    TaperDelegate(f),
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    param.policy == params::Policy::Automation
                ));
                if (!param.short_name.empty()) {
                    aax_param->AddShortenedName(param.short_name.c_str());
                }
                // AAX validator reports errors for parameters with more than 2048 steps.
                // See: https://dev.avid.com/MP_DeveloperForumSupport?filterId=a9T310000004FCnEAM#!/feedtype=SINGLE_QUESTION_DETAIL&dc=Developer_Community_Q_A&criteria=ALLQUESTIONS&id=9065A000000oScuQAE
                const auto steps_raw = (f.max_val - f.min_val) / f.step_size + 1;
                const auto steps = std::min(steps_raw, 2048.);
                assert(steps >= 0 && "params::Semantics::Fixed must have max_val >= min_val."); // Otherwise we're gonna have an issue with the cast.
                aax_param->SetNumberOfSteps(static_cast<uint32_t>(steps)); // Step count here is number of values.
                aax_param->SetType(AAX_eParameterType_Continuous);
                mParameterManager.AddParameter(aax_param.release());
            },
            [&](const params::Semantics::Real& r) {
                using TaperDelegate = Real_semanticsTaperDelegate<double>;
                using DisplayDelegate = AAX_CNumberDisplayDelegate<double, 1, 1>; // precision: 1, space after: 1
                const auto units_str = Value_helper::units_label(r.units);

                auto aax_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<double>(
                    aax_id.c_str(),
                    AAX_CString(param.name.c_str()),
                    r.def_val,
                    TaperDelegate(r), // So we can use our own control adapter.
                    AAX_CUnitDisplayDelegateDecorator<double>(DisplayDelegate(), units_str.c_str()),
                    param.policy == params::Policy::Automation
                ));
                if (!param.short_name.empty()) {
                    aax_param->AddShortenedName(param.short_name.c_str());
                }
                aax_param->SetNumberOfSteps(2048); // Most steps that will pass validation.
                aax_param->SetType(AAX_eParameterType_Continuous);
                mParameterManager.AddParameter(aax_param.release());
            },
        }, param.semantics);
    }

    // Pro Tools master bypass. It packs into the coefficient segments as a
    // pseudo-parameter at `bypass_address`, so the algorithm receives it exactly like
    // any other value and Host_bypass lives beside the kernel.
    const auto bypass_id = AAX_CString{cDefaultMasterBypassID};
    auto bypass_param = std::unique_ptr<AAX_IParameter>(new AAX_CParameter<bool>(
        bypass_id.CString(),
        AAX_CString{"Bypass"},
        false,
        AAX_CBinaryTaperDelegate<bool>(),
        AAX_CBinaryDisplayDelegate<bool>("Bypass", "On"),
        true
    ));
    bypass_param->AddShortenedName("Bypass");
    bypass_param->SetNumberOfSteps(2);
    bypass_param->SetType(AAX_eParameterType_Discrete);
    mParameterManager.AddParameter(bypass_param.release());

    auto sample_rate = AAX_CSampleRate{};
    Controller()->GetSampleRate(&sample_rate);

    // The config packet is delivered before the algorithm's instance-init callback
    // runs, which is what lets the kernel's allocating reset() happen there rather
    // than on the real-time thread.
    _config.sample_rate = static_cast<double>(sample_rate);
    _config.max_frames = 0;
    _config.pad = 0;
    _config_dirty = true;

    // Everything is dirty at startup so the first GenerateCoefficients hands the
    // algorithm a complete initial state.
    _segment_dirty.fill(true);
    _runtime_dirty.store(true, std::memory_order_release);

#if TINY_HAS_WORKER
    _worker_runner.start(sample_rate);
#endif

    return AAX_SUCCESS;
}

// MARK: - coefficient packets

auto Parameters::_mark_dirty(uint32_t address) -> void
{
    const auto segment = segment_of(address);
    if (segment < num_segments) {
        _segment_dirty[segment] = true;
    }
}

auto Parameters::_plain_value(uint32_t address) const -> double
{
    const auto* aax_param = address == bypass_address
        ? mParameterManager.GetParameterByID(cDefaultMasterBypassID)
        : get_aax_param(&mParameterManager, address);

    if (aax_param == nullptr) return 0.;

    // Only the getter matching the parameter's own storage type returns true, so this
    // reads the exact stored value with no round trip through normalized space. Our
    // taper delegates make the stored value plain space for every semantic.
    auto d_value = double{};
    auto i_value = int32_t{};
    auto b_value = bool{};
    if (aax_param->GetValueAsDouble(&d_value)) return d_value;
    if (aax_param->GetValueAsInt32(&i_value)) return static_cast<double>(i_value);
    if (aax_param->GetValueAsBool(&b_value)) return b_value ? 1. : 0.;

    assert(false && "Unexpected parameter value type.");
    return 0.;
}

auto Parameters::_fill_segment(size_t segment) -> void
{
    auto& out = _segments[segment];
    out.seq = _seq++;

    const auto base = segment * coefs_per_segment;
    for (auto i = size_t{}; i < coefs_per_segment; ++i) {
        const auto address = base + i;
        out.value[i] = address < num_coefs ? _plain_value(static_cast<uint32_t>(address)) : 0.;
    }
}

auto Parameters::_post_segment(size_t segment) -> void
{
    _fill_segment(segment);
    Controller()->PostPacket(coef_field(segment), &_segments[segment], sizeof(Coef_segment));
}

auto Parameters::_post_config() -> void
{
    Controller()->PostPacket(field_config, &_config, sizeof(_config));
}

auto Parameters::_post_runtime() -> void
{
    Controller()->PostPacket(field_runtime, &_runtime, sizeof(_runtime));
}

AAX_Result Parameters::UpdateParameterNormalizedValue(AAX_CParamID iParamID, double aValue, AAX_EUpdateSource inSource)
{
    const auto result = Super::UpdateParameterNormalizedValue(iParamID, aValue, inSource);
    if (result != AAX_SUCCESS) return result;

    if (const auto tiny_id = aax_id_to_tiny(iParamID); tiny_id && *tiny_id < num_params) {
        _mark_dirty(*tiny_id);
    }
    else if (std::strcmp(iParamID, cDefaultMasterBypassID) == 0) {
        _mark_dirty(bypass_address);
    }

    return AAX_SUCCESS;
}

AAX_Result Parameters::GenerateCoefficients()
{
    // The host timestamps every packet posted inside this method with the automation
    // breakpoint's timeline position, then splits render buffers (down to 32 samples
    // on Native) so the algorithm sees it at the right sample. That is the whole
    // reason to be decoupled — don't post from anywhere else.
    if (_config_dirty) {
        _post_config();
        _config_dirty = false;
    }

    if (_runtime_dirty.exchange(false, std::memory_order_acq_rel)) {
        _post_runtime();
    }

    for (auto segment = size_t{}; segment < num_segments; ++segment) {
        if (!_segment_dirty[segment]) continue;
        _segment_dirty[segment] = false;
        _post_segment(segment);
    }

    return AAX_SUCCESS;
}

AAX_Result Parameters::TimerWakeup()
{
    // Notifications (offline mode, transport state, accepted latency) change the
    // runtime packet without touching a parameter, so nothing would otherwise call
    // GenerateCoefficients. Posting from here is only a problem for unbuffered ports
    // (PTSW-187216); the runtime port is buffered, which is the documented workaround.
    if (_runtime_dirty.exchange(false, std::memory_order_acq_rel)) {
        _post_runtime();
    }

    return Super::TimerWakeup();
}

// MARK: - custom data (Direct Data bridge)

AAX_Result Parameters::SetCustomData(AAX_CTypeID iDataBlockID, uint32_t inDataSize, const void* iData)
{
    if (iDataBlockID != custom_data_return || iData == nullptr) {
        return Super::SetCustomData(iDataBlockID, inDataSize, iData);
    }
    if (inDataSize < sizeof(Return_block)) return AAX_ERROR_INVALID_ARGUMENT;

    auto block = Return_block{};
    std::memcpy(&block, iData, sizeof(block));

    const auto* payload = static_cast<const unsigned char*>(iData) + sizeof(block);
    if (inDataSize - sizeof(block) < block.payload_bytes) return AAX_ERROR_INVALID_ARGUMENT;

    switch (static_cast<Ring_kind>(block.kind)) {
        case Ring_kind::Meter: {
            if (block.payload_bytes != sizeof(Ring_meter)) break;
            auto meter = Ring_meter{};
            std::memcpy(&meter, payload, sizeof(meter));
            if (meter.address < num_meters) {
                _last_meters[meter.address] = meter.value;
                _meter_queue.push(Set_meter{.address = meter.address, .value = meter.value});
            }
            break;
        }
        case Ring_kind::Propose_latency: {
            if (block.payload_bytes != sizeof(Ring_latency)) break;
            auto latency = Ring_latency{};
            std::memcpy(&latency, payload, sizeof(latency));
            // In AAX the host owns latency: propose, then read back what it accepted
            // when the notification arrives.
            Controller()->SetSignalLatency(static_cast<int32_t>(latency.samples));
            _pending_latency.store(true, std::memory_order_release);
            break;
        }
        case Ring_kind::Worker_from_processor: {
#if TINY_HAS_WORKER
            using From_processor = typename User_worker::Model::From_processor;
            if constexpr (!std::is_same_v<From_processor, std::monostate>) {
                if (block.payload_bytes != sizeof(From_processor)) break;
                auto msg = From_processor{};
                std::memcpy(&msg, payload, sizeof(msg));
                [[maybe_unused]] const auto pushed = _worker_from_proc.push(msg);
                assert(pushed && "Push to worker queue failed! Increase queue size.");
            }
#endif
            break;
        }
        case Ring_kind::Worker_to_processor:
        default:
            break;
    }

    return AAX_SUCCESS;
}

AAX_Result Parameters::GetCustomData(AAX_CTypeID iDataBlockID, uint32_t inDataSize, void* oData, uint32_t* oDataWritten) const
{
    if (iDataBlockID != custom_data_worker_reply) {
        return Super::GetCustomData(iDataBlockID, inDataSize, oData, oDataWritten);
    }

    if (oDataWritten != nullptr) *oDataWritten = 0;

#if TINY_HAS_WORKER
    using To_processor = typename User_worker::Model::To_processor;
    if constexpr (!std::is_same_v<To_processor, std::monostate>) {
        if (oData == nullptr || inDataSize < sizeof(To_processor)) return AAX_ERROR_INVALID_ARGUMENT;

        auto reply = To_processor{};
        if (!_worker_to_proc.pop(reply)) return AAX_SUCCESS; // Nothing pending.

        std::memcpy(oData, &reply, sizeof(reply));
        if (oDataWritten != nullptr) *oDataWritten = static_cast<uint32_t>(sizeof(reply));
    }
#else
    (void)inDataSize;
    (void)oData;
#endif

    return AAX_SUCCESS;
}

// MARK: - NotificationReceived

AAX_Result Parameters::NotificationReceived(AAX_CTypeID inNotificationType, const void* inNotificationData, uint32_t /*inNotificationDataSize*/)
{
    switch (inNotificationType) {
        case AAX_eNotificationEvent_SignalLatencyChanged: {
            // Check that there is a pending latency request.
            if (_pending_latency.exchange(false, std::memory_order_acq_rel)) {
                auto accepted_latency = int32_t{}; // In AAX, the controller owns the plug-in latency.
                if (Controller()->GetSignalLatency(&accepted_latency) == AAX_SUCCESS) {
                    _runtime.accepted_latency = static_cast<uint32_t>(accepted_latency);
                    _runtime.latency_seq += 1;
                    _runtime_dirty.store(true, std::memory_order_release);
                }
            }
            break;
        }
        case AAX_eNotificationEvent_DelayCompensationState: {
            if (const auto* data = inNotificationData) {
                const auto info = static_cast<const int32_t*>(data);
                _runtime.delay_comp = *info > 0 ? uint8_t{1} : uint8_t{0};
                _runtime_dirty.store(true, std::memory_order_release);
            }
            break;
        }
        case AAX_eNotificationEvent_TransportStateChanged: {
            if (const auto* data = inNotificationData) {
                const auto* info = static_cast<const AAX_TransportStateInfo_V1*>(data);
                _runtime.recording = info->mIsRecording ? uint8_t{1} : uint8_t{0};
                _runtime_dirty.store(true, std::memory_order_release);
            }
            break;
        }
        case AAX_eNotificationEvent_EnteringOfflineMode: {
            _runtime.offline = 1;
            _runtime_dirty.store(true, std::memory_order_release);
            break;
        }
        case AAX_eNotificationEvent_ExitingOfflineMode: {
            _runtime.offline = 0;
            _runtime_dirty.store(true, std::memory_order_release);
            break;
        }
        default: break;
    }
    return AAX_SUCCESS;
}

// MARK: - Chunk
// A lot of this was copied from AAX_CEffectParameters initially.

AAX_Result Parameters::GetNumberOfChunks(int32_t* oNumChunks) const
{
    *oNumChunks = 1;
    return AAX_SUCCESS;
}

AAX_Result Parameters::GetChunkIDFromIndex(int32_t iIndex, AAX_CTypeID* oChunkID) const
{
    if (iIndex != 0) {
		*oChunkID = AAX_CTypeID(0);
		return AAX_ERROR_INVALID_CHUNK_INDEX;
	}

	*oChunkID = State_rules::Aax::chunk_id;
    return AAX_SUCCESS;
}

AAX_Result Parameters::GetChunkSize(AAX_CTypeID iChunkID, uint32_t* oSize) const
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

AAX_Result Parameters::GetChunk(AAX_CTypeID iChunkID, AAX_SPlugInChunk* oChunk) const
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
	static constexpr char name[] = "AAX Plug-in State";
	static_assert(sizeof(name) <= 32, "Chunk name must fit fName[32].");
	std::memcpy(oChunk->fName, name, sizeof(name)); // fName was zeroed above; copy incl. terminator.
	return mChunkParser.GetChunkData(oChunk);
}

// MARK: - Set Chunk

auto Parameters::_snapshot_knob_params() -> std::array<double, num_params>
{
    auto out = std::array<double, num_params>{};
    for (auto i = decltype(num_params){}; i < num_params; ++i) {
        if (auto* aax_param = get_aax_param(&mParameterManager, i)) {
            out[i] = aax_param->GetNormalizedValue(); // AAX normalized == knob space.
        }
    }
    return out;
}

AAX_Result Parameters::SetChunk(AAX_CTypeID iChunkID, const AAX_SPlugInChunk* iChunk)
{
    using namespace params;

    if (iChunkID != State_rules::Aax::chunk_id) {
        return AAX_ERROR_INVALID_CHUNK_ID;
    }

    mChunkParser.LoadChunk(iChunk);

    // Snapshot for host-load undo capture (knob space, pre-load).
    const auto before = _snapshot_knob_params();

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
                    const auto knob_value = Value_helper::default_value(param, Space::Knob);
                    aax_param->SetNormalizedValue(knob_value);
                }
            }
        }
    }

    // Record the host load as one coalesced undo step (works editor open or closed).
    // Done here, after the param section, so it survives the editor-state early
    // returns below. The editor notify() is dispatched at the end of SetChunk, after
    // the editor state loads, so a preset's name/marker can fold into this step.
    const auto host_after = _snapshot_knob_params();
    auto host_changes = std::vector<Set_param>{};
    _undo_history.push_host_load(before, host_after, host_changes);

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
            default:
                break;
        }

        state_map.emplace(std::move(key), std::move(value));
    }

    // Prime the framework-owned size cache so the view opens pre-sized, then strip the
    // keys so the app editor never sees them.
    if (const auto size = editor_size_state::extract(state_map)) {
        resized({size->first, size->second});
    }
    editor_size_state::strip(state_map);

    _editor->load_state(state_map);

    // Bypass is a real AAX parameter now, so restoring it goes through the normal
    // parameter path and reaches the algorithm as a coefficient like anything else.
    auto bypassed = float{};
    if (mChunkParser.FindFloat(State_rules::Aax::host_bypass, &bypassed)) {
        if (auto* bypass_param = mParameterManager.GetParameterByID(cDefaultMasterBypassID)) {
            bypass_param->SetValueWithBool(bypassed >= 0.5f);
        }
    }

    // Notify the editor of the host load synchronously (works editor open or closed),
    // letting it fold its marker params into the load's single undo step via add_param.
    if (_editor) {
        auto add_param = [this](uint32_t addr, double knob) {
            if (auto* aax_param = get_aax_param(&mParameterManager, addr)) {
                const auto from = aax_param->GetNormalizedValue(); // Normalized == knob space.
                aax_param->SetNormalizedValue(knob);
                _undo_history.amend_host_load(addr, from, knob);
            }
        };
        _editor->notify(Host_event{Host_preset_loaded{
            .changes = host_changes,
            .params = host_after,
            .add_param = add_param,
        }});
    }

    return AAX_SUCCESS;
}

// MARK: - Compare Chunk

AAX_Result Parameters::CompareActiveChunk(const AAX_SPlugInChunk* iChunkP, AAX_CBoolean* oIsEqual) const
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

    if (!found_num_params || (found_num_params && num_params != static_cast<uint32_t>(num_chunk_params)))
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

// MARK: - private

void Parameters::_build_chunk() const
{
    mChunkParser.Clear();

    auto edit_state = _editor->save_state();

    // Inject the framework-owned editor window size (from our own cache) so the window
    // reopens pre-sized. The app editor never emits these keys.
    if (_last_size) {
        editor_size_state::inject(edit_state, _last_size->w, _last_size->h);
    }

    const auto edit_keys = join_keys(edit_state);

    // Add the number of parameters and the edit keys.
    mChunkParser.AddInt32(State_rules::Aax::num_params, static_cast<int32_t>(num_params));
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

    // Add the bypass state, read back from the parameter that now owns it.
    auto bypassed = bool{};
    if (const auto* bypass_param = mParameterManager.GetParameterByID(cDefaultMasterBypassID)) {
        bypass_param->GetValueAsBool(&bypassed);
    }
    mChunkParser.AddFloat(State_rules::Aax::host_bypass, bypassed ? 1.f : 0.f);

}

} // namespace tiny::aax
