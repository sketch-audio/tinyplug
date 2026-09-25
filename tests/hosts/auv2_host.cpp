// A fake AUv2 host. Registers a .component's factory in this process only (AudioComponentRegister:
// nothing is installed), then drives a real AudioComponentInstance through the AU API from a main
// thread and a render thread, so the sanitizers can see the wrapper:
//
//     auv2_host <path/to/Plugin.component> [--seed N] [--seconds S]

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

#include <dlfcn.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <audio_bench/audio_bench.hpp>

#include "gui.hpp"
#include "support.hpp"

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::hosts::Random;

auto g_options = tiny::hosts::Options{};

auto four_cc(CFStringRef s) -> OSType
{
    char chars[5]{};
    CFStringGetCString(s, chars, sizeof(chars), kCFStringEncodingMacRoman);
    return static_cast<OSType>((chars[0] << 24) | (chars[1] << 16) | (chars[2] << 8) | chars[3]);
}

// MARK: - bundle

struct Bundle {
    AudioComponentDescription desc{};
    AudioComponent component{};

    explicit Bundle(const std::string& path)
    {
        const auto slash = path.find_last_of('/');
        const auto name = path.substr(slash + 1, path.find_last_of('.') - slash - 1);
        auto* handle = dlopen((path + "/Contents/MacOS/" + name).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) throw std::runtime_error(std::format("dlopen failed: {}", dlerror()));

        auto* url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(path.c_str()), static_cast<CFIndex>(path.size()), true);
        auto* bundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        auto* list = static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents")));
        if (!list || CFArrayGetCount(list) < 1) throw std::runtime_error("no AudioComponents in Info.plist");
        auto* info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
        desc.componentType = four_cc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("type"))));
        desc.componentSubType = four_cc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("subtype"))));
        desc.componentManufacturer = four_cc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("manufacturer"))));
        char factory_name[256]{};
        CFStringGetCString(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("factoryFunction"))), factory_name, sizeof(factory_name), kCFStringEncodingUTF8);
        auto version = UInt32{};
        CFNumberGetValue(static_cast<CFNumberRef>(CFDictionaryGetValue(info, CFSTR("version"))), kCFNumberSInt32Type, &version);
        auto* factory = reinterpret_cast<AudioComponentFactoryFunction>(dlsym(handle, factory_name));
        if (!factory) throw std::runtime_error(std::format("factory {} missing", factory_name));
        component = AudioComponentRegister(&desc, static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("name"))), version, factory);
        CFRelease(bundle);
        if (!component) throw std::runtime_error("AudioComponentRegister failed");
    }

    auto has_audio_input() const -> bool { return desc.componentType == kAudioUnitType_Effect || desc.componentType == kAudioUnitType_MusicEffect; }
    auto takes_midi() const -> bool
    {
        return desc.componentType == kAudioUnitType_MusicDevice || desc.componentType == kAudioUnitType_MusicEffect || desc.componentType == kAudioUnitType_MIDIProcessor;
    }
};

// MARK: - instance

struct Param { AudioUnitParameterID id{}; AudioUnitParameterInfo info{}; };

class Instance {
public:

    Instance(const Bundle& bundle) : _bundle{bundle}
    {
        if (AudioComponentInstanceNew(bundle.component, &unit) != noErr || !unit) throw std::runtime_error("AudioComponentInstanceNew failed");
        AudioUnitAddPropertyListener(unit, kAudioUnitProperty_Latency, &Instance::on_property, this);
        AudioUnitAddPropertyListener(unit, kAudioUnitProperty_ParameterList, &Instance::on_property, this);

        auto size = UInt32{};
        if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, &size, nullptr) == noErr && size > 0) {
            auto ids = std::vector<AudioUnitParameterID>(size / sizeof(AudioUnitParameterID));
            AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0, ids.data(), &size);
            for (const auto id : ids) {
                auto p = Param{.id = id};
                auto info_size = static_cast<UInt32>(sizeof(p.info));
                if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global, id, &p.info, &info_size) == noErr) {
                    if (p.info.flags & kAudioUnitParameterFlag_CFNameRelease && p.info.cfNameString) CFRelease(p.info.cfNameString);
                    params.push_back(p);
                }
            }
        }
    }

    ~Instance()
    {
        AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_Latency, &Instance::on_property, this);
        AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_ParameterList, &Instance::on_property, this);
        AudioComponentInstanceDispose(unit);
    }

    Instance(const Instance&) = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    // Formats, slice size, callbacks, then AudioUnitInitialize: a host's configure.
    auto initialize(double rate, UInt32 max_frames, bool offline) -> bool
    {
        auto format = AudioStreamBasicDescription{
            .mSampleRate = rate,
            .mFormatID = kAudioFormatLinearPCM,
            .mFormatFlags = kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved,
            .mBytesPerPacket = 4, .mFramesPerPacket = 1, .mBytesPerFrame = 4,
            .mChannelsPerFrame = 2, .mBitsPerChannel = 32,
        };
        AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, sizeof(format));
        if (_bundle.has_audio_input()) {
            AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
            auto callback = AURenderCallbackStruct{&Instance::provide_input, this};
            AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        }
        AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &max_frames, sizeof(max_frames));
        auto offline_flag = UInt32{offline ? 1u : 0u};
        AudioUnitSetProperty(unit, kAudioUnitProperty_OfflineRender, kAudioUnitScope_Global, 0, &offline_flag, sizeof(offline_flag));
        auto callbacks = HostCallbackInfo{this, &Instance::beat_and_tempo, nullptr, &Instance::transport_state, nullptr};
        AudioUnitSetProperty(unit, kAudioUnitProperty_HostCallbacks, kAudioUnitScope_Global, 0, &callbacks, sizeof(callbacks));
        this->max_frames = max_frames;
        this->rate = rate;
        return AudioUnitInitialize(unit) == noErr;
    }

    auto uninitialize() -> void { AudioUnitUninitialize(unit); }

    auto save() -> std::vector<uint8_t>
    {
        auto plist = CFPropertyListRef{};
        auto size = static_cast<UInt32>(sizeof(plist));
        expect_true(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) == noErr && plist, "ClassInfo get failed");
        auto bytes = to_bytes(plist);
        CFRelease(plist);
        return bytes;
    }

    auto load(const std::vector<uint8_t>& bytes) -> bool
    {
        auto plist = from_bytes(bytes);
        if (!plist) return false;
        const auto ok = load(plist);
        CFRelease(plist);
        return ok;
    }

    auto load(CFPropertyListRef plist) -> bool
    {
        return AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, sizeof(plist)) == noErr;
    }

    static auto to_bytes(CFPropertyListRef plist) -> std::vector<uint8_t>
    {
        auto* data = CFPropertyListCreateData(nullptr, plist, kCFPropertyListBinaryFormat_v1_0, 0, nullptr);
        auto out = std::vector<uint8_t>(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
        CFRelease(data);
        return out;
    }
    static auto from_bytes(const std::vector<uint8_t>& bytes) -> CFPropertyListRef
    {
        auto* data = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
        auto* plist = CFPropertyListCreateWithData(nullptr, data, kCFPropertyListImmutable, nullptr, nullptr);
        CFRelease(data);
        return plist;
    }

    auto latency() -> double
    {
        auto seconds = Float64{};
        auto size = static_cast<UInt32>(sizeof(seconds));
        AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size);
        return seconds;
    }
    auto tail() -> double
    {
        auto seconds = Float64{};
        auto size = static_cast<UInt32>(sizeof(seconds));
        AudioUnitGetProperty(unit, kAudioUnitProperty_TailTime, kAudioUnitScope_Global, 0, &seconds, &size);
        return seconds;
    }
    auto set_bypass(bool on) -> void
    {
        auto value = UInt32{on ? 1u : 0u};
        AudioUnitSetProperty(unit, kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0, &value, sizeof(value));
    }

    // What a host's main thread does between calls: listeners, then the run loop.
    auto service() -> void
    {
        if (latency_changed.exchange(false)) (void)latency(); // The acceptance.
        tiny::hosts::pump_main(200us);
    }

    AudioComponentInstance unit{};
    std::vector<Param> params{};
    UInt32 max_frames{512};
    double rate{48000};
    std::atomic<bool> latency_changed{};
    std::atomic<int> latency_notes{};

    // Render-thread state the callbacks read.
    std::atomic<uint32_t> input_seed{1};
    double beat{};
    bool playing{true};

private:

    const Bundle& _bundle;

    static void on_property(void* self, AudioUnit, AudioUnitPropertyID id, AudioUnitScope, AudioUnitElement)
    {
        auto& i = *static_cast<Instance*>(self);
        if (id == kAudioUnitProperty_Latency) { i.latency_changed = true; i.latency_notes.fetch_add(1); }
    }

    static OSStatus provide_input(void* self, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* data)
    {
        auto& i = *static_cast<Instance*>(self);
        auto x = i.input_seed.load(std::memory_order_relaxed);
        for (auto b = UInt32{}; b < data->mNumberBuffers; ++b) {
            auto* samples = static_cast<float*>(data->mBuffers[b].mData);
            if (!samples) continue;
            for (auto f = UInt32{}; f < frames; ++f) {
                x = x * 1664525u + 1013904223u;
                samples[f] = static_cast<float>(x >> 8) / static_cast<float>(1u << 24) - 0.5f;
            }
        }
        i.input_seed.store(x, std::memory_order_relaxed);
        return noErr;
    }

    static OSStatus beat_and_tempo(void* self, Float64* beat, Float64* tempo)
    {
        auto& i = *static_cast<Instance*>(self);
        if (beat) *beat = i.beat;
        if (tempo) *tempo = 120;
        return noErr;
    }

    static OSStatus transport_state(void* self, Boolean* playing, Boolean* changed, Float64* sample_pos, Boolean* cycling, Float64*, Float64*)
    {
        auto& i = *static_cast<Instance*>(self);
        if (playing) *playing = i.playing;
        if (changed) *changed = false;
        if (sample_pos) *sample_pos = i.beat * 60. / 120. * i.rate;
        if (cycling) *cycling = false;
        return noErr;
    }
};

// MARK: - render engine

class Engine {
public:

    struct Result {
        long blocks{}, errors{}, non_finite{}, allocations{};
    };

    Engine(Instance& instance, const Bundle& bundle, uint32_t seed)
        : _i{instance}, _bundle{bundle}, _random{seed}
    {
        for (auto& channel : _storage) channel.resize(instance.max_frames);
        _list = static_cast<AudioBufferList*>(std::calloc(1, offsetof(AudioBufferList, mBuffers) + 2 * sizeof(AudioBuffer)));
        _list->mNumberBuffers = 2;
        _events.reserve(8);
        _held.reserve(8);
    }

    ~Engine() { stop(); std::free(_list); }
    Engine(const Engine&) = delete;
    auto operator=(const Engine&) -> Engine& = delete;

    auto start() -> void
    {
        _running = true;
        _thread = std::thread{[this] { while (_running.load(std::memory_order_relaxed)) run_block(); }};
    }

    auto stop() -> Result
    {
        _running = false;
        if (_thread.joinable()) _thread.join();
        return _result;
    }

private:

    Instance& _i;
    const Bundle& _bundle;
    Random _random;
    std::atomic<bool> _running{};
    std::thread _thread{};
    Result _result{};
    std::array<std::vector<float>, 2> _storage{};
    AudioBufferList* _list{};
    Float64 _sample_time{};
    std::vector<AudioUnitParameterEvent> _events{};
    std::vector<UInt8> _held{};

    auto run_block() -> void
    {
        const auto frames = static_cast<UInt32>(1 + _random.below(_i.max_frames));

        // Automation: scheduled from the render thread, as hosts do for sample-accurate changes.
        _events.clear();
        if (!_i.params.empty() && _random.chance(0.5)) {
            for (auto n = _random.below(3) + 1; n > 0; --n) {
                const auto& p = _i.params[_random.below(static_cast<uint32_t>(_i.params.size()))];
                if (!(p.info.flags & kAudioUnitParameterFlag_IsWritable)) continue;
                auto e = AudioUnitParameterEvent{.scope = kAudioUnitScope_Global, .element = 0, .parameter = p.id};
                const auto value = static_cast<AudioUnitParameterValue>(_random.real(p.info.minValue, p.info.maxValue));
                if (_random.chance(0.3)) {
                    e.eventType = kParameterEvent_Ramped;
                    e.eventValues.ramp = {static_cast<SInt32>(_random.below(frames)), frames, p.info.minValue, value};
                }
                else {
                    e.eventType = kParameterEvent_Immediate;
                    e.eventValues.immediate = {_random.below(frames), value};
                }
                _events.push_back(e);
            }
            if (!_events.empty()) AudioUnitScheduleParameters(_i.unit, _events.data(), static_cast<UInt32>(_events.size()));
        }
        if (_bundle.takes_midi() && _random.chance(0.3)) {
            if (!_held.empty() && _random.chance(0.5)) {
                MusicDeviceMIDIEvent(_i.unit, 0x80, _held.back(), 0, _random.below(frames));
                _held.pop_back();
            }
            else if (_held.size() < 8) {
                const auto key = static_cast<UInt8>(36 + _random.below(60));
                MusicDeviceMIDIEvent(_i.unit, 0x90, key, 100, _random.below(frames));
                _held.push_back(key);
            }
        }
        if (_random.chance(0.02)) _i.playing = !_i.playing;
        if (_i.playing) _i.beat += frames * 2.0 / _i.rate;

        for (auto c = 0; c < 2; ++c) {
            _list->mBuffers[c] = AudioBuffer{1, frames * static_cast<UInt32>(sizeof(float)), _storage[static_cast<size_t>(c)].data()};
        }
        auto stamp = AudioTimeStamp{};
        stamp.mSampleTime = _sample_time;
        stamp.mFlags = kAudioTimeStampSampleTimeValid;
        _sample_time += frames;
        auto flags = AudioUnitRenderActionFlags{};

        const auto before = tiny::hosts::trapped_allocations();
        auto status = OSStatus{};
        {
            const auto scope = tiny::hosts::Trap_scope{};
            status = AudioUnitRender(_i.unit, &flags, &stamp, 0, frames, _list);
        }
        _result.allocations += tiny::hosts::trapped_allocations() - before;
        ++_result.blocks;
        if (status != noErr) ++_result.errors;
        for (auto c = UInt32{}; c < _list->mNumberBuffers; ++c) {
            const auto* samples = static_cast<const float*>(_list->mBuffers[c].mData);
            for (auto f = UInt32{}; samples && f < frames; ++f) if (!std::isfinite(samples[f])) { ++_result.non_finite; break; }
        }
    }
};

// MARK: - editor

// The unit's Cocoa view (kAudioUnitProperty_CocoaUI), in a window for as long as this lives.
class Editor_session {
public:
    Editor_session(Instance& instance, tiny::hosts::Window& window) : _window{window}
    {
        _view = tiny::hosts::make_au_cocoa_view(instance.unit);
        if (_view) _window.add(_view);
    }
    ~Editor_session() { close(); }
    Editor_session(const Editor_session&) = delete;
    auto operator=(const Editor_session&) -> Editor_session& = delete;

    auto use(Random& random) -> void
    {
        if (_view) tiny::hosts::send_input(_view, random, 3);
        tiny::hosts::pump_main(8ms);
    }
    auto close() -> void
    {
        if (!_view) return;
        _window.remove(_view);
        tiny::hosts::release_view(_view);
        _view = nullptr;
    }
    auto open() const -> bool { return _view != nullptr; }

private:
    tiny::hosts::Window& _window;
    void* _view{};
};

// MARK: - checks

auto check_result(const Engine::Result& r, const char* when) -> void
{
    expect_true(r.errors == 0, std::format("{}: {} renders failed", when, r.errors));
    expect_true(r.non_finite == 0, std::format("{}: {} blocks wrote NaN or inf", when, r.non_finite));
    expect_true(r.allocations == 0, std::format("{}: {} allocations on the render thread in {} blocks", when, r.allocations, r.blocks));
}

// A ClassInfo dictionary with some entries mangled: wrong types, truncated data, missing keys.
auto corrupt(CFPropertyListRef plist, Random& random) -> CFPropertyListRef
{
    auto* dict = CFDictionaryCreateMutableCopy(nullptr, 0, static_cast<CFDictionaryRef>(plist));
    const auto count = CFDictionaryGetCount(dict);
    auto keys = std::vector<const void*>(static_cast<size_t>(count));
    auto values = std::vector<const void*>(static_cast<size_t>(count));
    CFDictionaryGetKeysAndValues(dict, keys.data(), values.data());
    for (auto k = size_t{}; k < keys.size(); ++k) {
        if (!random.chance(0.3)) continue;
        switch (random.below(4)) {
            case 0: CFDictionaryRemoveValue(dict, keys[k]); break;
            case 1: {
                auto n = static_cast<int32_t>(random.below(1u << 30)) - (1 << 29);
                auto* number = CFNumberCreate(nullptr, kCFNumberSInt32Type, &n);
                CFDictionarySetValue(dict, keys[k], number);
                CFRelease(number);
                break;
            }
            case 2: CFDictionarySetValue(dict, keys[k], CFSTR("garbage")); break;
            default:
                if (CFGetTypeID(values[k]) == CFDataGetTypeID()) {
                    auto* data = static_cast<CFDataRef>(values[k]);
                    auto bytes = std::vector<UInt8>(CFDataGetBytePtr(data), CFDataGetBytePtr(data) + CFDataGetLength(data));
                    bytes.resize(random.below(static_cast<uint32_t>(bytes.size() + 1)));
                    for (auto& b : bytes) if (random.chance(0.05)) b = static_cast<UInt8>(random.below(256));
                    auto* cut = CFDataCreate(nullptr, bytes.data(), static_cast<CFIndex>(bytes.size()));
                    CFDictionarySetValue(dict, keys[k], cut);
                    CFRelease(cut);
                }
                break;
        }
    }
    return dict;
}

constexpr auto rates = std::array{44100., 48000., 88200., 96000.};
constexpr auto slice_sizes = std::array{64u, 128u, 512u, 1024u, 4096u};

// MARK: - scenarios

auto add_scenarios(const Bundle& bundle) -> void
{
    Tests::add("lifecycle: scan-style open and dispose, never initialized", [&bundle] {
        for (auto n = 0; n < 5; ++n) { auto instance = Instance{bundle}; }
        tiny::hosts::pump_main(10ms);
    });

    Tests::add("lifecycle: initialize, render, uninitialize across rates, slices and offline", [&bundle] {
        auto instance = Instance{bundle};
        auto random = Random{g_options.seed};
        for (auto cycle = 0; cycle < 8; ++cycle) {
            expect_true(instance.initialize(rates[random.below(rates.size())], slice_sizes[random.below(slice_sizes.size())], random.chance(0.3)), "AudioUnitInitialize failed");
            auto engine = Engine{instance, bundle, g_options.seed + static_cast<uint32_t>(cycle)};
            engine.start();
            for (auto t = 0; t < 20; ++t) instance.service();
            check_result(engine.stop(), "rendering");
            instance.uninitialize();
            instance.service();
        }
    });

    Tests::add("state: ClassInfo save, restore, save is identical, idle and while rendering", [&bundle] {
        auto instance = Instance{bundle};
        const auto first = instance.save();
        expect_true(instance.load(first), "restore failed");
        expect_true(instance.save() == first, "ClassInfo changed across a restore");
        expect_true(instance.initialize(48000, 512, false), "AudioUnitInitialize failed");
        auto engine = Engine{instance, bundle, g_options.seed};
        engine.start();
        for (auto n = 0; n < 20; ++n) {
            expect_true(instance.load(first), "restore while rendering failed");
            instance.service();
        }
        check_result(engine.stop(), "rendering");
        instance.uninitialize();
        expect_true(instance.load(first), "restore");
        expect_true(instance.save() == first, "ClassInfo changed across restores while rendering");
    });

    Tests::add("state: a ClassInfo load off main is the state at once, and main applies it at its next tick", [&bundle] {
        auto instance = Instance{bundle};
        auto random = Random{g_options.seed};
        // By content: a dictionary parsed from bytes need not serialize in the same key order.
        const auto same = [](const std::vector<uint8_t>& x, const std::vector<uint8_t>& y) {
            auto px = Instance::from_bytes(x), py = Instance::from_bytes(y);
            const auto equal = px && py && CFEqual(px, py);
            if (px) CFRelease(px);
            if (py) CFRelease(py);
            return equal;
        };
        const auto a = instance.save();
        for (const auto& p : instance.params) {
            AudioUnitSetParameter(instance.unit, p.id, kAudioUnitScope_Global, 0, static_cast<AudioUnitParameterValue>(random.real(p.info.minValue, p.info.maxValue)), 0);
        }
        const auto b = instance.save();
        expect_true(!same(a, b), "the two states should differ");

        auto loaded = false;
        std::thread{[&] { loaded = instance.load(a); }}.join();
        expect_true(loaded, "load off main failed");
        auto from_other = std::vector<uint8_t>{};
        std::thread{[&] { from_other = instance.save(); }}.join();
        expect_true(same(from_other, a), "a save off main straight after a load off main must return what was loaded");

        tiny::hosts::pump_main(std::chrono::milliseconds{300}); // Main's tick applies it.
        std::thread{[&] { from_other = instance.save(); }}.join();
        expect_true(same(from_other, a), "the applied load must save as loaded, off main");
        expect_true(same(instance.save(), a), "the applied load must save as loaded, on main");
    });

    Tests::add("state: corrupted ClassInfo never crashes, and a good one restores", [&bundle] {
        auto instance = Instance{bundle};
        auto random = Random{g_options.seed};
        const auto good_bytes = instance.save();
        auto* good = Instance::from_bytes(good_bytes);
        for (auto n = 0; n < 200; ++n) {
            auto* bad = corrupt(good, random);
            (void)instance.load(bad);
            CFRelease(bad);
        }
        expect_true(instance.load(good), "a good ClassInfo after bad ones must restore");
        CFRelease(good);
        expect_true(instance.save() == good_bytes, "a good ClassInfo after bad ones must restore the state exactly");
    });

    Tests::add("editor: open, use and close, idle and while rendering, with restores while open", [&bundle] {
        auto window = tiny::hosts::Window{};
        auto instance = Instance{bundle};
        auto random = Random{g_options.seed};
        const auto state = instance.save();
        for (auto cycle = 0; cycle < 6; ++cycle) {
            const auto rendering = cycle % 2 == 1;
            if (rendering) expect_true(instance.initialize(48000, 512, false), "AudioUnitInitialize failed");
            auto engine = rendering ? std::make_unique<Engine>(instance, bundle, g_options.seed + static_cast<uint32_t>(cycle)) : nullptr;
            if (engine) engine->start();
            {
                auto editor = Editor_session{instance, window};
                expect_true(editor.open(), "no Cocoa view");
                for (auto k = 0; k < 12; ++k) {
                    editor.use(random);
                    if (k % 4 == 3) (void)instance.load(state); // A host restore reaches an open editor.
                    instance.service();
                }
            }
            if (engine) {
                check_result(engine->stop(), "rendering");
                instance.uninitialize();
            }
            instance.service();
        }
    });

    // Hosts don't all close a plug-in's window before disposing of the unit.
    Tests::add("editor: the view outlives the unit it belongs to", [&bundle] {
        auto window = tiny::hosts::Window{};
        auto random = Random{g_options.seed};
        for (auto n = 0; n < 3; ++n) {
            auto view = static_cast<void*>(nullptr);
            {
                auto instance = Instance{bundle};
                expect_true(instance.initialize(48000, 256, false), "AudioUnitInitialize failed");
                view = tiny::hosts::make_au_cocoa_view(instance.unit);
                expect_true(view != nullptr, "no Cocoa view");
                window.add(view);
                for (auto k = 0; k < 4; ++k) { tiny::hosts::send_input(view, random, 3); tiny::hosts::pump_main(8ms); }
            } // The unit is disposed; its view is still in the window, drawing.
            for (auto k = 0; k < 6; ++k) { tiny::hosts::send_input(view, random, 2); tiny::hosts::pump_main(8ms); }
            window.remove(view);
            tiny::hosts::release_view(view);
            tiny::hosts::pump_main(20ms);
        }
    });

    // The TSan target: render thread busy, main thread setting parameters, restoring state,
    // flipping bypass and asking for latency, the way Logic does.
    Tests::add("chaos: rendering with automation while main sets parameters, restores and reconfigures", [&bundle] {
        auto window = tiny::hosts::Window{};
        auto instance = Instance{bundle};
        auto random = Random{g_options.seed};
        auto editor = std::unique_ptr<Editor_session>{};
        auto states = std::vector<std::vector<uint8_t>>{instance.save()};
        expect_true(instance.initialize(48000, 512, false), "AudioUnitInitialize failed");
        auto engine = std::make_unique<Engine>(instance, bundle, g_options.seed);
        engine->start();
        auto blocks = long{};
        auto restores = 0, cycles = 0;
        auto latencies = std::vector<double>{};

        // Hosts save and load ClassInfo off main too (autosave, background project writes).
        auto off_main_running = std::atomic<bool>{true};
        auto off_main_saves = std::atomic<long>{}, off_main_loads = std::atomic<long>{}, off_main_failures = std::atomic<long>{};
        auto off_main = std::thread{[&] {
            auto r = Random{g_options.seed + 99};
            while (off_main_running.load()) {
                auto plist = CFPropertyListRef{};
                auto size = static_cast<UInt32>(sizeof(plist));
                if (AudioUnitGetProperty(instance.unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &plist, &size) != noErr || !plist) { ++off_main_failures; continue; }
                ++off_main_saves;
                if (r.chance(0.1)) {
                    if (instance.load(plist)) ++off_main_loads;
                    else ++off_main_failures;
                }
                CFRelease(plist);
                std::this_thread::sleep_for(std::chrono::microseconds{r.below(2000)});
            }
        }};

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(g_options.chaos_seconds);
        while (std::chrono::steady_clock::now() < deadline) {
            switch (random.below(9)) {
                case 0: states.push_back(instance.save()); break;
                case 1: (void)instance.load(states[random.below(static_cast<uint32_t>(states.size()))]); ++restores; break;
                case 2:
                    for (const auto& p : instance.params) {
                        // Control parameters aren't host-writable (so Logic shows no dead control), but
                        // the plug-in's own editor sets them; now and then, stand in for it.
                        if (!(p.info.flags & kAudioUnitParameterFlag_IsWritable) && !random.chance(0.1)) continue;
                        if (random.chance(0.5)) AudioUnitSetParameter(instance.unit, p.id, kAudioUnitScope_Global, 0, static_cast<AudioUnitParameterValue>(random.real(p.info.minValue, p.info.maxValue)), 0);
                        auto value = AudioUnitParameterValue{};
                        AudioUnitGetParameter(instance.unit, p.id, kAudioUnitScope_Global, 0, &value);
                    }
                    break;
                case 3: {
                    const auto l = instance.latency();
                    if (std::find(latencies.begin(), latencies.end(), l) == latencies.end()) latencies.push_back(l);
                    (void)instance.tail();
                    break;
                }
                case 4: instance.set_bypass(random.chance(0.3)); break;
                case 6:
                    if (instance.latency_changed.load() || random.chance(0.1)) {
                        const auto r = engine->stop();
                        check_result(r, "rendering");
                        blocks += r.blocks;
                        // Reset only while nothing renders: the AU SDK's own DoReset races with DoRender.
                        AudioUnitReset(instance.unit, kAudioUnitScope_Global, 0);
                        instance.uninitialize();
                        instance.service();
                        expect_true(instance.initialize(rates[random.below(rates.size())], slice_sizes[random.below(slice_sizes.size())], random.chance(0.2)), "reinitialize failed");
                        engine = std::make_unique<Engine>(instance, bundle, g_options.seed + static_cast<uint32_t>(++cycles));
                        engine->start();
                    }
                    break;
                case 7:
                    if (random.chance(0.15)) {
                        if (editor) editor.reset();
                        else editor = std::make_unique<Editor_session>(instance, window);
                    }
                    else if (editor) editor->use(random);
                    break;
                default: instance.service(); break;
            }
        }
        off_main_running = false;
        off_main.join();
        expect_true(off_main_saves > 0 && off_main_failures == 0, std::format("off main: {} saves, {} failures", off_main_saves.load(), off_main_failures.load()));
        editor.reset();
        const auto last = engine->stop();
        check_result(last, "rendering");
        blocks += last.blocks;
        instance.uninitialize();
        instance.service();
        std::printf("  chaos: %ld blocks, %d restores (%ld off main), %ld off-main saves, %d reinitializations, %d latency notifications, %zu distinct latencies\n",
                    blocks, restores, off_main_loads.load(), off_main_saves.load(), cycles, instance.latency_notes.load(), latencies.size());
    });

    Tests::add("teardown: disposed straight after a burst of activity, with nothing drained", [&bundle] {
        for (auto n = 0; n < 10; ++n) {
            auto instance = Instance{bundle};
            const auto state = instance.save();
            expect_true(instance.initialize(48000, 256, false), "AudioUnitInitialize failed");
            auto engine = Engine{instance, bundle, g_options.seed + static_cast<uint32_t>(n)};
            engine.start();
            for (auto k = 0; k < 5; ++k) (void)instance.load(state);
            check_result(engine.stop(), "rendering");
        } // Disposed while still initialized, without servicing the run loop.
        tiny::hosts::pump_main(20ms);
    });
}

} // namespace

auto main(int argc, char** argv) -> int
{
    g_options = tiny::hosts::parse_options(argc, argv);
    if (g_options.bundle.empty()) {
        std::printf("usage: auv2_host <Plugin.component> [--seed N] [--seconds S]\n");
        return 2;
    }
    std::printf("auv2_host: %s, seed %u\n", g_options.bundle.c_str(), g_options.seed);
    tiny::hosts::gui_init();
    try {
        const auto bundle = Bundle{g_options.bundle};
        add_scenarios(bundle);
        return audio_bench::Tests::run_all() == 0 ? 0 : 1;
    }
    catch (const std::exception& e) {
        std::printf("auv2_host: %s\n", e.what());
        return 1;
    }
}
