#pragma once

#include <memory>
#include <string>
#include <vector>

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "plug_processor.h"
#include "models/meter_model.h"
#include "models/param_model.h"
#include "plug_info.h"

#include "clap_adapters.h"
#include "clap_view.h"

using MisbehaviourHandler = clap::helpers::MisbehaviourHandler; // Studio One appears to be misbehaving.
using CheckingLevel = clap::helpers::CheckingLevel;
using PluginBase = clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal>;

namespace tiny {

class Clap_plugin : public PluginBase {
public:

    Clap_plugin(const clap_host* host) : PluginBase{&descriptor, host}, _host{host} {};
    ~Clap_plugin() override = default;

    static const inline clap_plugin_descriptor_t descriptor{
        .clap_version = CLAP_VERSION,
        .id = Plug_info::base_identifier,
        .name = Plug_info::product_name,
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

    // state
    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream* stream) noexcept override;
    bool stateLoad(const clap_istream* stream) noexcept override;

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

    using User_params = Param_infos<Param_model>;
    using User_meters = Meter_infos<Meter_model>;
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
    std::unique_ptr<Plug_processor> _processor = std::make_unique<Plug_processor>();
    std::shared_ptr<Plug_editor> _editor = std::make_shared<Plug_editor>();

    // GUI
    std::unique_ptr<Clap_view> _view{nullptr};

    // Latency 
    uint32_t _latency{_processor->latency_samps()};

    using Latency_flag = std::atomic<std::optional<uint32_t>>;
    static_assert(Latency_flag::is_always_lock_free);

    // Communicates the pending latency from `process` to `setActive`.
    Latency_flag _pending_latency{};

    // Communicates the accepted latency from `setActive` to `process`.
    Latency_flag _accepted_latency{};

    // Tail
    uint32_t _tail{_processor->tail_samps()};

    // Values in host space.
    using Host_value = std::atomic<double>;
    using Host_values = std::array<Host_value, num_params>;
    Host_values _hostvalues{User_params::make_defaults<Host_value>(Value_space::Host)};

    std::array<double, num_meters> _last_meters{};

    static constexpr auto to_processor_size = 2 * num_params + 1;
    static constexpr auto to_editor_size = num_params + num_meters + 1;

    using From_flush_queue = Lock_free_queue<Render_event, to_processor_size>;
    using From_ui_queue = Lock_free_queue<User_action, to_processor_size>;
    using To_editor_queue = Overwrite_queue<Ui_event, to_editor_size>;

    From_flush_queue _from_flush{};
    From_ui_queue _from_ui{};

    To_editor_queue _to_editor{};

    // MARK: - private

    auto _handle_host_flushed() -> void;
    auto _handle_user_actions(const clap_output_events_t* out_events) -> void;
    auto _handle_user_action(const User_action& action) -> void;

    // This is where we handle host events from automation or flush.
    // - Kernel needs the plain value.
    // - Host-facing atomics need updated.
    // - UI needs the knob value.
    template<bool on_audio_thread>
    auto _handle_host_event(const clap_event_header* event) -> void
    {
        if (event->space_id != CLAP_CORE_EVENT_SPACE_ID) return;

        switch (event->type) {
            case CLAP_EVENT_PARAM_VALUE: {
                const auto* value_event = reinterpret_cast<const clap_event_param_value*>(event);
                const auto id = value_event->param_id;
                const auto& param = User_params::param_spec(id);

                // Send plain value to kernel.
                const auto plain_value = Value_conv::host_to_plain(value_event->value, param.semantics);
                if constexpr (on_audio_thread) {
                    // On the audio thread we can handle the event now.
                    _processor->handle_event(Set_param{.address = id, .value = plain_value});
                }
                else {
                    // On flush, we need to push into a queue for later.
                    [[maybe_unused]] const auto success = _from_flush.push(Set_param{.address = id, .value = plain_value});
                    assert(success && "Push to flush queue failed! Increase queue size.");
                }

                // Maintain host atomics.
                _hostvalues[id].store(value_event->value, std::memory_order_relaxed);

                // Notify UI.
                const auto knob_value = Value_conv::host_to_knob(value_event->value, param.semantics);
                _to_editor.push(Set_param{.address = id, .value = knob_value});

                break;
            }
        }
    }

    // MARK: - gui api

    // Preferred platform GUI API (used in a couple places).
    static constexpr auto gui_preferred_api = []() {
        if constexpr (Platform::resolved == Platform::Type::macos) {
            return CLAP_WINDOW_API_COCOA;
        }
        else if constexpr (Platform::resolved == Platform::Type::windows) {
            return CLAP_WINDOW_API_WIN32;
        }
        else {
            return CLAP_WINDOW_API_X11; // Not yet supported.
        }
    }();

};

} // namespace tiny