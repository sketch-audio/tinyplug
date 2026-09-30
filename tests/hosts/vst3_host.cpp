// A fake VST3 host. Loads a real .vst3, keeps the processor (IComponent + IAudioProcessor) and the
// controller (IEditController) apart the way a distributable host does, and drives them from a UI
// thread and an audio thread, so the sanitizers can see the wrapper:
//
//     vst3_host <path/to/Plugin.vst3> [--seed N] [--seconds S]
//
// Messages between the two halves go through a proxy that, like the SDK's reference ConnectionProxy,
// only delivers on the UI thread: IConnectionPoint::notify is [UI-thread & Connected]. A send from
// any other thread is dropped and counted as a failure, as are component-handler calls off the UI
// thread.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <audio_bench/audio_bench.hpp>

#include "base/source/fobject.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/processdata.h"

#include "gui.hpp"
#include "support.hpp"
#include "pluginterfaces/gui/iplugview.h"

namespace {

using namespace std::chrono_literals;
using namespace Steinberg;
using namespace Steinberg::Vst;
using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::hosts::Random;

auto g_options = tiny::hosts::Options{};
const auto g_ui_thread = std::this_thread::get_id();

auto on_ui() -> bool { return std::this_thread::get_id() == g_ui_thread; }

// Contract violations the host saw, from any thread.
class Violations {
public:
    auto add(std::string what) -> void
    {
        const auto lock = std::lock_guard{_mutex};
        if (_list.size() < 64) _list.push_back(std::move(what));
        ++_count;
    }
    auto take() -> std::pair<long, std::vector<std::string>>
    {
        const auto lock = std::lock_guard{_mutex};
        return {std::exchange(_count, 0), std::exchange(_list, {})};
    }
private:
    std::mutex _mutex{};
    std::vector<std::string> _list{};
    long _count{};
};

// MARK: - bundle

using Factory_proc = IPluginFactory* (PLUGIN_API*)();
#if defined(__APPLE__)
using Bundle_entry = bool (*)(CFBundleRef);
using Bundle_exit = bool (*)();
#else
using Bundle_entry = bool (PLUGIN_API*)(); // InitDll
using Bundle_exit = bool (PLUGIN_API*)();  // ExitDll
constexpr auto entry_name = "InitDll";
constexpr auto exit_name = "ExitDll";
#endif

struct Bundle {
    tiny::hosts::Library library;
#if defined(__APPLE__)
    CFBundleRef cf_bundle{};
#endif
    IPtr<IPluginFactory> factory{};
    FUID processor_cid{};

    explicit Bundle(const std::string& path) : library{path}
    {
#if defined(__APPLE__)
        auto* url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), true);
        cf_bundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        auto entry = reinterpret_cast<Bundle_entry>(library.symbol("bundleEntry"));
        if (!entry || !entry(cf_bundle)) throw std::runtime_error("bundleEntry missing or failed");
#else
        // Optional on Windows, as in the SDK's own loader.
        if (auto entry = reinterpret_cast<Bundle_entry>(library.symbol(entry_name)); entry && !entry()) throw std::runtime_error("InitDll failed");
#endif

        auto get_factory = reinterpret_cast<Factory_proc>(library.symbol("GetPluginFactory"));
        if (!get_factory) throw std::runtime_error("GetPluginFactory missing");
        factory = owned(get_factory());

        for (auto i = 0; i < factory->countClasses(); ++i) {
            auto info = PClassInfo{};
            factory->getClassInfo(i, &info);
            if (std::strcmp(info.category, kVstAudioEffectClass) == 0) processor_cid = FUID::fromTUID(info.cid);
        }
        if (!processor_cid.isValid()) throw std::runtime_error("no audio effect class");
    }

    ~Bundle()
    {
        factory = nullptr;
#if defined(__APPLE__)
        if (auto exit = reinterpret_cast<Bundle_exit>(library.symbol("bundleExit"))) exit();
        if (cf_bundle) CFRelease(cf_bundle);
#else
        if (auto exit = reinterpret_cast<Bundle_exit>(library.symbol(exit_name))) exit();
#endif
    }
};

// MARK: - host objects

// Delivers on the UI thread only, like the reference ConnectionProxy; anything else is a violation.
class Strict_proxy : public FObject, public IConnectionPoint {
public:
    Strict_proxy(IConnectionPoint* src, Violations& violations, const char* name)
        : _src{src}, _violations{violations}, _name{name} {}

    tresult PLUGIN_API connect(IConnectionPoint* other) override
    {
        _dst = other;
        return _src->connect(this);
    }
    tresult PLUGIN_API disconnect(IConnectionPoint* other) override
    {
        if (other != _dst) return kInvalidArgument;
        _src->disconnect(this);
        _dst = nullptr;
        return kResultTrue;
    }
    tresult PLUGIN_API notify(IMessage* message) override
    {
        if (!on_ui()) {
            _violations.add(std::format("{} sent '{}' off the UI thread (dropped)", _name, message ? message->getMessageID() : "?"));
            return kResultFalse;
        }
        return _dst ? _dst->notify(message) : kResultFalse;
    }

    OBJ_METHODS(Strict_proxy, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IConnectionPoint)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)

private:
    IConnectionPoint* _src{};
    IConnectionPoint* _dst{};
    Violations& _violations;
    const char* _name{};
};

class Handler : public FObject, public IComponentHandler {
public:
    explicit Handler(Violations& violations) : _violations{violations} {}

    std::atomic<bool> latency_changed{}, values_changed{}, reload{};
    std::atomic<int> restarts{};

    tresult PLUGIN_API beginEdit(ParamID) override { check("beginEdit"); return kResultOk; }
    tresult PLUGIN_API performEdit(ParamID, ParamValue) override { check("performEdit"); return kResultOk; }
    tresult PLUGIN_API endEdit(ParamID) override { check("endEdit"); return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override
    {
        check("restartComponent");
        restarts.fetch_add(1);
        if (flags & kLatencyChanged) latency_changed = true;
        if (flags & kParamValuesChanged) values_changed = true;
        if (flags & kReloadComponent) reload = true;
        return kResultOk;
    }

    OBJ_METHODS(Handler, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IComponentHandler)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)

private:
    Violations& _violations;
    auto check(const char* what) -> void
    {
        if (!on_ui()) _violations.add(std::format("IComponentHandler::{} called off the UI thread", what));
    }
};

// A memory stream that reads and writes at most `chunk_max` bytes per call, as some hosts do.
class Trickle_stream : public MemoryStream {
public:
    Trickle_stream(Random* random, uint32_t chunk_max) : _random{random}, _chunk_max{chunk_max} {}
    Trickle_stream(const std::vector<char>& bytes, Random* random, uint32_t chunk_max)
        : _random{random}, _chunk_max{chunk_max}
    {
        int32 written = 0;
        MemoryStream::write(const_cast<char*>(bytes.data()), static_cast<int32>(bytes.size()), &written);
        seek(0, kIBSeekSet, nullptr);
    }
    tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
    {
        return MemoryStream::read(buffer, limit(numBytes), numBytesRead);
    }
    auto bytes() -> std::vector<char>
    {
        return std::vector<char>(getData(), getData() + getSize());
    }
private:
    Random* _random{};
    uint32_t _chunk_max{};
    auto limit(int32 n) -> int32 { return _chunk_max ? std::min<int32>(n, static_cast<int32>(1 + _random->below(_chunk_max))) : n; }
};

// MARK: - instance

struct Output_param { ParamID id{}; ParamValue value{}; };

class Instance {
public:

    Instance(const Bundle& bundle, Violations& violations)
        : _violations{violations}
    {
        host = owned(new HostApplication{});
        auto* raw = static_cast<IComponent*>(nullptr);
        if (bundle.factory->createInstance(bundle.processor_cid.toTUID(), IComponent::iid, reinterpret_cast<void**>(&raw)) != kResultOk || !raw)
            throw std::runtime_error("create component failed");
        component = owned(raw);
        if (component->initialize(host) != kResultOk) throw std::runtime_error("component initialize failed");
        processor = FUnknownPtr<IAudioProcessor>(component);

        TUID controller_cid{}; // An array: `auto` would decay it to a pointer into a temporary.
        if (component->getControllerClassId(controller_cid) != kResultOk) throw std::runtime_error("no controller class");
        auto* raw_ctrl = static_cast<IEditController*>(nullptr);
        if (bundle.factory->createInstance(controller_cid, IEditController::iid, reinterpret_cast<void**>(&raw_ctrl)) != kResultOk || !raw_ctrl)
            throw std::runtime_error("create controller failed");
        controller = owned(raw_ctrl);
        if (controller->initialize(host) != kResultOk) throw std::runtime_error("controller initialize failed");

        handler = owned(new Handler{violations});
        controller->setComponentHandler(handler);

        auto component_cp = FUnknownPtr<IConnectionPoint>(component);
        auto controller_cp = FUnknownPtr<IConnectionPoint>(controller);
        if (component_cp && controller_cp) {
            _to_controller = owned(new Strict_proxy{component_cp, violations, "processor"});
            _to_component = owned(new Strict_proxy{controller_cp, violations, "controller"});
            _to_controller->connect(controller_cp);
            _to_component->connect(component_cp);
        }

        // The host's first sync: the controller learns the processor's state.
        auto state = Trickle_stream{nullptr, 0};
        if (component->getState(&state) == kResultOk) {
            state.seek(0, IBStream::kIBSeekSet, nullptr);
            controller->setComponentState(&state);
        }

        for (auto i = 0; i < controller->getParameterCount(); ++i) {
            auto info = ParameterInfo{};
            if (controller->getParameterInfo(i, info) == kResultOk) params.push_back(info);
        }
        audio_inputs = component->getBusCount(kAudio, kInput);
        audio_outputs = component->getBusCount(kAudio, kOutput);
        event_inputs = component->getBusCount(kEvent, kInput);
        for (auto b = 0; b < audio_inputs; ++b) component->activateBus(kAudio, kInput, b, true);
        for (auto b = 0; b < audio_outputs; ++b) component->activateBus(kAudio, kOutput, b, true);
        for (auto b = 0; b < event_inputs; ++b) component->activateBus(kEvent, kInput, b, true);
    }

    ~Instance()
    {
        if (_to_controller) _to_controller->disconnect(FUnknownPtr<IConnectionPoint>(controller));
        if (_to_component) _to_component->disconnect(FUnknownPtr<IConnectionPoint>(component));
        controller->setComponentHandler(nullptr);
        controller->terminate();
        component->terminate();
        processor = nullptr;
        controller = nullptr;
        component = nullptr;
    }

    Instance(const Instance&) = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    auto setup(double rate, int32 max_block, bool offline) -> void
    {
        auto setup = ProcessSetup{offline ? kOffline : kRealtime, kSample32, max_block, rate};
        expect_true(processor->setupProcessing(setup) == kResultOk, "setupProcessing failed");
        this->max_block = max_block;
        this->offline = offline;
    }

    auto save_component(Random& random, uint32_t chunk = 0) -> std::vector<char>
    {
        auto s = Trickle_stream{&random, chunk};
        expect_true(component->getState(&s) == kResultOk, "component getState failed");
        return s.bytes();
    }
    auto save_controller(Random& random) -> std::vector<char>
    {
        auto s = Trickle_stream{&random, 0};
        expect_true(controller->getState(&s) == kResultOk, "controller getState failed");
        return s.bytes();
    }
    // A host load: the processor's chunk to both halves, then the controller's own.
    auto load(const std::vector<char>& comp, const std::vector<char>& ctrl, Random& random, uint32_t chunk = 0) -> bool
    {
        auto a = Trickle_stream{comp, &random, chunk};
        const auto ok = component->setState(&a) == kResultOk;
        auto b = Trickle_stream{comp, &random, chunk};
        controller->setComponentState(&b);
        auto c = Trickle_stream{ctrl, &random, chunk};
        controller->setState(&c);
        return ok;
    }

    // Output parameter changes (VST3 meters) reach the controller on the UI thread.
    auto post_outputs(const std::vector<Output_param>& outs) -> void
    {
        const auto lock = std::lock_guard{_out_mutex};
        _outputs.insert(_outputs.end(), outs.begin(), outs.end());
    }

    // What a host's UI thread does between calls.
    auto service() -> void
    {
        auto outs = std::vector<Output_param>{};
        {
            const auto lock = std::lock_guard{_out_mutex};
            outs.swap(_outputs);
        }
        for (const auto& o : outs) controller->setParamNormalized(o.id, o.value);
        if (handler->latency_changed.exchange(false)) (void)processor->getLatencySamples(); // The acceptance.
        if (handler->values_changed.exchange(false)) for (const auto& p : params) (void)controller->getParamNormalized(p.id);
        tiny::hosts::pump_main(200us);
    }

    IPtr<HostApplication> host{};
    IPtr<IComponent> component{};
    IPtr<IAudioProcessor> processor{};
    IPtr<IEditController> controller{};
    IPtr<Handler> handler{};
    std::vector<ParameterInfo> params{};
    int32 audio_inputs{}, audio_outputs{}, event_inputs{};
    int32 max_block{512};
    bool offline{};

private:
    Violations& _violations;
    IPtr<Strict_proxy> _to_controller{}, _to_component{};
    std::mutex _out_mutex{};
    std::vector<Output_param> _outputs{};
};

// MARK: - audio engine

class Engine {
public:

    struct Result {
        long blocks{}, errors{}, non_finite{}, allocations{};
    };

    Engine(Instance& instance, uint32_t seed) : _i{instance}, _random{seed}, _inputs{static_cast<int32>(instance.params.size()) + 8}, _outputs{static_cast<int32>(instance.params.size()) + 64}
    {
        _data.prepare(*instance.component, instance.max_block, kSample32);
        _data.processMode = instance.offline ? kOffline : kRealtime;
        _data.inputParameterChanges = &_inputs;
        _data.outputParameterChanges = &_outputs;
        _data.inputEvents = instance.event_inputs > 0 ? &_events : nullptr;
        _collected.reserve(256);
    }

    ~Engine() { stop(); }
    Engine(const Engine&) = delete;
    auto operator=(const Engine&) -> Engine& = delete;

    auto start() -> void
    {
        _running = true;
        _thread = std::thread{[this] {
            _i.processor->setProcessing(true);
            while (_running.load(std::memory_order_relaxed)) run_block();
            _i.processor->setProcessing(false);
        }};
    }

    auto stop() -> Result
    {
        _running = false;
        if (_thread.joinable()) _thread.join();
        return _result;
    }

private:

    Instance& _i;
    Random _random;
    std::atomic<bool> _running{};
    std::thread _thread{};
    Result _result{};
    HostProcessData _data{};
    ParameterChanges _inputs;
    ParameterChanges _outputs;
    EventList _events{64};
    ProcessContext _context{};
    TSamples _position{};
    std::vector<Output_param> _collected{};
    int32 _next_note{1};
    std::vector<int16> _held{};

    auto automate(int32 frames) -> void
    {
        _inputs.clearQueue();
        if (_i.params.empty() || !_random.chance(0.5)) return;
        for (auto n = _random.below(4) + 1; n > 0; --n) {
            const auto& p = _i.params[_random.below(static_cast<uint32_t>(_i.params.size()))];
            if (p.flags & ParameterInfo::kIsReadOnly) continue;
            auto index = int32{};
            auto* queue = _inputs.addParameterData(p.id, index);
            if (!queue || queue->getPointCount() > 0) continue; // Already automated this block: points must stay in order.
            auto offset = int32{};
            for (auto points = _random.below(3) + 1; points > 0 && offset < frames; --points) {
                auto value = _random.real(0, 1);
                if (p.stepCount > 0) value = std::round(value * p.stepCount) / p.stepCount;
                auto point = int32{};
                queue->addPoint(offset, value, point);
                offset += static_cast<int32>(1 + _random.below(static_cast<uint32_t>(frames)));
            }
        }
    }

    auto notes(int32 frames) -> void
    {
        _events.clear();
        if (_i.event_inputs == 0 || !_random.chance(0.3)) return;
        auto e = Event{};
        e.busIndex = 0;
        e.sampleOffset = static_cast<int32>(_random.below(static_cast<uint32_t>(frames)));
        if (!_held.empty() && _random.chance(0.5)) {
            e.type = Event::kNoteOffEvent;
            e.noteOff = {0, _held.back(), 0.f, -1, 0.f};
            _held.pop_back();
        }
        else if (_held.size() < 8) {
            const auto pitch = static_cast<int16>(36 + _random.below(60));
            e.type = Event::kNoteOnEvent;
            e.noteOn = {0, pitch, 0.f, 0.8f, 0, _next_note++};
            _held.push_back(pitch);
        }
        else return;
        _events.addEvent(e);
    }

    auto run_block() -> void
    {
        // A parameter flush: no samples, no buffers.
        if (_random.chance(0.03)) {
            automate(1);
            auto flush = ProcessData{};
            flush.processMode = _data.processMode;
            flush.symbolicSampleSize = kSample32;
            flush.inputParameterChanges = &_inputs;
            flush.outputParameterChanges = &_outputs;
            _outputs.clearQueue();
            if (_i.processor->process(flush) != kResultOk) ++_result.errors;
            return;
        }

        const auto frames = static_cast<int32>(1 + _random.below(static_cast<uint32_t>(_i.max_block)));
        _data.numSamples = frames;
        automate(frames);
        notes(frames);
        _outputs.clearQueue();
        for (auto b = 0; b < _data.numInputs; ++b)
            for (auto c = 0; c < _data.inputs[b].numChannels; ++c)
                for (auto f = 0; f < frames; ++f) _data.inputs[b].channelBuffers32[c][f] = static_cast<float>(_random.real(-0.5, 0.5));

        _context.state = ProcessContext::kPlaying | ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid;
        _context.sampleRate = 48000;
        _context.tempo = 120;
        _context.timeSigNumerator = 4;
        _context.timeSigDenominator = 4;
        _context.projectTimeSamples = _position;
        _context.projectTimeMusic = static_cast<double>(_position) * 2 / 48000;
        _data.processContext = _random.chance(0.05) ? nullptr : &_context; // Some hosts send none.
        _position += frames;
        if (_random.chance(0.01)) _position = static_cast<TSamples>(_random.below(1'000'000)); // A loop jump.

        const auto before = tiny::hosts::trapped_allocations();
        auto result = tresult{};
        {
            const auto scope = tiny::hosts::Trap_scope{};
            result = _i.processor->process(_data);
        }
        _result.allocations += tiny::hosts::trapped_allocations() - before;
        ++_result.blocks;
        if (result != kResultOk) ++_result.errors;
        for (auto b = 0; b < _data.numOutputs; ++b)
            for (auto c = 0; c < _data.outputs[b].numChannels; ++c)
                for (auto f = 0; f < frames; ++f)
                    if (!std::isfinite(_data.outputs[b].channelBuffers32[c][f])) { ++_result.non_finite; goto checked; }
        checked:

        _collected.clear();
        for (auto q = 0; q < _outputs.getParameterCount(); ++q) {
            auto* queue = _outputs.getParameterData(q);
            auto offset = int32{};
            auto value = ParamValue{};
            if (queue && queue->getPointCount() > 0 && queue->getPoint(queue->getPointCount() - 1, offset, value) == kResultOk) {
                _collected.push_back({queue->getParameterId(), value});
            }
        }
        if (!_collected.empty()) _i.post_outputs(_collected);
    }
};

// MARK: - editor

// The host's side of an editor: resizes when asked, and only on the UI thread.
class Plug_frame : public FObject, public IPlugFrame {
public:
    explicit Plug_frame(Violations& violations) : _violations{violations} {}
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override
    {
        if (!on_ui()) _violations.add("IPlugFrame::resizeView called off the UI thread");
        return view && size ? view->onSize(size) : kInvalidArgument;
    }
    OBJ_METHODS(Plug_frame, FObject)
    DEFINE_INTERFACES
        DEF_INTERFACE(IPlugFrame)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
private:
    Violations& _violations;
};

#if defined(_WIN32)
const auto platform_type = kPlatformTypeHWND;
#else
const auto platform_type = kPlatformTypeNSView;
#endif

// The controller's editor, attached to a window for as long as this lives.
class Editor_session {
public:
    Editor_session(Instance& instance, tiny::hosts::Window& window, Violations& violations)
    {
        _view = owned(instance.controller->createView(ViewType::kEditor));
        if (!_view || _view->isPlatformTypeSupported(platform_type) != kResultTrue) { _view = nullptr; return; }
        _frame = owned(new Plug_frame{violations});
        _view->setFrame(_frame);
        expect_true(_view->attached(window.content(), platform_type) == kResultOk, "IPlugView::attached failed");
        _ns_view = window.editor_view();
    }
    ~Editor_session()
    {
        if (!_view) return;
        _view->removed();
        _view->setFrame(nullptr);
    }
    Editor_session(const Editor_session&) = delete;
    auto operator=(const Editor_session&) -> Editor_session& = delete;

    auto use(Random& random) -> void
    {
        if (_ns_view) tiny::hosts::send_input(_ns_view, random, 3);
        tiny::hosts::pump_main(8ms);
    }

private:
    IPtr<IPlugView> _view{};
    IPtr<Plug_frame> _frame{};
    void* _ns_view{};
};

// MARK: - checks

auto check_clean(Violations& violations, const char* when) -> void
{
    const auto [count, list] = violations.take();
    auto text = std::string{};
    for (auto k = size_t{}; k < std::min<size_t>(list.size(), 5); ++k) text += "\n    " + list[k];
    expect_true(count == 0, std::format("{}: {} contract violation(s):{}", when, count, text));
}

auto check_result(const Engine::Result& r, const char* when) -> void
{
    expect_true(r.errors == 0, std::format("{}: {} process calls failed", when, r.errors));
    expect_true(r.non_finite == 0, std::format("{}: {} blocks wrote NaN or inf", when, r.non_finite));
    expect_true(r.allocations == 0, std::format("{}: {} allocations on the audio thread in {} blocks", when, r.allocations, r.blocks));
}

auto activate(Instance& i, double rate, int32 block, bool offline) -> void
{
    i.setup(rate, block, offline);
    expect_true(i.component->setActive(true) == kResultOk, "setActive(true) failed");
}

auto deactivate(Instance& i) -> void
{
    i.component->setActive(false);
}

constexpr auto rates = std::array{44100., 48000., 88200., 96000.};
constexpr auto block_sizes = std::array{32, 64, 128, 512, 1024, 4096};

// MARK: - scenarios

auto add_scenarios(const Bundle& bundle) -> void
{
    Tests::add("lifecycle: scan-style create and terminate, never activated", [&bundle] {
        auto violations = Violations{};
        for (auto n = 0; n < 5; ++n) { auto instance = Instance{bundle, violations}; }
        tiny::hosts::pump_main(10ms);
        check_clean(violations, "create/terminate");
    });

    Tests::add("lifecycle: activate, process, deactivate across rates, blocks and offline", [&bundle] {
        auto violations = Violations{};
        {
            auto instance = Instance{bundle, violations};
            auto random = Random{g_options.seed};
            for (auto cycle = 0; cycle < 8; ++cycle) {
                activate(instance, rates[random.below(rates.size())], block_sizes[random.below(block_sizes.size())], random.chance(0.3));
                auto engine = Engine{instance, g_options.seed + static_cast<uint32_t>(cycle)};
                engine.start();
                for (auto t = 0; t < 20; ++t) instance.service();
                check_result(engine.stop(), "processing");
                deactivate(instance);
                instance.service();
            }
        }
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "lifecycle");
    });

    Tests::add("state: save, load, save is identical, whole and trickled, inactive and active", [&bundle] {
        auto violations = Violations{};
        {
            auto instance = Instance{bundle, violations};
            auto random = Random{g_options.seed};
            const auto comp = instance.save_component(random);
            const auto ctrl = instance.save_controller(random);
            for (const auto chunk : {0u, 1u, 7u}) {
                expect_true(instance.load(comp, ctrl, random, chunk), std::format("load failed with {}-byte reads", chunk));
                expect_true(instance.save_component(random) == comp, std::format("processor state changed across a load with {}-byte reads", chunk));
                expect_true(instance.save_controller(random) == ctrl, std::format("controller state changed across a load with {}-byte reads", chunk));
            }
            activate(instance, 48000, 512, false);
            auto engine = Engine{instance, g_options.seed};
            engine.start();
            for (auto n = 0; n < 20; ++n) {
                expect_true(instance.load(comp, ctrl, random, n % 2 ? 3u : 0u), "load while processing failed");
                instance.service();
            }
            check_result(engine.stop(), "processing");
            deactivate(instance);
            instance.service();
            expect_true(instance.load(comp, ctrl, random), "load");
            expect_true(instance.save_component(random) == comp, "processor state changed across loads while processing");
        }
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "state");
    });

    Tests::add("state: truncated and corrupted chunks never crash, and a good one restores", [&bundle] {
        auto violations = Violations{};
        {
            auto instance = Instance{bundle, violations};
            auto random = Random{g_options.seed};
            const auto comp = instance.save_component(random);
            const auto ctrl = instance.save_controller(random);
            for (auto size = size_t{}; size < comp.size(); ++size) (void)instance.load(std::vector<char>(comp.begin(), comp.begin() + static_cast<std::ptrdiff_t>(size)), ctrl, random);
            for (auto size = size_t{}; size < ctrl.size(); size += 1 + ctrl.size() / 64) (void)instance.load(comp, std::vector<char>(ctrl.begin(), ctrl.begin() + static_cast<std::ptrdiff_t>(size)), random);
            for (auto n = 0; n < 200; ++n) {
                auto bad = n % 2 ? comp : ctrl;
                for (auto flips = 1 + random.below(4); flips > 0; --flips) bad[random.below(static_cast<uint32_t>(bad.size()))] ^= static_cast<char>(1 + random.below(255));
                if (n % 2) (void)instance.load(bad, ctrl, random, random.below(4));
                else (void)instance.load(comp, bad, random, random.below(4));
            }
            expect_true(instance.load(comp, ctrl, random), "a good chunk after bad ones must load");
            expect_true(instance.save_component(random) == comp, "a good chunk after bad ones must restore the processor exactly");
        }
        tiny::hosts::pump_main(20ms);
        (void)violations.take();
    });

    Tests::add("params: displayed text re-parses to the same text", [&bundle] {
        auto violations = Violations{};
        {
            auto instance = Instance{bundle, violations};
            for (const auto& p : instance.params) {
                for (auto k = 0; k <= 20; ++k) {
                    auto v = k / 20.;
                    if (p.stepCount > 0) v = std::round(v * p.stepCount) / p.stepCount;
                    String128 text{}, again{};
                    if (instance.controller->getParamStringByValue(p.id, v, text) != kResultTrue) continue;
                    auto parsed = ParamValue{};
                    if (instance.controller->getParamValueByString(p.id, text, parsed) != kResultTrue) continue;
                    instance.controller->getParamStringByValue(p.id, parsed, again);
                    expect_true(std::u16string{reinterpret_cast<const char16_t*>(text)} == std::u16string{reinterpret_cast<const char16_t*>(again)},
                                std::format("param {}: text changed across a round trip at {}", p.id, v));
                }
            }
        }
        check_clean(violations, "params");
    });

    Tests::add("editor: open, use and close, idle and while processing, with loads while open", [&bundle] {
        auto violations = Violations{};
        {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{bundle, violations};
            auto random = Random{g_options.seed};
            const auto comp = instance.save_component(random);
            const auto ctrl = instance.save_controller(random);
            for (auto cycle = 0; cycle < 6; ++cycle) {
                const auto processing = cycle % 2 == 1;
                if (processing) activate(instance, 48000, 512, false);
                auto engine = processing ? std::make_unique<Engine>(instance, g_options.seed + static_cast<uint32_t>(cycle)) : nullptr;
                if (engine) engine->start();
                {
                    auto editor = Editor_session{instance, window, violations};
                    for (auto k = 0; k < 12; ++k) {
                        editor.use(random);
                        if (k % 4 == 3) (void)instance.load(comp, ctrl, random); // A host load reaches an open editor.
                        instance.service();
                    }
                }
                if (engine) {
                    check_result(engine->stop(), "processing");
                    deactivate(instance);
                }
                instance.service();
            }
        }
        tiny::hosts::pump_main(50ms);
        check_clean(violations, "editor");
    });

    Tests::add("editor teardown: view released and components terminated at once, mid-activity", [&bundle] {
        auto violations = Violations{};
        for (auto n = 0; n < 5; ++n) {
            {
                auto window = tiny::hosts::Window{};
                auto instance = Instance{bundle, violations};
                auto random = Random{g_options.seed + static_cast<uint32_t>(n)};
                activate(instance, 48000, 256, false);
                auto engine = Engine{instance, g_options.seed + static_cast<uint32_t>(n)};
                engine.start();
                {
                    auto editor = Editor_session{instance, window, violations};
                    for (auto k = 0; k < 6; ++k) editor.use(random);
                    check_result(engine.stop(), "processing");
                    deactivate(instance);
                }
            } // View released, then both components terminated, with no run loop in between.
            tiny::hosts::pump_main(30ms);
        }
        check_clean(violations, "editor teardown");
    });

    Tests::add("chaos: processing with automation while the UI thread loads state, queries and restarts", [&bundle] {
        auto violations = Violations{};
        {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{bundle, violations};
            auto random = Random{g_options.seed};
            auto editor = std::unique_ptr<Editor_session>{};
            auto comps = std::vector<std::vector<char>>{instance.save_component(random)};
            const auto ctrl = instance.save_controller(random);
            activate(instance, 48000, 512, false);
            auto engine = std::make_unique<Engine>(instance, g_options.seed);
            engine->start();
            auto blocks = long{};
            auto loads = 0, cycles = 0;

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(g_options.chaos_seconds);
            while (std::chrono::steady_clock::now() < deadline) {
                switch (random.below(9)) {
                    case 0: comps.push_back(instance.save_component(random, random.below(8))); break;
                    case 1: (void)instance.load(comps[random.below(static_cast<uint32_t>(comps.size()))], ctrl, random, random.below(8)); ++loads; break;
                    case 2:
                        for (const auto& p : instance.params) {
                            String128 text{};
                            instance.controller->getParamStringByValue(p.id, instance.controller->getParamNormalized(p.id), text);
                        }
                        break;
                    case 3: (void)instance.processor->getLatencySamples(); (void)instance.processor->getTailSamples(); break;
                    case 4:
                        if (instance.handler->latency_changed.load() || random.chance(0.1)) {
                            const auto r = engine->stop();
                            check_result(r, "processing");
                            blocks += r.blocks;
                            deactivate(instance);
                            instance.service();
                            activate(instance, rates[random.below(rates.size())], block_sizes[random.below(block_sizes.size())], random.chance(0.2));
                            engine = std::make_unique<Engine>(instance, g_options.seed + static_cast<uint32_t>(++cycles));
                            engine->start();
                        }
                        break;
                    case 5:
                        if (random.chance(0.15)) {
                            if (editor) editor.reset();
                            else editor = std::make_unique<Editor_session>(instance, window, violations);
                        }
                        else if (editor) editor->use(random);
                        break;
                    default: instance.service(); break;
                }
            }
            editor.reset();
            const auto last = engine->stop();
            check_result(last, "processing");
            blocks += last.blocks;
            deactivate(instance);
            instance.service();
            std::printf("  chaos: %ld blocks, %d loads, %d reactivations, %d restarts\n", blocks, loads, cycles, instance.handler->restarts.load());
        }
        tiny::hosts::pump_main(50ms);
        check_clean(violations, "chaos");
    });

    Tests::add("teardown: terminated straight after a burst of activity, with nothing drained", [&bundle] {
        auto violations = Violations{};
        for (auto n = 0; n < 10; ++n) {
            auto instance = Instance{bundle, violations};
            auto random = Random{g_options.seed + static_cast<uint32_t>(n)};
            const auto comp = instance.save_component(random);
            const auto ctrl = instance.save_controller(random);
            activate(instance, 48000, 256, false);
            auto engine = Engine{instance, g_options.seed + static_cast<uint32_t>(n)};
            engine.start();
            for (auto k = 0; k < 5; ++k) (void)instance.load(comp, ctrl, random);
            check_result(engine.stop(), "processing");
            deactivate(instance);
        } // Terminated without servicing the UI thread or the run loop.
        tiny::hosts::pump_main(20ms);
        check_clean(violations, "teardown");
    });
}

} // namespace

auto main(int argc, char** argv) -> int
{
    g_options = tiny::hosts::parse_options(argc, argv);
    if (g_options.bundle.empty()) {
        std::printf("usage: vst3_host <Plugin.vst3> [--seed N] [--seconds S]\n");
        return 2;
    }
    std::printf("vst3_host: %s, seed %u\n", g_options.bundle.c_str(), g_options.seed);
    tiny::hosts::gui_init();
    try {
        const auto bundle = Bundle{g_options.bundle};
        add_scenarios(bundle);
        return audio_bench::Tests::run_all() == 0 ? 0 : 1;
    }
    catch (const std::exception& e) {
        std::printf("vst3_host: %s\n", e.what());
        return 1;
    }
}
