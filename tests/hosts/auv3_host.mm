// A fake AUv3 host. Loads the extension's code from a test bundle, registers the audio unit class in
// this process (registerSubclass), and creates units the way the system does, through the extension's
// view controller (its AUAudioUnitFactory). Then drives the AUAudioUnit API from the main thread and
// a render thread, so the sanitizers can see the wrapper:
//
//     auv3_host <path/to/Plugin.auv3test> <path/to/Plugin.component> [--seed N] [--seconds S]
//
// The component description comes from the AUv2 bundle's Info.plist: AUv3 derives the same one.

#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudioKit/CoreAudioKit.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include <audio_bench/audio_bench.hpp>

#include "gui.hpp"
#include "support.hpp"

// Counts latency changes the unit announces through KVO, as hosts observe them.
@interface Latency_observer : NSObject
@property (atomic) int notes;
@end
@implementation Latency_observer
- (void)observeValueForKeyPath:(NSString*)keyPath ofObject:(id)object change:(NSDictionary*)change context:(void*)context
{
    self.notes = self.notes + 1;
}
@end

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::hosts::Random;

auto g_options = tiny::hosts::Options{};
auto g_desc = AudioComponentDescription{};

auto four_cc(CFStringRef s) -> OSType
{
    char chars[5]{};
    CFStringGetCString(s, chars, sizeof(chars), kCFStringEncodingMacRoman);
    return static_cast<OSType>((chars[0] << 24) | (chars[1] << 16) | (chars[2] << 8) | chars[3]);
}

auto description_from(const std::string& component) -> AudioComponentDescription
{
    auto* url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8*>(component.c_str()), static_cast<CFIndex>(component.size()), true);
    auto* bundle = CFBundleCreate(nullptr, url);
    CFRelease(url);
    if (!bundle) throw std::runtime_error("can't open the AUv2 bundle");
    auto* list = static_cast<CFArrayRef>(CFBundleGetValueForInfoDictionaryKey(bundle, CFSTR("AudioComponents")));
    if (!list || CFArrayGetCount(list) < 1) throw std::runtime_error("no AudioComponents in the AUv2 Info.plist");
    auto* info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
    const auto desc = AudioComponentDescription{
        four_cc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("type")))),
        four_cc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("subtype")))),
        four_cc(static_cast<CFStringRef>(CFDictionaryGetValue(info, CFSTR("manufacturer")))),
        0, 0};
    CFRelease(bundle);
    return desc;
}

auto has_audio_input() -> bool { return g_desc.componentType == kAudioUnitType_Effect || g_desc.componentType == kAudioUnitType_MusicEffect; }

// MARK: - bundle

struct Bundle {
    Class vc_class{};

    explicit Bundle(const std::string& path)
    {
        const auto slash = path.find_last_of('/');
        const auto name = path.substr(slash + 1, path.find_last_of('.') - slash - 1);
        if (!dlopen((path + "/Contents/MacOS/" + name).c_str(), RTLD_NOW | RTLD_LOCAL)) throw std::runtime_error(std::format("dlopen failed: {}", dlerror()));
        vc_class = NSClassFromString(@"Auv3_AUViewController");
        Class au_class = NSClassFromString(@"Auv3_AUAudioUnit");
        if (!vc_class || !au_class) throw std::runtime_error("AUv3 classes missing");
        [AUAudioUnit registerSubclass:au_class asComponentDescription:g_desc name:@"tinyplug test" version:1];
    }
};

// MARK: - instance

class Instance {
public:

    explicit Instance(const Bundle& bundle)
    {
        vc = [[bundle.vc_class alloc] init];
        NSError* error = nil;
        au = [(id<AUAudioUnitFactory>)vc createAudioUnitWithComponentDescription:g_desc error:&error];
        if (!au) throw std::runtime_error("createAudioUnitWithComponentDescription failed");
        observer = [Latency_observer new];
        [au addObserver:observer forKeyPath:@"latency" options:0 context:nullptr];
        for (AUParameter* p in au.parameterTree.allParameters) params.push_back(p);

        auto* self_beat = &beat;
        auto* self_playing = &playing;
        au.musicalContextBlock = ^BOOL(double* tempo, double* num, NSInteger* denom, double* beat_pos, NSInteger* offset, double* downbeat) {
            if (tempo) *tempo = 120;
            if (num) *num = 4;
            if (denom) *denom = 4;
            if (beat_pos) *beat_pos = *self_beat;
            if (offset) *offset = 0;
            if (downbeat) *downbeat = std::floor(*self_beat / 4) * 4;
            return YES;
        };
        au.transportStateBlock = ^BOOL(AUHostTransportStateFlags* flags, double* sample, double* start, double* end) {
            if (flags) *flags = *self_playing ? AUHostTransportStateMoving : AUHostTransportStateChanged;
            if (sample) *sample = *self_beat * 24000;
            if (start) *start = 0;
            if (end) *end = 0;
            return YES;
        };
    }

    ~Instance()
    {
        [au removeObserver:observer forKeyPath:@"latency"];
    }

    Instance(const Instance&) = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    auto allocate(double rate, AUAudioFrameCount max_frames, bool offline) -> bool
    {
        auto* format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:rate channels:2];
        NSError* error = nil;
        [au.outputBusses[0] setFormat:format error:&error];
        if (has_audio_input() && au.inputBusses.count > 0) [au.inputBusses[0] setFormat:format error:&error];
        au.maximumFramesToRender = max_frames;
        au.renderingOffline = offline;
        this->max_frames = max_frames;
        this->rate = rate;
        return [au allocateRenderResourcesAndReturnError:&error];
    }

    auto deallocate() -> void { [au deallocateRenderResources]; }

    auto save() -> std::vector<uint8_t>
    {
        NSDictionary* state = au.fullState;
        expect_true(state != nil, "fullState is nil");
        NSData* data = [NSPropertyListSerialization dataWithPropertyList:state format:NSPropertyListBinaryFormat_v1_0 options:0 error:nil];
        const auto* bytes = static_cast<const uint8_t*>(data.bytes);
        return {bytes, bytes + data.length};
    }

    static auto decode(const std::vector<uint8_t>& bytes) -> NSDictionary*
    {
        NSData* data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
        return [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:nil error:nil];
    }

    auto load(NSDictionary* state) -> void { au.fullState = state; }

    // What a host's main thread does between calls.
    auto service() -> void { tiny::hosts::pump_main(200us); }

    NSViewController* vc{};
    AUAudioUnit* au{};
    Latency_observer* observer{};
    std::vector<AUParameter*> params{};
    AUAudioFrameCount max_frames{512};
    double rate{48000};
    double beat{};
    bool playing{true};
};

// MARK: - render engine

class Engine {
public:

    struct Result {
        long blocks{}, errors{}, non_finite{}, allocations{};
    };

    Engine(Instance& instance, uint32_t seed) : _i{instance}, _random{seed}
    {
        _render = instance.au.renderBlock;
        _schedule = instance.au.scheduleParameterBlock;
        _midi = instance.au.scheduleMIDIEventBlock;
        for (auto& channel : _storage) channel.resize(instance.max_frames);
        _list = static_cast<AudioBufferList*>(std::calloc(1, offsetof(AudioBufferList, mBuffers) + 2 * sizeof(AudioBuffer)));
        _list->mNumberBuffers = 2;
        __block auto input_seed = uint32_t{1};
        _pull = ^AUAudioUnitStatus(AudioUnitRenderActionFlags*, const AudioTimeStamp*, AUAudioFrameCount frames, NSInteger, AudioBufferList* data) {
            for (auto b = UInt32{}; b < data->mNumberBuffers; ++b) {
                auto* samples = static_cast<float*>(data->mBuffers[b].mData);
                if (!samples) continue;
                for (auto f = AUAudioFrameCount{}; f < frames; ++f) {
                    input_seed = input_seed * 1664525u + 1013904223u;
                    samples[f] = static_cast<float>(input_seed >> 8) / static_cast<float>(1u << 24) - 0.5f;
                }
            }
            return noErr;
        };
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
    Random _random;
    std::atomic<bool> _running{};
    std::thread _thread{};
    Result _result{};
    AURenderBlock _render{};
    AUScheduleParameterBlock _schedule{};
    AUScheduleMIDIEventBlock _midi{};
    AURenderPullInputBlock _pull{};
    std::array<std::vector<float>, 2> _storage{};
    AudioBufferList* _list{};
    Float64 _sample_time{};
    std::vector<uint8_t> _held{};

    auto run_block() -> void
    {
        const auto frames = static_cast<AUAudioFrameCount>(1 + _random.below(_i.max_frames));

        if (_schedule && !_i.params.empty() && _random.chance(0.5)) {
            for (auto n = _random.below(3) + 1; n > 0; --n) {
                AUParameter* p = _i.params[_random.below(static_cast<uint32_t>(_i.params.size()))];
                const auto value = static_cast<AUValue>(_random.real(p.minValue, p.maxValue));
                const auto ramp = _random.chance(0.3) ? static_cast<AUAudioFrameCount>(_random.below(frames)) : 0;
                _schedule(AUEventSampleTimeImmediate + _random.below(frames), ramp, p.address, value);
            }
        }
        if (_midi && _random.chance(0.3)) {
            uint8_t bytes[3]{};
            if (!_held.empty() && _random.chance(0.5)) {
                bytes[0] = 0x80; bytes[1] = _held.back(); _held.pop_back();
            }
            else if (_held.size() < 8) {
                bytes[0] = 0x90; bytes[1] = static_cast<uint8_t>(36 + _random.below(60)); bytes[2] = 100;
                _held.push_back(bytes[1]);
            }
            if (bytes[0]) _midi(AUEventSampleTimeImmediate + _random.below(frames), 0, 3, bytes);
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
        auto status = AUAudioUnitStatus{};
        {
            const auto scope = tiny::hosts::Trap_scope{};
            status = _render(&flags, &stamp, frames, 0, _list, has_audio_input() ? _pull : AURenderPullInputBlock{});
        }
        _result.allocations += tiny::hosts::trapped_allocations() - before;
        ++_result.blocks;
        if (status != noErr) ++_result.errors;
        for (auto c = UInt32{}; c < _list->mNumberBuffers; ++c) {
            const auto* samples = static_cast<const float*>(_list->mBuffers[c].mData);
            for (auto f = AUAudioFrameCount{}; samples && f < frames; ++f) if (!std::isfinite(samples[f])) { ++_result.non_finite; break; }
        }
    }
};

// MARK: - editor

// The view controller's view, in a window for as long as this lives (viewWillAppear ... viewDidDisappear).
class Editor_session {
public:
    Editor_session(Instance& instance, tiny::hosts::Window& window) : _window{window}
    {
        _view = (__bridge void*)instance.vc.view;
        [(__bridge NSView*)_view setFrameSize:NSMakeSize(640, 480)];
        _window.add(_view);
    }
    ~Editor_session() { _window.remove(_view); }
    Editor_session(const Editor_session&) = delete;
    auto operator=(const Editor_session&) -> Editor_session& = delete;

    auto use(Random& random) -> void
    {
        tiny::hosts::send_input(_view, random, 3);
        tiny::hosts::pump_main(8ms);
    }

private:
    tiny::hosts::Window& _window;
    void* _view{};
};

auto check_result(const Engine::Result& r, const char* when) -> void
{
    expect_true(r.errors == 0, std::format("{}: {} renders failed", when, r.errors));
    expect_true(r.non_finite == 0, std::format("{}: {} blocks wrote NaN or inf", when, r.non_finite));
    expect_true(r.allocations == 0, std::format("{}: {} allocations on the render thread in {} blocks", when, r.allocations, r.blocks));
}

// A fullState with some entries mangled: wrong types, truncated data, missing keys. Not "data": that is
// AudioToolbox's parameter archive, and its parser hangs (spins zero-filling past the end) on a damaged
// one, so no wrapper code is reached; see the report on a tinyplug-owned copy of the values.
auto corrupt(NSDictionary* state, Random& random) -> NSDictionary*
{
    NSMutableDictionary* dict = [state mutableCopy];
    for (id key in state) {
        if ([key isEqual:@"data"] || !random.chance(0.3)) continue;
        switch (random.below(4)) {
            case 0: [dict removeObjectForKey:key]; break;
            case 1: dict[key] = @(static_cast<int32_t>(random.below(1u << 30)) - (1 << 29)); break;
            case 2: dict[key] = @"garbage"; break;
            default:
                if ([state[key] isKindOfClass:[NSData class]]) {
                    NSData* data = state[key];
                    auto bytes = std::vector<uint8_t>(static_cast<const uint8_t*>(data.bytes), static_cast<const uint8_t*>(data.bytes) + data.length);
                    bytes.resize(random.below(static_cast<uint32_t>(bytes.size() + 1)));
                    for (auto& b : bytes) if (random.chance(0.05)) b = static_cast<uint8_t>(random.below(256));
                    dict[key] = [NSData dataWithBytes:bytes.data() length:bytes.size()];
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
    Tests::add("lifecycle: create and release, never allocated", [&bundle] {
        for (auto n = 0; n < 5; ++n) @autoreleasepool { auto instance = Instance{bundle}; tiny::hosts::pump_main(2ms); }
        tiny::hosts::pump_main(20ms);
    });

    Tests::add("lifecycle: allocate, render, deallocate across rates, slices and offline", [&bundle] {
        @autoreleasepool {
            auto instance = Instance{bundle};
            auto random = Random{g_options.seed};
            for (auto cycle = 0; cycle < 8; ++cycle) {
                expect_true(instance.allocate(rates[random.below(rates.size())], slice_sizes[random.below(slice_sizes.size())], random.chance(0.3)), "allocateRenderResources failed");
                auto engine = Engine{instance, g_options.seed + static_cast<uint32_t>(cycle)};
                engine.start();
                for (auto t = 0; t < 20; ++t) instance.service();
                check_result(engine.stop(), "rendering");
                instance.deallocate();
                instance.service();
            }
        }
        tiny::hosts::pump_main(20ms);
    });

    Tests::add("state: fullState save, restore, save is identical, idle and while rendering", [&bundle] {
        @autoreleasepool {
            auto instance = Instance{bundle};
            const auto first = instance.save();
            instance.load(Instance::decode(first));
            expect_true(instance.save() == first, "fullState changed across a restore");
            expect_true(instance.allocate(48000, 512, false), "allocateRenderResources failed");
            auto engine = Engine{instance, g_options.seed};
            engine.start();
            for (auto n = 0; n < 20; ++n) { instance.load(Instance::decode(first)); instance.service(); }
            check_result(engine.stop(), "rendering");
            instance.deallocate();
            instance.load(Instance::decode(first));
            expect_true(instance.save() == first, "fullState changed across restores while rendering");
        }
    });

    Tests::add("state: a fullState load off main is the state at once, and main applies it at its next tick", [&bundle] {
        @autoreleasepool {
            auto instance = Instance{bundle};
            auto random = Random{g_options.seed};
            // By content: a dictionary parsed from bytes need not serialize in the same key order.
            const auto same = [](const std::vector<uint8_t>& x, const std::vector<uint8_t>& y) {
                return [Instance::decode(x) isEqualToDictionary:Instance::decode(y)];
            };
            const auto a = instance.save();
            for (AUParameter* p : instance.params) p.value = static_cast<AUValue>(random.real(p.minValue, p.maxValue));
            const auto b = instance.save();
            expect_true(!same(a, b), "the two states should differ");

            std::thread{[&] { @autoreleasepool { instance.load(Instance::decode(a)); } }}.join();
            auto from_other = std::vector<uint8_t>{};
            std::thread{[&] { @autoreleasepool { from_other = instance.save(); } }}.join();
            expect_true(same(from_other, a), "a save off main straight after a load off main must return what was loaded");

            tiny::hosts::pump_main(std::chrono::milliseconds{300}); // Main's tick applies it.
            std::thread{[&] { @autoreleasepool { from_other = instance.save(); } }}.join();
            expect_true(same(from_other, a), "the applied load must save as loaded, off main");
            expect_true(same(instance.save(), a), "the applied load must save as loaded, on main");
        }
    });

    Tests::add("state: corrupted fullState never crashes, and a good one restores", [&bundle] {
        @autoreleasepool {
            auto instance = Instance{bundle};
            auto random = Random{g_options.seed};
            const auto good_bytes = instance.save();
            NSDictionary* good = Instance::decode(good_bytes);
            for (auto n = 0; n < 200; ++n) @autoreleasepool { instance.load(corrupt(good, random)); }
            instance.load(good);
            expect_true(instance.save() == good_bytes, "a good fullState after bad ones must restore the state exactly");
        }
    });

    Tests::add("editor: open, use and close, idle and while rendering, with restores while open", [&bundle] {
        @autoreleasepool {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{bundle};
            auto random = Random{g_options.seed};
            const auto state = instance.save();
            for (auto cycle = 0; cycle < 6; ++cycle) @autoreleasepool {
                const auto rendering = cycle % 2 == 1;
                if (rendering) expect_true(instance.allocate(48000, 512, false), "allocateRenderResources failed");
                auto engine = rendering ? std::make_unique<Engine>(instance, g_options.seed + static_cast<uint32_t>(cycle)) : nullptr;
                if (engine) engine->start();
                {
                    auto editor = Editor_session{instance, window};
                    for (auto k = 0; k < 12; ++k) {
                        editor.use(random);
                        if (k % 4 == 3) instance.load(Instance::decode(state)); // A host restore reaches an open editor.
                        instance.service();
                    }
                }
                if (engine) {
                    check_result(engine->stop(), "rendering");
                    instance.deallocate();
                }
                instance.service();
            }
        }
        tiny::hosts::pump_main(30ms);
    });

    Tests::add("editor: unit and view controller released with the view still in the window", [&bundle] {
        auto window = tiny::hosts::Window{};
        auto random = Random{g_options.seed};
        for (auto n = 0; n < 3; ++n) {
            NSView* view = nil;
            @autoreleasepool {
                auto instance = Instance{bundle};
                expect_true(instance.allocate(48000, 256, false), "allocateRenderResources failed");
                view = instance.vc.view;
                window.add((__bridge void*)view);
                for (auto k = 0; k < 4; ++k) { tiny::hosts::send_input((__bridge void*)view, random, 3); tiny::hosts::pump_main(8ms); }
            } // The unit and its view controller go; the window still holds the view.
            for (auto k = 0; k < 6; ++k) { tiny::hosts::send_input((__bridge void*)view, random, 2); tiny::hosts::pump_main(8ms); }
            window.remove((__bridge void*)view);
            view = nil;
            tiny::hosts::pump_main(20ms);
        }
    });

    Tests::add("chaos: rendering with automation while main sets parameters, restores and reconfigures", [&bundle] {
        @autoreleasepool {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{bundle};
            auto random = Random{g_options.seed};
            auto editor = std::unique_ptr<Editor_session>{};
            auto states = std::vector<std::vector<uint8_t>>{instance.save()};
            expect_true(instance.allocate(48000, 512, false), "allocateRenderResources failed");
            auto engine = std::make_unique<Engine>(instance, g_options.seed);
            engine->start();
            auto blocks = long{};
            auto restores = 0, cycles = 0;

            // Out-of-process AUv3s get host calls over XPC, off the extension's main thread.
            auto off_main_running = std::atomic<bool>{true};
            auto off_main_saves = std::atomic<long>{}, off_main_loads = std::atomic<long>{};
            AUAudioUnit* au = instance.au;
            auto off_main = std::thread{[&, au] {
                auto r = Random{g_options.seed + 99};
                while (off_main_running.load()) {
                    @autoreleasepool {
                        NSDictionary* state = au.fullState;
                        if (state != nil) ++off_main_saves;
                        if (state != nil && r.chance(0.1)) { au.fullState = state; ++off_main_loads; }
                        if (r.chance(0.05)) (void)au.currentPreset;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds{r.below(2000)});
                }
            }};
            auto latencies = std::vector<double>{};

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(g_options.chaos_seconds);
            while (std::chrono::steady_clock::now() < deadline) @autoreleasepool {
                switch (random.below(8)) {
                    case 0: states.push_back(instance.save()); break;
                    case 1: instance.load(Instance::decode(states[random.below(static_cast<uint32_t>(states.size()))])); ++restores; break;
                    case 2:
                        for (AUParameter* p : instance.params) {
                            if (random.chance(0.5)) p.value = static_cast<AUValue>(random.real(p.minValue, p.maxValue));
                            (void)p.value;
                        }
                        break;
                    case 3: {
                        const auto l = instance.au.latency;
                        if (std::find(latencies.begin(), latencies.end(), l) == latencies.end()) latencies.push_back(l);
                        (void)instance.au.tailTime;
                        break;
                    }
                    case 4: instance.au.shouldBypassEffect = random.chance(0.3); break;
                    case 5:
                        if (random.chance(0.1)) {
                            const auto r = engine->stop();
                            check_result(r, "rendering");
                            blocks += r.blocks;
                            [instance.au reset]; // Only while nothing renders.
                            instance.deallocate();
                            instance.service();
                            expect_true(instance.allocate(rates[random.below(rates.size())], slice_sizes[random.below(slice_sizes.size())], random.chance(0.2)), "reallocate failed");
                            engine = std::make_unique<Engine>(instance, g_options.seed + static_cast<uint32_t>(++cycles));
                            engine->start();
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
            off_main_running = false;
            off_main.join();
            expect_true(off_main_saves > 0, "no fullState saves off main");
            std::printf("  off main: %ld saves, %ld loads\n", off_main_saves.load(), off_main_loads.load());
            editor.reset();
            const auto last = engine->stop();
            check_result(last, "rendering");
            blocks += last.blocks;
            instance.deallocate();
            instance.service();
            std::printf("  chaos: %ld blocks, %d restores, %d reallocations, %d latency notifications, %zu distinct latencies\n",
                        blocks, restores, cycles, instance.observer.notes, latencies.size());
        }
        tiny::hosts::pump_main(50ms);
    });

    Tests::add("teardown: released straight after a burst of activity, with nothing drained", [&bundle] {
        for (auto n = 0; n < 10; ++n) @autoreleasepool {
            auto instance = Instance{bundle};
            const auto state = instance.save();
            expect_true(instance.allocate(48000, 256, false), "allocateRenderResources failed");
            auto engine = Engine{instance, g_options.seed + static_cast<uint32_t>(n)};
            engine.start();
            for (auto k = 0; k < 5; ++k) instance.load(Instance::decode(state));
            check_result(engine.stop(), "rendering");
        } // Released while still allocated, without servicing the run loop.
        tiny::hosts::pump_main(20ms);
    });
}

} // namespace

auto main(int argc, char** argv) -> int
{
    @autoreleasepool {
        tiny::hosts::gui_init();
        // The last bare argument is the AUv2 bundle; parse_options takes the one before it.
        auto args = std::vector<char*>(argv, argv + argc);
        auto component = std::string{};
        for (auto i = argc - 1; i > 0; --i) {
            if (argv[i][0] != '-' && (i == 1 || argv[i - 1][0] != '-')) { component = argv[i]; args.erase(args.begin() + i); break; }
        }
        g_options = tiny::hosts::parse_options(static_cast<int>(args.size()), args.data());
        if (g_options.bundle.empty() || component.empty()) {
            std::printf("usage: auv3_host <Plugin.auv3test> <Plugin.component> [--seed N] [--seconds S]\n");
            return 2;
        }
        std::printf("auv3_host: %s, seed %u\n", g_options.bundle.c_str(), g_options.seed);
        try {
            g_desc = description_from(component);
            const auto bundle = Bundle{g_options.bundle};
            add_scenarios(bundle);
            return audio_bench::Tests::run_all() == 0 ? 0 : 1;
        }
        catch (const std::exception& e) {
            std::printf("auv3_host: %s\n", e.what());
            return 1;
        }
    }
}
