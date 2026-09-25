// A fake CLAP host. Loads a real .clap and drives it through the host-facing API from the threads a
// real host uses (main, audio), so the sanitizers can see the wrapper. One process per bundle:
//
//     clap_host <path/to/Plugin.clap> [--seed N] [--seconds S]
//
// The host answers thread_check honestly and fails a test on any misbehaving or error log, so the
// CLAP helpers' own contract checks (maximal in debug builds) count as assertions here.

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

#include <audio_bench/audio_bench.hpp>

#include "clap/clap.h"
#include "gui.hpp"
#include "support.hpp"

namespace {

using namespace std::chrono_literals;
using audio_bench::Tests;
using audio_bench::expect_true;
using tiny::hosts::Random;

auto g_options = tiny::hosts::Options{};

// Set while the host flushes on the audio thread. clap-helpers' debug flush validation calls its own
// main-thread-only checks from there and blames the host (HOST_MISBEHAVING "params.info() on wrong
// thread"); the wrapper itself logs nothing in a flush, so those entries are set aside.
thread_local bool t_audio_flush = false;

// MARK: - bundle

struct Bundle {
    void* handle{};
    const clap_plugin_entry* entry{};
    const clap_plugin_factory* factory{};

    explicit Bundle(const std::string& path)
    {
        const auto slash = path.find_last_of('/');
        const auto name = path.substr(slash + 1, path.find_last_of('.') - slash - 1);
        handle = dlopen((path + "/Contents/MacOS/" + name).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) throw std::runtime_error(std::format("dlopen failed: {}", dlerror()));
        entry = static_cast<const clap_plugin_entry*>(dlsym(handle, "clap_entry"));
        if (!entry || !entry->init(path.c_str())) throw std::runtime_error("clap_entry missing or init failed");
        factory = static_cast<const clap_plugin_factory*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory || factory->get_plugin_count(factory) != 1) throw std::runtime_error("expected one plug-in");
    }
    ~Bundle() { entry->deinit(); } // Never dlclose: macOS keeps Objective-C images anyway.
};

// MARK: - host

class Fake_host {
public:

    Fake_host() : _main{std::this_thread::get_id()} {}

    clap_host clap{
        .clap_version = CLAP_VERSION,
        .host_data = this,
        .name = "tinyplug fake host",
        .vendor = "tinyplug",
        .url = "",
        .version = "1",
        .get_extension = &get_extension,
        .request_restart = [](const clap_host* h) { self(h).restart_requested = true; },
        .request_process = [](const clap_host* h) { self(h).process_requested = true; },
        .request_callback = [](const clap_host* h) { self(h).callback_requested = true; },
    };

    std::atomic<bool> restart_requested{}, process_requested{}, callback_requested{}, flush_requested{};
    std::atomic<int> latency_changes{};

    auto set_audio_thread(std::thread::id id) -> void { _audio.store(id); }
    auto on_main() const -> bool { return std::this_thread::get_id() == _main; }

    // Misbehaving and error logs since the last call.
    auto take_problems() -> std::vector<std::string>
    {
        const auto lock = std::lock_guard{_log_mutex};
        return std::exchange(_problems, {});
    }

private:

    const std::thread::id _main;
    std::atomic<std::thread::id> _audio{};
    std::mutex _log_mutex{};
    std::vector<std::string> _problems{};

    static auto self(const clap_host* h) -> Fake_host& { return *static_cast<Fake_host*>(h->host_data); }

    static auto get_extension(const clap_host*, const char* id) -> const void*
    {
        static const clap_host_thread_check thread_check{
            .is_main_thread = [](const clap_host* h) { return self(h).on_main(); },
            .is_audio_thread = [](const clap_host* h) { return std::this_thread::get_id() == self(h)._audio.load(); },
        };
        static const clap_host_log log{
            .log = [](const clap_host* h, clap_log_severity severity, const char* msg) {
                if (severity < CLAP_LOG_ERROR || t_audio_flush) return;
                auto& host = self(h);
                const auto lock = std::lock_guard{host._log_mutex};
                host._problems.push_back(std::format("[log {}] {}", static_cast<int>(severity), msg ? msg : ""));
            },
        };
        static const clap_host_params params{
            .rescan = [](const clap_host*, clap_param_rescan_flags) {},
            .clear = [](const clap_host*, clap_id, clap_param_clear_flags) {},
            .request_flush = [](const clap_host* h) { self(h).flush_requested = true; },
        };
        static const clap_host_latency latency{
            .changed = [](const clap_host* h) { self(h).latency_changes.fetch_add(1); },
        };
        static const clap_host_tail tail{.changed = [](const clap_host*) {}};
        static const clap_host_state state{.mark_dirty = [](const clap_host*) {}};
        static const clap_host_audio_ports audio_ports{
            .is_rescan_flag_supported = [](const clap_host*, uint32_t) { return false; },
            .rescan = [](const clap_host*, uint32_t) {},
        };
        static const clap_host_note_ports note_ports{
            .supported_dialects = [](const clap_host*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
            .rescan = [](const clap_host*, uint32_t) {},
        };

        if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &thread_check;
        if (!std::strcmp(id, CLAP_EXT_LOG)) return &log;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio_ports;
        static const clap_host_gui gui{
            .resize_hints_changed = [](const clap_host*) {},
            .request_resize = [](const clap_host*, uint32_t, uint32_t) { return true; },
            .request_show = [](const clap_host*) { return false; },
            .request_hide = [](const clap_host*) { return false; },
            .closed = [](const clap_host*, bool) {},
        };

        if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &note_ports;
        if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
        return nullptr;
    }
};

// MARK: - streams

// A stream that moves at most `chunk_max` bytes per call (1 = one byte at a time), as some hosts do.
struct Out_stream {
    std::vector<uint8_t> bytes{};
    Random* random{};
    uint32_t chunk_max{0}; // 0: everything at once.

    clap_ostream clap{this, [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
        auto& self = *static_cast<Out_stream*>(s->ctx);
        const auto n = self.chunk_max ? std::min<uint64_t>(size, 1 + self.random->below(self.chunk_max)) : size;
        const auto* p = static_cast<const uint8_t*>(buffer);
        self.bytes.insert(self.bytes.end(), p, p + n);
        return static_cast<int64_t>(n);
    }};
};

struct In_stream {
    const std::vector<uint8_t>* bytes{};
    size_t pos{};
    Random* random{};
    uint32_t chunk_max{0};

    clap_istream clap{this, [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
        auto& self = *static_cast<In_stream*>(s->ctx);
        const auto left = self.bytes->size() - self.pos;
        auto n = std::min<uint64_t>(size, left);
        if (self.chunk_max) n = std::min<uint64_t>(n, 1 + self.random->below(self.chunk_max));
        std::memcpy(buffer, self.bytes->data() + self.pos, n);
        self.pos += n;
        return static_cast<int64_t>(n);
    }};
};

// MARK: - plug-in instance

struct Port {
    uint32_t channels{};
};

class Instance {
public:

    Instance(const Bundle& bundle, Fake_host& host)
    {
        const auto* desc = bundle.factory->get_plugin_descriptor(bundle.factory, 0);
        plugin = bundle.factory->create_plugin(bundle.factory, &host.clap, desc->id);
        if (!plugin || !plugin->init(plugin)) throw std::runtime_error("create or init failed");
        params = ext<clap_plugin_params>(CLAP_EXT_PARAMS);
        state = ext<clap_plugin_state>(CLAP_EXT_STATE);
        latency = ext<clap_plugin_latency>(CLAP_EXT_LATENCY);
        tail = ext<clap_plugin_tail>(CLAP_EXT_TAIL);
        audio_ports = ext<clap_plugin_audio_ports>(CLAP_EXT_AUDIO_PORTS);
        note_ports = ext<clap_plugin_note_ports>(CLAP_EXT_NOTE_PORTS);
        gui = ext<clap_plugin_gui>(CLAP_EXT_GUI);

        if (params) {
            for (auto i = uint32_t{}; i < params->count(plugin); ++i) {
                auto info = clap_param_info{};
                if (params->get_info(plugin, i, &info)) param_infos.push_back(info);
            }
        }
        for (const auto input : {true, false}) {
            const auto count = audio_ports ? audio_ports->count(plugin, input) : 0;
            for (auto i = uint32_t{}; i < count; ++i) {
                auto info = clap_audio_port_info{};
                if (audio_ports->get(plugin, i, input, &info)) (input ? inputs : outputs).push_back(Port{info.channel_count});
            }
        }
        if (note_ports && note_ports->count(plugin, true) > 0) {
            auto info = clap_note_port_info{};
            if (note_ports->get(plugin, 0, true, &info)) {
                notes_in = true;
                notes_clap = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
            }
        }
    }

    ~Instance() { plugin->destroy(plugin); }

    Instance(const Instance&) = delete;
    auto operator=(const Instance&) -> Instance& = delete;

    auto save(Random& random, uint32_t chunk_max = 0) const -> std::vector<uint8_t>
    {
        auto out = Out_stream{.random = &random, .chunk_max = chunk_max};
        expect_true(state->save(plugin, &out.clap), "state save failed");
        return out.bytes;
    }

    auto load(const std::vector<uint8_t>& bytes, Random& random, uint32_t chunk_max = 0) const -> bool
    {
        auto in = In_stream{.bytes = &bytes, .random = &random, .chunk_max = chunk_max};
        return state->load(plugin, &in.clap);
    }

    const clap_plugin* plugin{};
    const clap_plugin_params* params{};
    const clap_plugin_state* state{};
    const clap_plugin_latency* latency{};
    const clap_plugin_tail* tail{};
    const clap_plugin_audio_ports* audio_ports{};
    const clap_plugin_note_ports* note_ports{};
    const clap_plugin_gui* gui{};

    std::vector<clap_param_info> param_infos{};
    std::vector<Port> inputs{}, outputs{};
    bool notes_in{}, notes_clap{};

private:

    template<typename T>
    auto ext(const char* id) const -> const T* { return static_cast<const T*>(plugin->get_extension(plugin, id)); }
};

// MARK: - events

// Input events for one call, sorted by time. Storage is reserved up front so building a block
// never allocates on the audio thread (not that the trap counts the host's own work).
class Event_list {
public:

    Event_list() { _params.reserve(64); _notes.reserve(64); _midi.reserve(64); _sorted.reserve(192); }

    auto clear() -> void { _params.clear(); _notes.clear(); _midi.clear(); _sorted.clear(); }

    auto param(uint32_t time, const clap_param_info& info, double value) -> void
    {
        if (_params.size() == _params.capacity()) return;
        auto& e = _params.emplace_back();
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = info.id;
        e.cookie = info.cookie;
        e.note_id = -1; e.port_index = -1; e.channel = -1; e.key = -1;
        e.value = value;
    }

    auto note(uint32_t time, bool on, int16_t key, int32_t id, bool clap_dialect) -> void
    {
        if (clap_dialect) {
            if (_notes.size() == _notes.capacity()) return;
            auto& e = _notes.emplace_back();
            e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            e.note_id = id; e.port_index = 0; e.channel = 0; e.key = key; e.velocity = on ? 0.8 : 0.;
        }
        else {
            if (_midi.size() == _midi.capacity()) return;
            auto& e = _midi.emplace_back();
            e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.port_index = 0;
            e.data[0] = static_cast<uint8_t>(on ? 0x90 : 0x80); e.data[1] = static_cast<uint8_t>(key); e.data[2] = on ? 100 : 0;
        }
    }

    auto seal() -> void
    {
        for (const auto& e : _params) _sorted.push_back(&e.header);
        for (const auto& e : _notes) _sorted.push_back(&e.header);
        for (const auto& e : _midi) _sorted.push_back(&e.header);
        std::stable_sort(_sorted.begin(), _sorted.end(), [](auto* a, auto* b) { return a->time < b->time; });
    }

    clap_input_events clap{this,
        [](const clap_input_events* l) { return static_cast<uint32_t>(static_cast<const Event_list*>(l->ctx)->_sorted.size()); },
        [](const clap_input_events* l, uint32_t i) { return static_cast<const Event_list*>(l->ctx)->_sorted[i]; }};

private:

    std::vector<clap_event_param_value> _params{};
    std::vector<clap_event_note> _notes{};
    std::vector<clap_event_midi> _midi{};
    std::vector<const clap_event_header*> _sorted{};
};

// Output events must arrive sorted and inside the block.
struct Out_events {
    uint32_t frames{};
    uint32_t last_time{};
    uint32_t count{};
    std::atomic<int> disorder{};

    clap_output_events clap{this, [](const clap_output_events* l, const clap_event_header* e) {
        auto& self = *static_cast<Out_events*>(l->ctx);
        if (e->time < self.last_time || (self.frames > 0 && e->time >= self.frames)) self.disorder.fetch_add(1);
        self.last_time = e->time;
        ++self.count;
        return true;
    }};

    auto reset(uint32_t block) -> void { frames = block; last_time = 0; }
};

// MARK: - audio engine

// Drives `process` on its own thread, as a host's audio thread, with automation, notes, transport
// and the odd flush. Validates every block.
class Engine {
public:

    struct Result {
        long blocks{}, errors{}, non_finite{}, disorder{}, allocations{};
    };

    Engine(Instance& instance, Fake_host& host, uint32_t seed, uint32_t max_frames)
        : _i{instance}, _host{host}, _random{seed}, _max_frames{max_frames}
    {
        auto make = [&](const std::vector<Port>& ports, std::vector<std::vector<float>>& storage,
                        std::vector<std::vector<float*>>& pointers, std::vector<clap_audio_buffer>& buffers) {
            for (const auto& port : ports) {
                auto& ptrs = pointers.emplace_back();
                for (auto c = uint32_t{}; c < port.channels; ++c) ptrs.push_back(storage.emplace_back(max_frames).data());
            }
            for (auto p = size_t{}; p < ports.size(); ++p) {
                buffers.push_back(clap_audio_buffer{pointers[p].data(), nullptr, ports[p].channels, 0, 0});
            }
        };
        make(instance.inputs, _in_storage, _in_ptrs, _in_buffers);
        make(instance.outputs, _out_storage, _out_ptrs, _out_buffers);
        _held.reserve(8);
    }

    // Starts the audio thread; it runs until stop().
    auto start() -> void
    {
        _running = true;
        _thread = std::thread{[this] {
            _host.set_audio_thread(std::this_thread::get_id());
            _i.plugin->start_processing(_i.plugin);
            while (_running.load(std::memory_order_relaxed)) run_block();
            _i.plugin->stop_processing(_i.plugin);
            _host.set_audio_thread({});
        }};
    }

    ~Engine() { stop(); }
    Engine(const Engine&) = delete;
    auto operator=(const Engine&) -> Engine& = delete;

    auto stop() -> Result
    {
        _running = false;
        if (_thread.joinable()) _thread.join();
        _result.disorder = _out.disorder.load();
        return _result;
    }

private:

    Instance& _i;
    Fake_host& _host;
    Random _random;
    uint32_t _max_frames{};
    std::atomic<bool> _running{};
    std::thread _thread{};
    Result _result{};

    std::vector<std::vector<float>> _in_storage{}, _out_storage{};
    std::vector<std::vector<float*>> _in_ptrs{}, _out_ptrs{};
    std::vector<clap_audio_buffer> _in_buffers{}, _out_buffers{};
    Event_list _events{};
    Out_events _out{};
    clap_event_transport _transport{};
    int64_t _steady{};
    double _beats{};
    bool _playing{true};
    int32_t _next_note{1};
    std::vector<std::pair<int16_t, int32_t>> _held{};

    auto build_events(uint32_t frames) -> void
    {
        _events.clear();
        const auto& infos = _i.param_infos;
        if (!infos.empty() && _random.chance(0.5)) {
            for (auto n = _random.below(6) + 1; n > 0; --n) {
                const auto& info = infos[_random.below(static_cast<uint32_t>(infos.size()))];
                if (info.flags & CLAP_PARAM_IS_READONLY) continue;
                auto value = _random.real(info.min_value, info.max_value);
                if (info.flags & CLAP_PARAM_IS_STEPPED) value = std::round(value);
                _events.param(_random.below(frames), info, value);
            }
        }
        if (_i.notes_in && _random.chance(0.3)) {
            if (!_held.empty() && _random.chance(0.5)) {
                const auto [key, id] = _held.back();
                _held.pop_back();
                _events.note(_random.below(frames), false, key, id, _i.notes_clap);
            }
            else if (_held.size() < 8) {
                const auto key = static_cast<int16_t>(36 + _random.below(60));
                _held.emplace_back(key, _next_note);
                _events.note(_random.below(frames), true, key, _next_note++, _i.notes_clap);
            }
        }
        _events.seal();
    }

    auto transport(uint32_t frames) -> const clap_event_transport*
    {
        if (_random.chance(0.02)) _playing = !_playing;
        if (_random.chance(0.05)) return nullptr; // Some hosts send none.
        _transport.header = {sizeof(_transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        _transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE
                         | (_playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
        _transport.tempo = 120;
        _transport.tsig_num = 4;
        _transport.tsig_denom = 4;
        _transport.song_pos_beats = static_cast<clap_beattime>(_beats * CLAP_BEATTIME_FACTOR);
        _transport.bar_start = static_cast<clap_beattime>(std::floor(_beats / 4) * 4 * CLAP_BEATTIME_FACTOR);
        _transport.bar_number = static_cast<int32_t>(_beats / 4);
        if (_playing) _beats += frames * 2.0 / 48000.;
        if (_random.chance(0.01)) _beats = _random.real(0, 64); // A loop jump.
        return &_transport;
    }

    auto run_block() -> void
    {
        const auto frames = 1 + _random.below(_max_frames);
        build_events(frames);
        _out.reset(frames);

        // A flush in place of a block, allowed on the audio thread while active. Not trapped: in debug
        // builds clap-helpers' maximal flush validation calls main-thread-only checks from here and
        // formats their complaints (the "thread-error" lines), which allocates. Compiled out in release.
        if (_i.params && _random.chance(0.03)) {
            _out.reset(0);
            t_audio_flush = true;
            _i.params->flush(_i.plugin, &_events.clap, &_out.clap);
            t_audio_flush = false;
            return;
        }

        for (auto& buffer : _in_storage) for (auto f = uint32_t{}; f < frames; ++f) buffer[f] = static_cast<float>(_random.real(-0.5, 0.5));
        auto process = clap_process{
            .steady_time = _steady,
            .frames_count = frames,
            .transport = transport(frames),
            .audio_inputs = _in_buffers.data(),
            .audio_outputs = _out_buffers.data(),
            .audio_inputs_count = static_cast<uint32_t>(_in_buffers.size()),
            .audio_outputs_count = static_cast<uint32_t>(_out_buffers.size()),
            .in_events = &_events.clap,
            .out_events = &_out.clap,
        };
        _steady += frames;

        const auto before = tiny::hosts::trapped_allocations();
        auto status = clap_process_status{};
        {
            const auto scope = tiny::hosts::Trap_scope{};
            status = _i.plugin->process(_i.plugin, &process);
        }
        _result.allocations += tiny::hosts::trapped_allocations() - before;
        ++_result.blocks;
        if (status == CLAP_PROCESS_ERROR) ++_result.errors;
        for (const auto& buffer : _out_storage) {
            for (auto f = uint32_t{}; f < frames; ++f) if (!std::isfinite(buffer[f])) { ++_result.non_finite; break; }
        }
    }
};

// MARK: - editor

// The plug-in's editor, embedded in a window for as long as this lives: create, set_parent, show,
// then hide and destroy.
class Editor_session {
public:
    Editor_session(Instance& instance, tiny::hosts::Window& window) : _i{instance}
    {
        if (!_i.gui || !_i.gui->is_api_supported(_i.plugin, CLAP_WINDOW_API_COCOA, false)) return;
        if (!_i.gui->create(_i.plugin, CLAP_WINDOW_API_COCOA, false)) return;
        _open = true;
        auto parent = clap_window{};
        parent.api = CLAP_WINDOW_API_COCOA;
        parent.cocoa = window.content();
        expect_true(_i.gui->set_parent(_i.plugin, &parent), "gui set_parent failed");
        _i.gui->show(_i.plugin);
        _view = window.editor_view();
    }
    ~Editor_session()
    {
        if (!_open) return;
        _i.gui->hide(_i.plugin);
        _i.gui->destroy(_i.plugin);
    }
    Editor_session(const Editor_session&) = delete;
    auto operator=(const Editor_session&) -> Editor_session& = delete;

    // Some input, then enough of the run loop for frames to draw.
    auto use(Random& random) -> void
    {
        if (_view) tiny::hosts::send_input(_view, random, 3);
        tiny::hosts::pump_main(8ms);
    }

private:
    Instance& _i;
    bool _open{};
    void* _view{};
};

// MARK: - checks

auto check_clean(Fake_host& host, const char* when) -> void
{
    const auto problems = host.take_problems();
    auto text = std::string{};
    for (auto k = size_t{}; k < std::min<size_t>(problems.size(), 5); ++k) text += "\n    " + problems[k];
    expect_true(problems.empty(), std::format("{} logged {} problem(s):{}", when, problems.size(), text));
}

auto check_result(const Engine::Result& r, const char* when) -> void
{
    expect_true(r.errors == 0, std::format("{}: {} blocks returned CLAP_PROCESS_ERROR", when, r.errors));
    expect_true(r.non_finite == 0, std::format("{}: {} blocks wrote NaN or inf", when, r.non_finite));
    expect_true(r.disorder == 0, std::format("{}: {} output events out of order or outside the block", when, r.disorder));
    expect_true(r.allocations == 0, std::format("{}: {} allocations on the audio thread in {} blocks", when, r.allocations, r.blocks));
}

// Service what a real host's main loop would: callbacks, the run loop, restart and flush requests.
auto service_main(Instance& i, Fake_host& host, bool active) -> void
{
    if (host.callback_requested.exchange(false)) i.plugin->on_main_thread(i.plugin);
    if (!active && host.flush_requested.exchange(false) && i.params) {
        auto none = Event_list{};
        none.seal();
        auto out = Out_events{};
        i.params->flush(i.plugin, &none.clap, &out.clap);
    }
    tiny::hosts::pump_main(200us);
}

constexpr auto rates = std::array{44100., 48000., 88200., 96000.};
constexpr auto block_sizes = std::array{32u, 64u, 128u, 512u, 1024u, 4096u};

// MARK: - scenarios

auto add_scenarios(const Bundle& bundle) -> void
{
    Tests::add("lifecycle: scan-style create and destroy, never activated", [&bundle] {
        for (auto n = 0; n < 5; ++n) {
            auto host = std::make_unique<Fake_host>();
            { auto instance = Instance{bundle, *host}; }
            tiny::hosts::pump_main(5ms); // A late callback into the freed host is ASan's to see.
            check_clean(*host, "create/destroy");
        }
    });

    Tests::add("lifecycle: activate, process, deactivate across rates and block sizes", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto instance = Instance{bundle, *host};
            auto random = Random{g_options.seed};
            for (auto cycle = 0; cycle < 8; ++cycle) {
                const auto rate = rates[random.below(rates.size())];
                const auto max_frames = block_sizes[random.below(block_sizes.size())];
                expect_true(instance.plugin->activate(instance.plugin, rate, 1, max_frames), "activate failed");
                auto engine = Engine{instance, *host, g_options.seed + static_cast<uint32_t>(cycle), max_frames};
                engine.start();
                for (auto t = 0; t < 20; ++t) service_main(instance, *host, true);
                check_result(engine.stop(), "processing");
                instance.plugin->deactivate(instance.plugin);
                service_main(instance, *host, false);
            }
        }
        tiny::hosts::pump_main(20ms);
        check_clean(*host, "lifecycle");
    });

    Tests::add("state: save, load, save is identical, whole and trickled, inactive and active", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto instance = Instance{bundle, *host};
            auto random = Random{g_options.seed};
            const auto first = instance.save(random);
            expect_true(!first.empty(), "empty state");
            for (const auto chunk : {0u, 1u, 7u}) {
                expect_true(instance.load(first, random, chunk), std::format("load failed with {}-byte reads", chunk));
                expect_true(instance.save(random, chunk) == first, std::format("state changed across a load with {}-byte streams", chunk));
            }
            expect_true(instance.plugin->activate(instance.plugin, 48000, 1, 512), "activate failed");
            auto engine = Engine{instance, *host, g_options.seed, 512};
            engine.start();
            for (auto n = 0; n < 20; ++n) {
                expect_true(instance.load(first, random, n % 2 ? 3u : 0u), "load while processing failed");
                service_main(instance, *host, true);
            }
            check_result(engine.stop(), "processing");
            instance.plugin->deactivate(instance.plugin);
            expect_true(instance.load(first, random), "load");
            expect_true(instance.save(random) == first, "state changed across loads while processing");
        }
        tiny::hosts::pump_main(20ms);
        check_clean(*host, "state");
    });

    Tests::add("state: truncated and corrupted chunks never crash, and a good one restores", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto instance = Instance{bundle, *host};
            auto random = Random{g_options.seed};
            const auto good = instance.save(random);
            for (auto size = size_t{}; size < good.size(); ++size) {
                const auto prefix = std::vector<uint8_t>(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size));
                (void)instance.load(prefix, random);
            }
            for (auto n = 0; n < 200; ++n) {
                auto bad = good;
                for (auto flips = 1 + random.below(4); flips > 0; --flips) bad[random.below(static_cast<uint32_t>(bad.size()))] ^= static_cast<uint8_t>(1 + random.below(255));
                (void)instance.load(bad, random, random.below(4));
            }
            for (auto n = 0; n < 50; ++n) {
                auto junk = std::vector<uint8_t>(random.below(256));
                for (auto& b : junk) b = static_cast<uint8_t>(random.below(256));
                (void)instance.load(junk, random);
            }
            expect_true(instance.load(good, random), "a good chunk after bad ones must load");
            expect_true(instance.save(random) == good, "a good chunk after bad ones must restore the state exactly");
        }
        tiny::hosts::pump_main(20ms);
        (void)host->take_problems(); // Bad chunks may legitimately log.
    });

    Tests::add("params: displayed text re-parses to the same text, values stay in range", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto instance = Instance{bundle, *host};
            for (const auto& info : instance.param_infos) {
                auto value = 0.;
                expect_true(instance.params->get_value(instance.plugin, info.id, &value), std::format("get_value {}", info.name));
                expect_true(value >= info.min_value && value <= info.max_value, std::format("{} = {} outside [{}, {}]", info.name, value, info.min_value, info.max_value));
                for (auto k = 0; k <= 20; ++k) {
                    auto v = info.min_value + (info.max_value - info.min_value) * k / 20.;
                    if (info.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
                    char text[256]{}, again[256]{};
                    if (!instance.params->value_to_text(instance.plugin, info.id, v, text, sizeof(text))) continue;
                    auto parsed = 0.;
                    expect_true(instance.params->text_to_value(instance.plugin, info.id, text, &parsed), std::format("{}: '{}' didn't parse", info.name, text));
                    expect_true(instance.params->value_to_text(instance.plugin, info.id, parsed, again, sizeof(again)), "value_to_text");
                    expect_true(std::string{text} == again, std::format("{}: '{}' re-parsed as '{}'", info.name, text, again));
                }
            }
        }
        check_clean(*host, "params");
    });

    Tests::add("params: a flush while inactive lands in the reported values", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto instance = Instance{bundle, *host};
            if (!instance.params || instance.param_infos.empty()) return;
            auto random = Random{g_options.seed};
            auto events = Event_list{};
            auto wanted = std::vector<double>{};
            for (const auto& info : instance.param_infos) {
                auto v = random.real(info.min_value, info.max_value);
                if (info.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
                wanted.push_back(v);
                if (!(info.flags & CLAP_PARAM_IS_READONLY)) events.param(0, info, v);
            }
            events.seal();
            auto out = Out_events{};
            instance.params->flush(instance.plugin, &events.clap, &out.clap);
            for (auto k = size_t{}; k < instance.param_infos.size(); ++k) {
                const auto& info = instance.param_infos[k];
                if (info.flags & CLAP_PARAM_IS_READONLY) continue;
                auto value = 0.;
                instance.params->get_value(instance.plugin, info.id, &value);
                expect_true(std::abs(value - wanted[k]) <= 1e-6 * std::max(1., std::abs(wanted[k])), std::format("{}: flushed {} but reports {}", info.name, wanted[k], value));
            }
        }
        check_clean(*host, "flush");
    });

    Tests::add("editor: open, use and close, idle and while processing, with loads while open", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{bundle, *host};
            auto random = Random{g_options.seed};
            const auto chunk = instance.save(random);
            for (auto cycle = 0; cycle < 6; ++cycle) {
                const auto processing = cycle % 2 == 1;
                if (processing) expect_true(instance.plugin->activate(instance.plugin, 48000, 1, 512), "activate failed");
                auto engine = processing ? std::make_unique<Engine>(instance, *host, g_options.seed + static_cast<uint32_t>(cycle), 512) : nullptr;
                if (engine) engine->start();
                {
                    auto editor = Editor_session{instance, window};
                    for (auto k = 0; k < 12; ++k) {
                        editor.use(random);
                        if (k % 4 == 3) (void)instance.load(chunk, random); // A host load reaches an open editor.
                        service_main(instance, *host, processing);
                    }
                }
                if (engine) {
                    check_result(engine->stop(), "processing");
                    instance.plugin->deactivate(instance.plugin);
                }
                service_main(instance, *host, false);
            }
        }
        tiny::hosts::pump_main(50ms);
        check_clean(*host, "editor");
    });

    Tests::add("editor teardown: gui destroyed and plug-in destroyed at once, mid-activity", [&bundle] {
        for (auto n = 0; n < 5; ++n) {
            auto host = std::make_unique<Fake_host>();
            {
                auto window = tiny::hosts::Window{};
                auto instance = Instance{bundle, *host};
                auto random = Random{g_options.seed + static_cast<uint32_t>(n)};
                expect_true(instance.plugin->activate(instance.plugin, 48000, 1, 256), "activate failed");
                auto engine = Engine{instance, *host, g_options.seed + static_cast<uint32_t>(n), 256};
                engine.start();
                {
                    auto editor = Editor_session{instance, window};
                    for (auto k = 0; k < 6; ++k) editor.use(random);
                    check_result(engine.stop(), "processing");
                    instance.plugin->deactivate(instance.plugin);
                } // gui destroyed, then the plug-in, with no run loop in between.
            }
            tiny::hosts::pump_main(30ms); // Late frames or dialog answers now meet freed objects.
            (void)host->take_problems();
        }
    });

    // The TSan target: the audio thread and the main thread both busy, the way a session is.
    Tests::add("chaos: processing with automation while main loads state, queries and restarts", [&bundle] {
        auto host = std::make_unique<Fake_host>();
        {
            auto window = tiny::hosts::Window{};
            auto instance = Instance{bundle, *host};
            auto random = Random{g_options.seed};
            auto editor = std::unique_ptr<Editor_session>{};
            auto chunks = std::vector<std::vector<uint8_t>>{instance.save(random)};
            auto max_frames = 512u;
            expect_true(instance.plugin->activate(instance.plugin, 48000, 1, max_frames), "activate failed");
            auto engine = std::make_unique<Engine>(instance, *host, g_options.seed, max_frames);
            engine->start();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(g_options.chaos_seconds);
            auto cycles = 0, loads = 0, saves = 0;
            auto blocks = long{};
            while (std::chrono::steady_clock::now() < deadline) {
                switch (random.below(9)) {
                    case 0: chunks.push_back(instance.save(random, random.below(8))); ++saves; break;
                    case 1: (void)instance.load(chunks[random.below(static_cast<uint32_t>(chunks.size()))], random, random.below(8)); ++loads; break;
                    case 2:
                        for (const auto& info : instance.param_infos) {
                            auto value = 0.;
                            char text[128]{};
                            instance.params->get_value(instance.plugin, info.id, &value);
                            instance.params->value_to_text(instance.plugin, info.id, value, text, sizeof(text));
                        }
                        break;
                    case 3:
                        if (instance.latency) (void)instance.latency->get(instance.plugin);
                        if (instance.tail) (void)instance.tail->get(instance.plugin);
                        break;
                    case 4:
                        // Deactivate and come back, maybe at a new rate: what a host does on a
                        // restart request, a device change, or a bounce.
                        if (host->restart_requested.exchange(false) || random.chance(0.1)) {
                            const auto r = engine->stop();
                            check_result(r, "processing");
                            blocks += r.blocks;
                            instance.plugin->deactivate(instance.plugin);
                            service_main(instance, *host, false);
                            max_frames = block_sizes[random.below(block_sizes.size())];
                            expect_true(instance.plugin->activate(instance.plugin, rates[random.below(rates.size())], 1, max_frames), "reactivate failed");
                            if (instance.latency) (void)instance.latency->get(instance.plugin); // Only while active.
                            engine = std::make_unique<Engine>(instance, *host, g_options.seed + static_cast<uint32_t>(++cycles), max_frames);
                            engine->start();
                        }
                        break;
                    case 5:
                        if (random.chance(0.15)) {
                            if (editor) editor.reset();
                            else editor = std::make_unique<Editor_session>(instance, window);
                        }
                        else if (editor) editor->use(random);
                        break;
                    default: service_main(instance, *host, true); break;
                }
            }
            editor.reset();
            const auto last = engine->stop();
            check_result(last, "processing");
            blocks += last.blocks;
            instance.plugin->deactivate(instance.plugin);
            service_main(instance, *host, false);
            std::printf("  chaos: %ld blocks, %d loads, %d saves, %d reactivations, %d latency changes\n",
                        blocks, loads, saves, cycles, host->latency_changes.load());
        }
        tiny::hosts::pump_main(50ms);
        check_clean(*host, "chaos");
    });

    Tests::add("teardown: destroyed straight after a burst of activity, with nothing drained", [&bundle] {
        for (auto n = 0; n < 10; ++n) {
            auto host = std::make_unique<Fake_host>();
            {
                auto instance = Instance{bundle, *host};
                auto random = Random{g_options.seed + static_cast<uint32_t>(n)};
                const auto chunk = instance.save(random);
                expect_true(instance.plugin->activate(instance.plugin, 48000, 1, 256), "activate failed");
                auto engine = Engine{instance, *host, g_options.seed + static_cast<uint32_t>(n), 256};
                engine.start();
                for (auto k = 0; k < 5; ++k) (void)instance.load(chunk, random);
                check_result(engine.stop(), "processing");
                instance.plugin->deactivate(instance.plugin);
            } // Destroyed without servicing callbacks or the run loop.
            tiny::hosts::pump_main(10ms); // Anything still in flight now meets a freed host and plug-in.
            (void)host->take_problems();
        }
    });
}

} // namespace

auto main(int argc, char** argv) -> int
{
    g_options = tiny::hosts::parse_options(argc, argv);
    if (g_options.bundle.empty()) {
        std::printf("usage: clap_host <Plugin.clap> [--seed N] [--seconds S]\n");
        return 2;
    }
    std::printf("clap_host: %s, seed %u\n", g_options.bundle.c_str(), g_options.seed);
    tiny::hosts::gui_init();
    try {
        const auto bundle = Bundle{g_options.bundle};
        add_scenarios(bundle);
        return audio_bench::Tests::run_all() == 0 ? 0 : 1;
    }
    catch (const std::exception& e) {
        std::printf("clap_host: %s\n", e.what());
        return 1;
    }
}
