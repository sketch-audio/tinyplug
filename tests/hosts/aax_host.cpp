// A fake AAX host. There is no bundle to load without reimplementing Pro Tools' description
// host, so the wrapper's own sources are compiled in against one demo, and the host drives the
// real data model (Parameters), Direct Data module, GUI and algorithm through the SDK's
// interfaces, from the threads Pro Tools uses, so the sanitizers can see the wrapper:
//
//     aax_host_<Demo> [--seed N] [--seconds S]
//
// Main is the host and GUI thread. The algorithm renders on an audio thread, the Direct Data
// timer runs on its own, and chunk calls also come from other threads, concurrently, as the
// SDK allows unless a plug-in sets AAX_eProperty_RequiresChunkCallsOnMainThread.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <audio_bench/audio_bench.hpp>

#include "AAX_IACFAutomationDelegate.h"
#include "AAX_IACFController.h"
#include "AAX_IACFViewContainer.h"
#include "AAX_IPrivateDataAccess.h"
#include "AAX_UIDs.h"
#include "acfextras.h"

#include "alg_proc.hpp"
#include "direct_data.hpp"
#include "parameters.hpp"
#include "../../wrappers/aax/source/gui.hpp" // By path: the fake hosts' own gui.hpp shadows it.

#include "gui.hpp"
#include "support.hpp"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TINY_HOST_TSAN 1
extern "C" void AnnotateIgnoreReadsBegin(const char* file, int line);
extern "C" void AnnotateIgnoreReadsEnd(const char* file, int line);
#endif
#endif

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::hosts::Random;
namespace aax = tiny::aax;

auto g_options = tiny::hosts::Options{};
const auto g_main_thread = std::this_thread::get_id();

auto on_main() -> bool { return std::this_thread::get_id() == g_main_thread; }

// Contract violations the host saw, from any thread.
class Violations {
public:
    auto add(std::string what) -> void
    {
        const auto lock = std::lock_guard{_mutex};
        if (_list.size() < 32) _list.push_back(std::move(what));
        ++_count;
    }
    auto take() -> std::pair<long, std::vector<std::string>>
    {
        const auto lock = std::lock_guard{_mutex};
        auto out = std::pair{_count, std::move(_list)};
        _count = 0;
        _list = {};
        return out;
    }
private:
    std::mutex _mutex{};
    std::vector<std::string> _list{};
    long _count{};
};

// MARK: - ACF plumbing

// Objects the host owns outright: ACF reference counts are answered but never free anything.
#define TINY_HOST_NO_REFCOUNT \
    acfUInt32 ACFMETHODCALLTYPE AddRef() override { return 1; } \
    acfUInt32 ACFMETHODCALLTYPE Release() override { return 1; }

// PostPacket and the automation echo: what the host would timestamp and deliver.
struct Packet {
    AAX_CFieldIndex field{};
    std::vector<unsigned char> bytes{};
};

class Fake_controller : public AAX_IACFController {
public:

    explicit Fake_controller(Violations& violations) : _violations{violations} {}

    ACFMETHOD(QueryInterface)(const acfIID&, void** out) override { *out = nullptr; return ACF_E_NOINTERFACE; }
    TINY_HOST_NO_REFCOUNT

    AAX_Result GetEffectID(AAX_IString*) const override { return AAX_SUCCESS; }
    AAX_Result GetSampleRate(AAX_CSampleRate* out) const override { *out = static_cast<AAX_CSampleRate>(sample_rate.load()); return AAX_SUCCESS; }
    AAX_Result GetInputStemFormat(AAX_EStemFormat* out) const override { *out = AAX_eStemFormat_Stereo; return AAX_SUCCESS; }
    AAX_Result GetOutputStemFormat(AAX_EStemFormat* out) const override { *out = AAX_eStemFormat_Stereo; return AAX_SUCCESS; }
    AAX_Result GetSignalLatency(int32_t* out) const override { *out = accepted_latency.load(); return AAX_SUCCESS; }
    AAX_Result GetCycleCount(AAX_EProperty, AAX_CPropertyValue* out) const override { *out = 0; return AAX_SUCCESS; }
    AAX_Result GetTODLocation(AAX_CTimeOfDay* out) const override { *out = 0; return AAX_SUCCESS; }
    AAX_Result SetCycleCount(AAX_EProperty*, AAX_CPropertyValue*, int32_t) override { return AAX_SUCCESS; }
    AAX_Result GetCurrentMeterValue(AAX_CTypeID, float* out) const override { *out = 0; return AAX_SUCCESS; }
    AAX_Result GetMeterPeakValue(AAX_CTypeID, float* out) const override { *out = 0; return AAX_SUCCESS; }
    AAX_Result ClearMeterPeakValue(AAX_CTypeID) const override { return AAX_SUCCESS; }
    AAX_Result GetMeterClipped(AAX_CTypeID, AAX_CBoolean* out) const override { *out = false; return AAX_SUCCESS; }
    AAX_Result ClearMeterClipped(AAX_CTypeID) const override { return AAX_SUCCESS; }
    AAX_Result GetMeterCount(uint32_t* out) const override { *out = 0; return AAX_SUCCESS; }
    AAX_Result GetNextMIDIPacket(AAX_CFieldIndex*, AAX_CMidiPacket*) override { return AAX_ERROR_UNIMPLEMENTED; }

    // The host owns latency: a request is answered later, on main, with a notification.
    AAX_Result SetSignalLatency(int32_t samples) override
    {
        requested_latency.store(samples);
        latency_requested.store(true);
        return AAX_SUCCESS;
    }

    // Packets are posted from GenerateCoefficients, or TimerWakeup for the buffered runtime port.
    AAX_Result PostPacket(AAX_CFieldIndex field, const void* payload, uint32_t size) override
    {
        if (!on_main()) _violations.add(std::format("PostPacket(field {}) off the main thread", field));
        const auto* bytes = static_cast<const unsigned char*>(payload);
        const auto lock = std::lock_guard{_packet_mutex};
        _packets.push_back({field, {bytes, bytes + size}});
        return AAX_SUCCESS;
    }

    auto take_packets(std::vector<Packet>& out) -> void
    {
        const auto lock = std::lock_guard{_packet_mutex};
        for (auto& p : _packets) out.push_back(std::move(p));
        _packets.clear();
    }

    std::atomic<double> sample_rate{48000};
    std::atomic<int32_t> accepted_latency{};
    std::atomic<int32_t> requested_latency{};
    std::atomic<bool> latency_requested{};

private:
    Violations& _violations;
    std::mutex _packet_mutex{};
    std::deque<Packet> _packets{};
};

// Set-value requests are echoed back by the host as UpdateParameterNormalizedValue, on main.
class Fake_automation : public AAX_IACFAutomationDelegate {
public:

    ACFMETHOD(QueryInterface)(const acfIID&, void** out) override { *out = nullptr; return ACF_E_NOINTERFACE; }
    TINY_HOST_NO_REFCOUNT

    AAX_Result RegisterParameter(AAX_CParamID) override { return AAX_SUCCESS; }
    AAX_Result UnregisterParameter(AAX_CParamID) override { return AAX_SUCCESS; }
    AAX_Result PostSetValueRequest(AAX_CParamID id, double value) const override
    {
        const auto lock = std::lock_guard{_mutex};
        _requests.push_back({id, value});
        return AAX_SUCCESS;
    }
    AAX_Result PostCurrentValue(AAX_CParamID, double) const override { return AAX_SUCCESS; }
    AAX_Result PostTouchRequest(AAX_CParamID) override { return AAX_SUCCESS; }
    AAX_Result PostReleaseRequest(AAX_CParamID) override { return AAX_SUCCESS; }
    AAX_Result GetTouchState(AAX_CParamID, AAX_CBoolean* out) override { *out = false; return AAX_SUCCESS; }

    struct Request { std::string id{}; double value{}; };
    auto take(std::vector<Request>& out) -> void
    {
        const auto lock = std::lock_guard{_mutex};
        for (auto& r : _requests) out.push_back(std::move(r));
        _requests.clear();
    }

private:
    mutable std::mutex _mutex{};
    mutable std::vector<Request> _requests{};
};

// What every component's Initialize receives: hands out the controller, the automation
// delegate and the data model's own interface, as Pro Tools does.
class Host_unknown : public IACFUnknown {
public:

    Host_unknown(Fake_controller& controller, Fake_automation& automation) : _controller{controller}, _automation{automation} {}

    ACFMETHOD(QueryInterface)(const acfIID& iid, void** out) override
    {
        *out = nullptr;
        if (iid == IID_IAAXControllerV1) { *out = static_cast<AAX_IACFController*>(&_controller); return ACF_OK; }
        if (iid == IID_IAAXAutomationDelegateV1) { *out = static_cast<AAX_IACFAutomationDelegate*>(&_automation); return ACF_OK; }
        if (iid == IID_IAAXEffectParametersV1 && params != nullptr) return static_cast<AAX_IACFEffectParameters*>(params)->QueryInterface(iid, out);
        return ACF_E_NOINTERFACE;
    }
    TINY_HOST_NO_REFCOUNT

    AAX_IEffectParameters* params{};

private:
    Fake_controller& _controller;
    Fake_automation& _automation;
};

class Fake_view_container : public AAX_IACFViewContainer {
public:

    explicit Fake_view_container(void* ns_view) : _view{ns_view} {}

    ACFMETHOD(QueryInterface)(const acfIID& iid, void** out) override
    {
        *out = nullptr;
        if (iid == IID_IAAXViewContainerV1) { *out = static_cast<AAX_IACFViewContainer*>(this); return ACF_OK; }
        return ACF_E_NOINTERFACE;
    }
    TINY_HOST_NO_REFCOUNT

    int32_t GetType() override { return AAX_eViewContainer_Type_NSView; }
    void* GetPtr() override { return _view; }
    AAX_Result GetModifiers(uint32_t* out) override { *out = 0; return AAX_SUCCESS; }
    AAX_Result SetViewSize(AAX_Point&) override { return AAX_SUCCESS; }
    AAX_Result HandleParameterMouseDown(AAX_CParamID, uint32_t) override { return AAX_ERROR_UNIMPLEMENTED; } // Not handled: the plug-in keeps the gesture.
    AAX_Result HandleParameterMouseDrag(AAX_CParamID, uint32_t) override { return AAX_ERROR_UNIMPLEMENTED; }
    AAX_Result HandleParameterMouseUp(AAX_CParamID, uint32_t) override { return AAX_ERROR_UNIMPLEMENTED; }

private:
    void* _view{};
};

// MARK: - private data

// The algorithm's private data blocks, and the host's copies across them. Pro Tools copies
// bytes; here each copy is made of acquire loads and release stores, which is the ordering a
// host has to provide for the rings to work at all. The seqlocked stores (blocks, the state
// outbox) are read torn by design and the reader rejects the copy, so those slot reads are
// hidden from TSan: that race is the protocol, not a bug.
class Private_data : public AAX_IPrivateDataAccess {
public:

    struct Block {
        AAX_CFieldIndex field{};
        unsigned char* data{};
        uint32_t size{};
        uint32_t seqlock_from{UINT32_MAX}; // Reads at or past this offset are torn by design.
    };

    Private_data() = default;
    ~Private_data() override { for (auto& b : _blocks) std::free(b.data); }
    Private_data(const Private_data&) = delete;
    auto operator=(const Private_data&) -> Private_data& = delete;

    template<typename T>
    auto add(AAX_CFieldIndex field, uint32_t seqlock_from = UINT32_MAX) -> T*
    {
        const auto size = (sizeof(T) + 63) / 64 * 64;
        auto* data = static_cast<unsigned char*>(std::aligned_alloc(64, size));
        std::memset(data, 0, size);
        _blocks.push_back({field, data, static_cast<uint32_t>(sizeof(T)), seqlock_from});
        return reinterpret_cast<T*>(data);
    }

    auto blocks() -> std::vector<Block>& { return _blocks; }

    // Traffic, for the chaos report: proof the channels actually carried something.
    std::atomic<long> reads{}, writes{}, state_edits{}, seqlock_reads{};

    AAX_Result ReadPortDirect(AAX_CFieldIndex field, const uint32_t offset, const uint32_t size, void* out) override
    {
        const auto* b = find(field, offset, size);
        if (b == nullptr) return AAX_ERROR_INVALID_FIELD_INDEX;
        const auto* src = b->data + offset;
        auto* dst = static_cast<unsigned char*>(out);
        reads.fetch_add(1, std::memory_order_relaxed);
        if (offset >= b->seqlock_from) {
            seqlock_reads.fetch_add(1, std::memory_order_relaxed);
#if TINY_HOST_TSAN
            AnnotateIgnoreReadsBegin(__FILE__, __LINE__);
#endif
            std::memcpy(dst, src, size);
#if TINY_HOST_TSAN
            AnnotateIgnoreReadsEnd(__FILE__, __LINE__);
#endif
            return AAX_SUCCESS;
        }
        for (auto i = uint32_t{}; i < size;) {
            if ((reinterpret_cast<uintptr_t>(src + i) % 8) == 0 && size - i >= 8) {
                const auto v = __atomic_load_n(reinterpret_cast<const uint64_t*>(src + i), __ATOMIC_ACQUIRE);
                std::memcpy(dst + i, &v, 8);
                i += 8;
            }
            else {
                dst[i] = __atomic_load_n(src + i, __ATOMIC_ACQUIRE);
                i += 1;
            }
        }
        return AAX_SUCCESS;
    }

    AAX_Result WritePortDirect(AAX_CFieldIndex field, const uint32_t offset, const uint32_t size, const void* in) override
    {
        auto* b = find(field, offset, size);
        if (b == nullptr) return AAX_ERROR_INVALID_FIELD_INDEX;
        writes.fetch_add(1, std::memory_order_relaxed);
#if TINY_HAS_STATE
        if (field == aax::field_state_inbox && offset == aax::State_inbox::offset_posted) state_edits.fetch_add(1, std::memory_order_relaxed);
#endif
        auto* dst = b->data + offset;
        const auto* src = static_cast<const unsigned char*>(in);
        for (auto i = uint32_t{}; i < size;) {
            if ((reinterpret_cast<uintptr_t>(dst + i) % 8) == 0 && size - i >= 8) {
                auto v = uint64_t{};
                std::memcpy(&v, src + i, 8);
                __atomic_store_n(reinterpret_cast<uint64_t*>(dst + i), v, __ATOMIC_RELEASE);
                i += 8;
            }
            else {
                __atomic_store_n(dst + i, src[i], __ATOMIC_RELEASE);
                i += 1;
            }
        }
        return AAX_SUCCESS;
    }

private:
    std::vector<Block> _blocks{};

    auto find(AAX_CFieldIndex field, uint32_t offset, uint32_t size) -> Block*
    {
        for (auto& b : _blocks) {
            if (b.field == field) return uint64_t{offset} + size <= b.size ? &b : nullptr;
        }
        return nullptr;
    }
};

// MARK: - instance

constexpr auto chunk_id = tiny::State_rules::Aax::chunk_id;
constexpr auto max_frames = 1024;

using Chunk = std::vector<unsigned char>;

class Instance {
public:

    Instance(Violations& violations)
        : controller{violations}, _violations{violations}
    {
        params = static_cast<aax::Parameters*>(aax::Parameters::Create());
        params->AddRef();
        _host.params = params;
        expect_true(params->Initialize(&_host) == AAX_SUCCESS, "Parameters::Initialize failed");

        direct = static_cast<aax::Direct_data*>(aax::Direct_data::Create());
        direct->AddRef();
        expect_true(direct->Initialize(&_host) == AAX_SUCCESS, "Direct_data::Initialize failed");

        _build_context();
        add_instance();
        service();
    }

    ~Instance()
    {
        stop_audio();
        stop_direct_data();
        aax::alg_init(&_ctx, AAX_eComponentInstanceInitAction_RemovingInstance);
        direct->Uninitialize();
        direct->Release();
        params->Uninitialize();
        params->Release();
    }

    Instance(const Instance&) = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    // What the host does to bring an algorithm instance up: fill every private data block, then init.
    auto add_instance() -> void
    {
        for (auto& b : _private.blocks()) {
            expect_true(params->ResetFieldData(b.field, b.data, b.size) == AAX_SUCCESS, std::format("ResetFieldData({}) failed", b.field));
        }
        aax::alg_init(&_ctx, AAX_eComponentInstanceInitAction_AddingNewInstance);
    }

    // A reset, as at each edge of a bounce: the reset snapshot is refreshed, the instance reinitialised.
    auto reset_instance() -> void
    {
        expect_true(params->ResetFieldData(aax::field_reset_state, _reset_state, sizeof(aax::Reset_state)) == AAX_SUCCESS, "ResetFieldData(reset) failed");
        aax::alg_init(&_ctx, AAX_eComponentInstanceInitAction_ResetInstance);
    }

    auto notify(AAX_CTypeID type, const void* data = nullptr, uint32_t size = 0) -> void
    {
        params->NotificationReceived(type, data, size);
        if (gui != nullptr) gui->NotificationReceived(type, data, size);
    }

    // What the host's main thread does between calls: echo set-value requests, answer latency
    // requests, generate coefficients, let the timer run.
    auto service() -> void
    {
        _requests.clear();
        _automation.take(_requests);
        for (const auto& r : _requests) {
            params->UpdateParameterNormalizedValue(r.id.c_str(), r.value, AAX_eUpdateSource_Unspecified);
            if (gui != nullptr) gui->ParameterUpdated(r.id.c_str());
        }
        if (controller.latency_requested.exchange(false)) {
            controller.accepted_latency.store(controller.requested_latency.load());
            notify(AAX_eNotificationEvent_SignalLatencyChanged);
        }
        params->GenerateCoefficients();
        params->TimerWakeup();
        if (gui != nullptr) gui->TimerWakeup();
        tiny::hosts::pump_main(200us);
    }

    // Automation playback, or a control surface: the host sets a parameter directly.
    auto automate(Random& random) -> void
    {
        auto count = int32_t{};
        params->GetNumberOfParameters(&count);
        if (count == 0) return;
        auto id = AAX_CString{};
        params->GetParameterIDFromIndex(static_cast<int32_t>(random.below(static_cast<uint32_t>(count))), &id);
        params->UpdateParameterNormalizedValue(id.CString(), random.real(0, 1), AAX_eUpdateSource_Unspecified);
        if (gui != nullptr) gui->ParameterUpdated(id.CString());
    }

    // Display strings, the way Pro Tools' mixer and automation lanes ask.
    auto query_params() -> void
    {
        auto count = int32_t{};
        params->GetNumberOfParameters(&count);
        for (auto i = 0; i < count; ++i) {
            auto id = AAX_CString{};
            params->GetParameterIDFromIndex(i, &id);
            auto value = double{};
            params->GetParameterNormalizedValue(id.CString(), &value);
            auto text = AAX_CString{};
            params->GetParameterValueString(id.CString(), &text, 32);
        }
    }

    auto save_chunk() -> Chunk
    {
        auto size = uint32_t{};
        expect_true(params->GetChunkSize(chunk_id, &size) == AAX_SUCCESS, "GetChunkSize failed");
        auto bytes = Chunk(sizeof(AAX_SPlugInChunkHeader) + size);
        auto* chunk = reinterpret_cast<AAX_SPlugInChunk*>(bytes.data());
        chunk->fSize = static_cast<int32_t>(size);
        chunk->fChunkID = chunk_id;
        expect_true(params->GetChunk(chunk_id, chunk) == AAX_SUCCESS, "GetChunk failed");
        return bytes;
    }

    auto load_chunk(const Chunk& bytes) -> bool
    {
        return params->SetChunk(chunk_id, reinterpret_cast<const AAX_SPlugInChunk*>(bytes.data())) == AAX_SUCCESS;
    }

    auto compare_chunk(const Chunk& bytes) -> bool
    {
        auto equal = AAX_CBoolean{};
        expect_true(params->CompareActiveChunk(reinterpret_cast<const AAX_SPlugInChunk*>(bytes.data()), &equal) == AAX_SUCCESS, "CompareActiveChunk failed");
        return equal != 0;
    }

    // The Direct Data timer: roughly every 30 ms in Pro Tools, never regular; faster here.
    auto start_direct_data(uint32_t seed) -> void
    {
        stop_direct_data();
        _dd_running = true;
        _dd_thread = std::thread{[this, seed] {
            auto random = Random{seed};
            while (_dd_running.load(std::memory_order_relaxed)) {
                direct->TimerWakeup_PrivateDataAccess(&_private);
                std::this_thread::sleep_for(std::chrono::microseconds{200 + random.below(3000)});
            }
        }};
    }

    auto stop_direct_data() -> void
    {
        _dd_running = false;
        if (_dd_thread.joinable()) _dd_thread.join();
    }

    // MARK: audio

    struct Result {
        long blocks{}, non_finite{}, allocations{};
    };

    auto start_audio(uint32_t seed) -> void
    {
        _audio_running = true;
        _audio_result = {};
        _audio_thread = std::thread{[this, seed] {
            auto random = Random{seed};
            while (_audio_running.load(std::memory_order_relaxed)) render(random);
        }};
    }

    auto stop_audio() -> Result
    {
        _audio_running = false;
        if (_audio_thread.joinable()) _audio_thread.join();
        return _audio_result;
    }

    Fake_controller controller;
    aax::Parameters* params{};
    aax::Direct_data* direct{};
    AAX_IEffectGUI* gui{};
    Host_unknown* host() { return &_host; }
    auto traffic() -> std::string
    {
        return std::format("{} packets, {} port reads ({} seqlocked), {} port writes, {} state edits",
                           _packets_delivered, _private.reads.load(), _private.seqlock_reads.load(), _private.writes.load(), _private.state_edits.load());
    }

private:
    Violations& _violations;
    Fake_automation _automation{};
    Host_unknown _host{controller, _automation};
    std::vector<Fake_automation::Request> _requests{};

    Private_data _private{};
    aax::Alg_context _ctx{};
    aax::Reset_state* _reset_state{};
    aax::Runtime_packet _runtime_port{};
    std::array<aax::Coef_segment, aax::num_segments> _coef_ports{};
    float _sample_rate{48000};
    int32_t _num_frames{};
    int32_t _sidechain_index{2};
    std::array<std::array<float, max_frames>, 3> _in{};
    std::array<std::array<float, max_frames>, 2> _out{};
    std::array<float*, 3> _in_ptrs{};
    std::array<float*, 2> _out_ptrs{};
    std::vector<Packet> _taken{};
    long _packets_delivered{};

    std::atomic<bool> _dd_running{};
    std::thread _dd_thread{};
    std::atomic<bool> _audio_running{};
    std::thread _audio_thread{};
    Result _audio_result{};

    auto _build_context() -> void
    {
        for (auto c = size_t{}; c < _in.size(); ++c) _in_ptrs[c] = _in[c].data();
        for (auto c = size_t{}; c < _out.size(); ++c) _out_ptrs[c] = _out[c].data();
        _ctx.audio_in = _in_ptrs.data();
        _ctx.audio_out = _out_ptrs.data();
        _ctx.num_frames = &_num_frames;
        _ctx.sample_rate = &_sample_rate;
#if TINY_WANTS_SIDECHAIN
        _ctx.sidechain_index = &_sidechain_index;
#endif
        _ctx.runtime = &_runtime_port;
        _runtime_port.delay_comp = 1;
        for (auto s = size_t{}; s < aax::num_segments; ++s) _ctx.coefs[s] = &_coef_ports[s];

        _reset_state = _private.add<aax::Reset_state>(aax::field_reset_state);
        _ctx.reset_state = _reset_state;
        _ctx.state = _private.add<aax::Alg_state>(aax::field_state);
        _ctx.returns = _private.add<aax::Return_ring>(aax::field_returns);
        _ctx.inbound = _private.add<aax::Inbound_ring>(aax::field_inbound);
#if TINY_HAS_BLOCKS
        tiny::blocks::for_each_address<tiny::models::Resolved::Blocks>([&](auto i) {
            using Store = aax::Block_store_at<decltype(i)::value>;
            _ctx.blocks[i] = _private.add<Store>(aax::block_field(i), Store::offset_slots);
        });
#endif
#if TINY_HAS_STATE
        _ctx.state_inbox = _private.add<aax::State_inbox>(aax::field_state_inbox);
        _ctx.state_outbox = _private.add<aax::State_outbox>(aax::field_state_outbox, aax::State_outbox::offset_slots);
#endif
    }

    // The host lands posted packets at the next render; timestamps are not modelled.
    auto _deliver_packets() -> void
    {
        _taken.clear();
        controller.take_packets(_taken);
        _packets_delivered += static_cast<long>(_taken.size());
        for (const auto& p : _taken) {
            if (p.field == aax::field_runtime && p.bytes.size() == sizeof(_runtime_port)) {
                std::memcpy(&_runtime_port, p.bytes.data(), sizeof(_runtime_port));
                continue;
            }
            for (auto s = size_t{}; s < aax::num_segments; ++s) {
                if (p.field == aax::coef_field(s) && p.bytes.size() == sizeof(aax::Coef_segment)) {
                    std::memcpy(&_coef_ports[s], p.bytes.data(), sizeof(aax::Coef_segment));
                }
            }
        }
    }

    auto render(Random& random) -> void
    {
        _deliver_packets(); // Before the trap: the host's own copies allocate.
        const auto frames = static_cast<int32_t>(1 + random.below(max_frames));
        _num_frames = frames;
        for (auto& ch : _in) for (auto f = 0; f < frames; ++f) ch[static_cast<size_t>(f)] = static_cast<float>(random.real(-0.5, 0.5));

        auto instances = std::array<aax::Alg_context*, 1>{&_ctx};
        const auto before = tiny::hosts::trapped_allocations();
        {
            const auto scope = tiny::hosts::Trap_scope{};
            aax::alg_render_stereo(instances.data(), instances.data() + 1);
        }
        _audio_result.allocations += tiny::hosts::trapped_allocations() - before;
        ++_audio_result.blocks;
        for (const auto& ch : _out) {
            if (!std::all_of(ch.begin(), ch.begin() + frames, [](float x) { return std::isfinite(x); })) { ++_audio_result.non_finite; break; }
        }
        std::this_thread::sleep_for(std::chrono::microseconds{random.below(500)});
    }
};

// MARK: - editor

// The data model's GUI, in a window for as long as this lives.
class Editor_session {
public:
    Editor_session(Instance& instance, tiny::hosts::Window& window) : _instance{instance}, _window{window}
    {
        _gui = aax::Gui::Create();
        _gui->AddRef();
        expect_true(_gui->Initialize(instance.host()) == AAX_SUCCESS, "Gui::Initialize failed");
        _container = std::make_unique<Fake_view_container>(window.content());
        _gui->SetViewContainer(_container.get());
        _ns_view = window.editor_view();
        instance.gui = _gui;
    }
    ~Editor_session()
    {
        _instance.gui = nullptr;
        _gui->SetViewContainer(nullptr);
        _gui->Uninitialize();
        _gui->Release();
    }
    Editor_session(const Editor_session&) = delete;
    auto operator=(const Editor_session&) -> Editor_session& = delete;

    auto use(Random& random) -> void
    {
        if (_ns_view) tiny::hosts::send_input(_ns_view, random, 3);
        tiny::hosts::pump_main(8ms);
    }

private:
    Instance& _instance;
    tiny::hosts::Window& _window;
    AAX_IEffectGUI* _gui{};
    std::unique_ptr<Fake_view_container> _container{};
    void* _ns_view{};
};

// MARK: - chunk threads

// Pro Tools can call the chunk methods from threads other than main, and more than one at a
// time (autosave, the compare light): AAX_Properties.h makes that the default unless a plug-in
// sets AAX_eProperty_RequiresChunkCallsOnMainThread. A pair that fails is the documented
// outcome of a collision; anything the sanitizers see is not.
class Chunk_threads {
public:
    Chunk_threads(Instance& instance, int count, uint32_t seed)
    {
        _running = true;
        for (auto t = 0; t < count; ++t) {
            _threads.emplace_back([this, &instance, seed, t] {
                auto random = Random{seed + static_cast<uint32_t>(t)};
                auto last = Chunk{};
                while (_running.load(std::memory_order_relaxed)) {
                    auto size = uint32_t{};
                    if (instance.params->GetChunkSize(chunk_id, &size) != AAX_SUCCESS) { ++failed; continue; }
                    auto bytes = Chunk(sizeof(AAX_SPlugInChunkHeader) + size);
                    auto* chunk = reinterpret_cast<AAX_SPlugInChunk*>(bytes.data());
                    chunk->fSize = static_cast<int32_t>(size);
                    chunk->fChunkID = chunk_id;
                    if (instance.params->GetChunk(chunk_id, chunk) == AAX_SUCCESS) { ++pairs; last = std::move(bytes); }
                    else ++failed;
                    if (!last.empty() && random.chance(0.3)) {
                        auto equal = AAX_CBoolean{};
                        instance.params->CompareActiveChunk(reinterpret_cast<const AAX_SPlugInChunk*>(last.data()), &equal);
                    }
                    if (!last.empty() && random.chance(0.05)) {
                        instance.params->SetChunk(chunk_id, reinterpret_cast<const AAX_SPlugInChunk*>(last.data()));
                        ++loads;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds{random.below(2000)});
                }
            });
        }
    }
    ~Chunk_threads()
    {
        _running = false;
        for (auto& t : _threads) t.join();
    }
    Chunk_threads(const Chunk_threads&) = delete;
    auto operator=(const Chunk_threads&) -> Chunk_threads& = delete;

    std::atomic<long> pairs{}, failed{}, loads{};

private:
    std::atomic<bool> _running{};
    std::vector<std::thread> _threads{};
};

// MARK: - checks

auto check_clean(Violations& violations, const char* when) -> void
{
    const auto [count, list] = violations.take();
    auto text = std::string{};
    for (auto k = size_t{}; k < std::min<size_t>(list.size(), 5); ++k) text += "\n    " + list[k];
    expect_true(count == 0, std::format("{}: {} contract violation(s):{}", when, count, text));
}

auto check_result(const Instance::Result& r, const char* when) -> void
{
    expect_true(r.non_finite == 0, std::format("{}: {} blocks wrote NaN or inf", when, r.non_finite));
    expect_true(r.allocations == 0, std::format("{}: {} allocations on the audio thread in {} blocks", when, r.allocations, r.blocks));
}

auto chunk_data(const Chunk& c) -> std::vector<unsigned char>
{
    return {c.begin() + static_cast<std::ptrdiff_t>(sizeof(AAX_SPlugInChunkHeader)), c.end()};
}

// MARK: - scenarios

auto add_scenarios() -> void
{
    Tests::add("lifecycle: create and destroy, never rendered", [] {
        auto violations = Violations{};
        for (auto n = 0; n < 5; ++n) { auto instance = Instance{violations}; }
        tiny::hosts::pump_main(10ms);
        check_clean(violations, "create/destroy");
    });

    Tests::add("lifecycle: render with Direct Data running, across resets and offline edges", [] {
        auto violations = Violations{};
        {
            auto instance = Instance{violations};
            auto random = Random{g_options.seed};
            instance.start_direct_data(g_options.seed);
            for (auto cycle = 0; cycle < 8; ++cycle) {
                const auto offline = random.chance(0.3);
                instance.notify(offline ? AAX_eNotificationEvent_EnteringOfflineMode : AAX_eNotificationEvent_ExitingOfflineMode);
                instance.reset_instance();
                instance.start_audio(g_options.seed + static_cast<uint32_t>(cycle));
                for (auto t = 0; t < 30; ++t) {
                    if (random.chance(0.3)) instance.automate(random);
                    instance.service();
                }
                check_result(instance.stop_audio(), "rendering");
                instance.service();
            }
        }
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "lifecycle");
    });

    Tests::add("chunk: save, load, save is identical and compares equal, idle and rendering", [] {
        auto violations = Violations{};
        {
            auto instance = Instance{violations};
            auto random = Random{g_options.seed};
            for (auto k = 0; k < 20; ++k) instance.automate(random);
            instance.service();
            const auto saved = instance.save_chunk();
            expect_true(instance.compare_chunk(saved), "a chunk just saved must compare equal");
            expect_true(instance.load_chunk(saved), "load failed");
            instance.service();
            expect_true(chunk_data(instance.save_chunk()) == chunk_data(saved), "chunk changed across a load");

            instance.start_direct_data(g_options.seed);
            instance.start_audio(g_options.seed);
            for (auto n = 0; n < 20; ++n) {
                expect_true(instance.load_chunk(saved), "load while rendering failed");
                instance.service();
            }
            check_result(instance.stop_audio(), "rendering");
            instance.stop_direct_data();
            instance.service();
            expect_true(chunk_data(instance.save_chunk()) == chunk_data(saved), "chunk changed across loads while rendering");
            expect_true(instance.compare_chunk(saved), "chunk no longer compares equal after loads while rendering");
        }
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "chunk");
    });

    Tests::add("chunk: truncated and corrupted chunks never crash, and a good one restores", [] {
        auto violations = Violations{};
        {
            auto instance = Instance{violations};
            auto random = Random{g_options.seed};
            const auto saved = instance.save_chunk();
            const auto header = sizeof(AAX_SPlugInChunkHeader);
            for (auto size = size_t{}; size < saved.size() - header; size += 1 + (saved.size() - header) / 64) {
                auto cut = Chunk(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(header + size));
                reinterpret_cast<AAX_SPlugInChunk*>(cut.data())->fSize = static_cast<int32_t>(size);
                (void)instance.load_chunk(cut);
                instance.service();
            }
            for (auto n = 0; n < 200; ++n) {
                auto bad = saved;
                for (auto flips = 1 + random.below(4); flips > 0; --flips) {
                    bad[header + random.below(static_cast<uint32_t>(saved.size() - header))] ^= static_cast<unsigned char>(1 + random.below(255));
                }
                (void)instance.load_chunk(bad);
                instance.service();
            }
            expect_true(instance.load_chunk(saved), "a good chunk after bad ones must load");
            instance.service();
            expect_true(chunk_data(instance.save_chunk()) == chunk_data(saved), "a good chunk after bad ones must restore exactly");
        }
        tiny::hosts::pump_main(20ms);
        (void)violations.take();
    });

    Tests::add("editor: open, use and close, idle and rendering, with loads while open", [] {
        auto violations = Violations{};
        {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{violations};
            auto random = Random{g_options.seed};
            const auto saved = instance.save_chunk();
            instance.start_direct_data(g_options.seed);
            for (auto cycle = 0; cycle < 6; ++cycle) {
                const auto rendering = cycle % 2 == 1;
                if (rendering) instance.start_audio(g_options.seed + static_cast<uint32_t>(cycle));
                {
                    auto editor = Editor_session{instance, window};
                    for (auto k = 0; k < 12; ++k) {
                        editor.use(random);
                        if (k % 4 == 3) (void)instance.load_chunk(saved); // A host load reaches an open editor.
                        if (k % 3 == 2) (void)instance.save_chunk();
                        instance.service();
                    }
                }
                if (rendering) check_result(instance.stop_audio(), "rendering");
                instance.service();
            }
        }
        tiny::hosts::pump_main(50ms);
        check_clean(violations, "editor");
    });

    Tests::add("chunk: a load off main is the state at once, and main applies it at the next wakeup", [] {
        auto violations = Violations{};
        {
            auto instance = Instance{violations};
            auto random = Random{g_options.seed};
            for (auto k = 0; k < 20; ++k) instance.automate(random);
            instance.service();
            const auto a = instance.save_chunk();
            for (auto k = 0; k < 20; ++k) instance.automate(random);
            instance.service();
            const auto b = instance.save_chunk();
            expect_true(chunk_data(a) != chunk_data(b), "the two states should differ");

            auto loaded = false;
            std::thread{[&] { loaded = instance.load_chunk(a); }}.join();
            expect_true(loaded, "load off main failed");
            expect_true(chunk_data(instance.save_chunk()) == chunk_data(a), "a save straight after a load off main must return what was loaded");
            expect_true(instance.compare_chunk(a), "a load off main must compare equal at once");

            auto from_other = Chunk{};
            std::thread{[&] { from_other = instance.save_chunk(); }}.join();
            expect_true(chunk_data(from_other) == chunk_data(a), "a save off main must see the load too");

            instance.service(); // TimerWakeup applies it,
            instance.service(); // and the host echoes the parameters it set.
            expect_true(chunk_data(instance.save_chunk()) == chunk_data(a), "the applied load must save as loaded");
            expect_true(!instance.compare_chunk(b), "the old state must no longer compare equal");
        }
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "off-main load");
    });

    Tests::add("chaos: rendering and Direct Data while main and three other threads load, save and compare", [] {
        auto violations = Violations{};
        {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{violations};
            auto random = Random{g_options.seed};
            auto editor = std::unique_ptr<Editor_session>{};
            auto chunks = std::vector<Chunk>{instance.save_chunk()};
            instance.start_direct_data(g_options.seed);
            instance.start_audio(g_options.seed);
            auto chunk_threads = std::make_unique<Chunk_threads>(instance, 3, g_options.seed);
            auto blocks = long{};
            auto loads = 0, resets = 0;

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(g_options.chaos_seconds);
            while (std::chrono::steady_clock::now() < deadline) {
                switch (random.below(10)) {
                    case 0: chunks.push_back(instance.save_chunk()); break;
                    case 1: (void)instance.load_chunk(chunks[random.below(static_cast<uint32_t>(chunks.size()))]); ++loads; break;
                    case 2: (void)instance.compare_chunk(chunks[random.below(static_cast<uint32_t>(chunks.size()))]); break;
                    case 3: instance.query_params(); break;
                    case 4: for (auto k = random.below(8); k > 0; --k) instance.automate(random); break;
                    case 5:
                        if (random.chance(0.1)) {
                            const auto r = instance.stop_audio();
                            check_result(r, "rendering");
                            blocks += r.blocks;
                            instance.notify(random.chance(0.5) ? AAX_eNotificationEvent_EnteringOfflineMode : AAX_eNotificationEvent_ExitingOfflineMode);
                            instance.reset_instance();
                            ++resets;
                            instance.start_audio(g_options.seed + static_cast<uint32_t>(resets));
                        }
                        break;
                    case 6:
                        if (random.chance(0.15)) {
                            if (editor) editor.reset();
                            else editor = std::make_unique<Editor_session>(instance, window);
                        }
                        else if (editor) editor->use(random);
                        break;
                    default: instance.service(); break;
                }
            }
            const auto pairs = chunk_threads->pairs.load(), failed = chunk_threads->failed.load(), off_main_loads = chunk_threads->loads.load();
            chunk_threads.reset();
            expect_true(pairs > 0 && failed == 0, std::format("chunk threads: {} pairs, {} failed", pairs, failed));
            editor.reset();
            const auto last = instance.stop_audio();
            check_result(last, "rendering");
            blocks += last.blocks;
            instance.stop_direct_data();
            instance.service();
            std::printf("  chaos: %ld blocks, %d loads (%ld off main), %ld off-main saves, %d resets; %s\n", blocks, loads, off_main_loads, pairs, resets, instance.traffic().c_str());
        }
        tiny::hosts::pump_main(50ms);
        check_clean(violations, "chaos");
    });

    Tests::add("teardown: destroyed straight after a burst of activity, with nothing drained", [] {
        auto violations = Violations{};
        for (auto n = 0; n < 10; ++n) {
            auto instance = Instance{violations};
            auto random = Random{g_options.seed + static_cast<uint32_t>(n)};
            const auto saved = instance.save_chunk();
            instance.start_direct_data(g_options.seed + static_cast<uint32_t>(n));
            instance.start_audio(g_options.seed + static_cast<uint32_t>(n));
            for (auto k = 0; k < 5; ++k) { (void)instance.load_chunk(saved); instance.automate(random); }
            check_result(instance.stop_audio(), "rendering");
        } // Destroyed without servicing main or the run loop.
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "teardown");
    });
}

} // namespace

auto main(int argc, char** argv) -> int
{
    g_options = tiny::hosts::parse_options(argc, argv);
    std::printf("aax_host: %s, seed %u\n", tiny::Plug_info::product_name, g_options.seed);
    tiny::hosts::gui_init();
    add_scenarios();
    return audio_bench::Tests::run_all() == 0 ? 0 : 1;
}
