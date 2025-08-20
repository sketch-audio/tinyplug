#pragma once

#include <memory>
#include <string>
#include <vector>

#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "param_model.h"
#include "plug_info.h"

#include "clap_adapters.h"
#include "clap_kernel.h"
#include "clap_view.h"

using MisbehaviourHandler = clap::helpers::MisbehaviourHandler; // Studio One appears to be misbehaving.
using CheckingLevel = clap::helpers::CheckingLevel;
using PluginBase = clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal>;

namespace tiny {

class Clap_plugin : public PluginBase {
public:

    Clap_plugin(const clap_host* host) : PluginBase{&descriptor, host},
        _host{host},
        _kernel{std::make_unique<Clap_kernel>(_host)}
    {};
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
    bool implementsAudioPorts() const noexcept override;
    uint32_t audioPortsCount(bool isInput) const noexcept override;
    bool audioPortsInfo(uint32_t index, bool isInput, clap_audio_port_info* info) const noexcept override;

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

    // MARK: - private

private:

    const clap_host* _host{nullptr};

    using User_params = Param_infos<Param_model>;
    static constexpr auto num_params = User_params::num_params;

    User_params _param_infos{};
    std::vector<std::string> _modules{tree_to_clap_modules(_param_infos.tree())};

    std::unique_ptr<Clap_kernel> _kernel = {nullptr}; // Now requires the host.

    std::unique_ptr<Clap_view> _view = std::make_unique<Clap_view>(Ui_receiver{
        .get_knob_value = [this](auto id) {
            const auto& param = _param_infos.param_for(id);
            const auto host_value = _kernel->get_host_value(id);
            const auto knob_value = Value_conv::host_to_knob(host_value, param.semantics);
            return knob_value;
        },
        .pop_event = [this](auto& event) {
            return _kernel->pop_export(event);
        },
        .action_handler = [this](auto& action) {
            _kernel->handle_action(action);
        }
    });

    // MARK: - gui api

    // Preferred platform GUI API (used in a couple places).
    static constexpr auto gui_preferred_api = []() {
        if (Platform::resolved == Platform::Type::macos) {
            return CLAP_WINDOW_API_COCOA;
        }
        else if (Platform::resolved == Platform::Type::windows) {
            return CLAP_WINDOW_API_WIN32;
        }
        else {
            return CLAP_WINDOW_API_X11; // Not yet supported.
        }
    }();

};

} // namespace tiny