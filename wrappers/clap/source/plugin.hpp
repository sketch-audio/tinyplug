#pragma once

#include <memory>
#include <string>
#include <vector>

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "processor.hpp"
#include "models/meters.hpp"
#include "models/params.hpp"
#include "plug_info.hpp"

#include "adapters.hpp"
#include "view.hpp"

#include <tiny_dsp/host_bypass.hpp>

using MisbehaviourHandler = ::clap::helpers::MisbehaviourHandler; // Studio One appears to be misbehaving.
using CheckingLevel = ::clap::helpers::CheckingLevel;

// Only terminate in debug mode
#if defined(NDEBUG)
using PluginBase = ::clap::helpers::Plugin<MisbehaviourHandler::Ignore, CheckingLevel::None>;
#else
using PluginBase = ::clap::helpers::Plugin<MisbehaviourHandler::Ignore, CheckingLevel::Maximal>;
#endif

namespace tiny::clap {

class Plugin : public PluginBase {
public:

    Plugin(const clap_host* host) : PluginBase{&descriptor, host}, _host{host}
    {
        _editor.emplace(Edit_context{
            .actions = _actions.actor(),
            .format = Format::Clap,
            .state_adapter = _state_adapter.actor(),
            .undo_redo = _undo_history.actor(),
            .tasks = _tasks.actor(),
        });

#if TINY_HAS_WORKER
        try_bind_worker(*_processor, Worker_processor_actor{
            [this](const auto& m) { return _worker_from_proc.push(m); }
        });
        try_bind_worker(*_editor, Worker_editor_actor{
            [this](const auto& m) { return _worker_from_edit.push(m); }
        });
#endif
    }
    ~Plugin() override = default;

    static const inline clap_plugin_descriptor_t descriptor{
        .clap_version = CLAP_VERSION,
        .id = Plug_info::base_identifier,
        .name = Plug_info::plugin_name,
        .vendor = Plug_info::company_name,
        .url = Plug_info::company_website,
        .manual_url = Plug_info::company_website,
        .support_url = Plug_info::company_website,
        .version = Plug_info::version_string,
        .description = Plug_info::Clap::description,
        .features = Plug_info::Clap::features.data()
    };

    // plugin
    bool init() noexcept override;
    bool activate(double sampleRate, uint32_t minFrameCount, uint32_t maxFrameCount) noexcept override;
    void deactivate() noexcept override;
    bool startProcessing() noexcept override;
    void stopProcessing() noexcept override;
    clap_process_status process(const clap_process* process) noexcept override;
    void reset() noexcept override;
    void onMainThread() noexcept override;
    const void* extension(const char* id) noexcept override;
    bool enableDraftExtensions() const noexcept override;

    // render (offline/bounce detection)
    bool implementsRender() const noexcept override { return true; }
    bool renderHasHardRealtimeRequirement() noexcept override { return false; }
    bool renderSetMode(clap_plugin_render_mode mode) noexcept override;

    // state
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream* stream) noexcept override;
    bool stateLoad(const clap_istream* stream) noexcept override;

    // preset load
    bool implementsPresetLoad() const noexcept override { return true; }
    bool presetLoadFromLocation(uint32_t location_kind, const char* location, const char* load_key) noexcept override;

    // audio ports
    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool isInput) const noexcept override;
    bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept override;

    // configurable audio ports
    bool implementsConfigurableAudioPorts() const noexcept override { return true; }
    bool configurableAudioPortsCanApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) const noexcept override;
    bool configurableAudioPortsApplyConfiguration(const clap_audio_port_configuration_request* requests, uint32_t request_count) noexcept override;

    // params
    bool implementsParams() const noexcept override { return true; };
    uint32_t paramsCount() const noexcept override;
    bool paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept override;
    bool paramsValue(clap_id paramId, double* value) noexcept override;
    bool paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept override;
    bool paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept override;
    void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override;

    // gui
    bool implementsGui() const noexcept override { return true; }
    bool guiIsApiSupported(const char* api, bool isFloating) noexcept override;
    bool guiGetPreferredApi(const char** api, bool* isFloating) noexcept override;
    bool guiCreate(const char* api, bool isFloating) noexcept override;
    void guiDestroy() noexcept override;
    bool guiSetScale(double scale) noexcept override;
    bool guiShow() noexcept override;
    bool guiHide() noexcept override;
    bool guiGetSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiCanResize() const noexcept override;
    bool guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept override;
    bool guiAdjustSize(uint32_t* width, uint32_t* height) noexcept override;
    bool guiSetSize(uint32_t width, uint32_t height) noexcept override;
    void guiSuggestTitle(const char* title) noexcept override;
    bool guiSetParent(const clap_window* window) noexcept override;
    bool guiSetTransient(const clap_window* window) noexcept override;

    // latency
    bool implementsLatency() const noexcept override { return true; }
    uint32_t latencyGet() const noexcept override;

    bool implementsTail() const noexcept override { return true; }
    uint32_t tailGet() const noexcept override;

    // MARK: - private

private:

    const clap_host* _host{nullptr};
    bool _once{false}; // Have we been reset?
    double _sr{48000};

    using User_params = params::Infos<models::Params>;
    using User_meters = meters::Infos<models::Meters>;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto num_meters = User_meters::num_meters;

    std::vector<std::string> _modules{tree_to_clap_modules(User_params::param_tree())};

    // IO
    static constexpr auto max_ichannels = size_t{2};
    static constexpr auto max_schannels = size_t{2};
    static constexpr auto max_ochannels = size_t{2};
    size_t _ichannels{max_ichannels};
    size_t _schannels{Plug_info::wants_sidechain ? max_schannels : 0};
    size_t _ochannels{max_ochannels};

    // Pointers to host io buffers.
    std::array<const float*, max_ichannels> _ibuffers{};
    std::array<const float*, max_schannels> _sbuffers{};
    std::array<float*, max_ochannels> _obuffers{};
    std::array<float, num_meters> _meters{};

    // USER
    std::unique_ptr<plugin::Processor> _processor = std::make_unique<plugin::Processor>();
    std::optional<plugin::Editor> _editor{};
    Task_manager _tasks{};

    // GUI
    std::unique_ptr<View> _view{nullptr};

    // Framework-owned editor window-size cache (plug-in lifetime, survives view
    // recreation). Primed from persisted state on load; reports the initial size so
    // the window reopens pre-sized. Updated on every host-echoed resize (guiSetSize).
    std::optional<Rect_size> _last_size{};

    // Undo history and action queue live here (plug-in lifetime), not in the view,
    // so the editor's Edit_context (built once at construction) stays valid across
    // window open/close and host preset loads are captured with the window closed.
    Undo_history _undo_history{};
    Action_queue _actions{};

    // Snapshot all current param values in knob space (for host-load undo diffs).
    auto _snapshot_knob_params() const -> std::array<double, num_params>
    {
        auto out = std::array<double, num_params>{};
        for (auto i = decltype(num_params){}; i < num_params; ++i) {
            const auto& param = User_params::param_spec(i);
            const auto host_value = _hostvalues[i].load(std::memory_order_relaxed);
            out[i] = params::Value_helper::host_to_knob(host_value, param.semantics);
        }
        return out;
    }

    // Latency
    uint32_t _latency{_processor->latency_samps()};
    uint32_t _reported_latency{_latency}; // Don't feedback latency changes.

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // Render mode (offline/bounce). Set off the audio thread via renderSetMode,
    // read on the audio thread in process.
    std::atomic<bool> _offline{false};

    // Fallback sample clock for a null transport when the host has no steady_time
    // either. Render-thread only.
    int64_t _free_run_pos{};

    // Tail
    uint32_t _tail{_processor->tail_samps()};

    // Values in host space.
    using Host_value = std::atomic<double>;
    using Host_values = std::array<Host_value, num_params>;

    using enum params::Space;
    Host_values _hostvalues{tiny::params::make_defaults<Host_value, User_params>(Host)};

    std::array<double, num_meters> _last_meters{};

    static constexpr auto queue_size = 4 * num_params + 1; // State only
    static constexpr auto meter_size = 25 * num_meters + 1;

    using From_flush_queue = Lock_free_queue<Render_event, queue_size>; //
    using From_ui_queue = Lock_free_queue<User_action, queue_size>;
    using Meter_queue = Lock_free_queue<Set_meter, meter_size>;

    From_flush_queue _from_flush{};
    From_ui_queue _from_ui{};

    Meter_queue _meter_queue{};

    // Resync mechanism. Currently only a fallback in case we overflow our queue in release.
    std::atomic<bool> _needs_clear{false}; // Set by `reset`, consumed at the top of `process`.
    std::atomic<bool> _needs_resync{false};
    bool _was_skipped{}; // Render-thread only. Detects the can_skip -> processing edge.
    std::optional<Render_mode> _last_render_mode{}; // Render-thread only. Detects the realtime <-> offline edge.

#if TINY_HAS_WORKER
    // Worker channel.
    using Worker_from_proc_q = Lock_free_queue<typename User_worker::Model::From_processor, User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_from_edit_q = Lock_free_queue<typename User_worker::Model::From_editor,    User_worker::Model::inbound_capacity, Queue_concurrency::spsc>;
    using Worker_to_proc_q   = Lock_free_queue<typename User_worker::Model::To_processor,   User_worker::Model::outbound_capacity>;
    using Worker_to_edit_q   = Lock_free_queue<typename User_worker::Model::To_editor,     User_worker::Model::outbound_capacity>;

    Worker_from_proc_q _worker_from_proc{};
    Worker_from_edit_q _worker_from_edit{};
    Worker_to_proc_q   _worker_to_proc{};
    Worker_to_edit_q   _worker_to_edit{};

    User_worker _worker{
        Worker_replies{
            [this](const auto& m) { return _worker_to_proc.push(m); },
            [this](const auto& m) { return _worker_to_edit.push(m); }
        },
        _tasks.actor()
    };
    Worker_runner<User_worker> _worker_runner{&_worker, &_worker_from_proc, &_worker_from_edit};
#endif

    auto _drain_worker_to_processor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_processor(*_processor, _worker_to_proc);
#endif
    }

    auto _drain_worker_to_editor() -> void
    {
#if TINY_HAS_WORKER
        try_drain_worker_to_editor(*_editor, _worker_to_edit);
#endif
    }

    Host_bypass _bypass{};

    State_adapter _state_adapter{{
        .load_model = []() {
            return State_adapter::Load_model{
                .param_tree = &User_params::param_tree(),
                .num_params = User_params::num_params,
            };
        },
        .save_model = [this]() {
            const auto knob = _snapshot_knob_params();
            return State_adapter::Save_model{
                .version = 1,
                .param_tree = &User_params::param_tree(),
                .param_values = std::vector<double>(knob.begin(), knob.end()),
                .editor_state = _editor ? _editor->save_state() : State_map{}
            };
        },
    }};

    // MARK: - private

    auto _resolve_transport(const clap_process* process) -> Musical_context;
    auto _read_state_chunk(const clap_istream* stream) -> bool;

    auto _update_state(const Maybe_values<double>& knob_values, const State_map& editor_state) -> void;
    auto _handle_host_flushed(bool needs_resync) -> void;
    auto _handle_user_actions(const clap_output_events_t* out_events, bool needs_resync) -> void;
    auto _handle_user_action(const User_action& action) -> void;

    // This is where we handle host events from automation or flush.
    // - Kernel needs the plain value.
    // - Host-facing atomics need updated.
    // - UI needs the knob value.
    template<bool on_audio_thread>
    auto _handle_host_event(const clap_event_header* event) -> void
    {
        using namespace params;

        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

        switch (event->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* value_event = reinterpret_cast<const clap_event_param_value*>(event);
                const auto id = value_event->param_id;

                if (id == Reserved::bypass_id) {
                    const auto bypass = value_event->value >= 0.5;
                    _bypass.set_bypassed(bypass);
                    return;
                }

                if (id >= num_params) return;
                const auto& param = User_params::param_spec(id);

                // Send plain value to kernel.
                const auto plain_value = Value_helper::host_to_plain(value_event->value, param.semantics);
                if constexpr (on_audio_thread) {
                    // On the audio thread we can handle the event now.
                    _processor->handle_event(Set_param{.address = id, .value = plain_value});
                }
                else {
                    // On flush, we need to push into a queue for later.
                    [[maybe_unused]] const auto success = _from_flush.push(Set_param{.address = id, .value = plain_value});
                    assert(success && "Push to flush queue failed! Increase queue size.");
                    if (!success) _needs_resync.store(true, std::memory_order_relaxed); // Resync from _hostvalues on the next process.
                }

                // Maintain host atomics.
                _hostvalues[id].store(value_event->value, std::memory_order_relaxed);

                break;
            }
            default:
                break;
        }
    }
};

} // namespace tiny::clap