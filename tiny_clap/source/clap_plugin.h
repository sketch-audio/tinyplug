#pragma once

#include <memory>

#include "clap/helpers/plugin.hh"

#include "plug_info.h"
#include "platform/platform_view.h"
#include "user_plug.h"

using MisbehaviourHandler = clap::helpers::MisbehaviourHandler;
using CheckingLevel = clap::helpers::CheckingLevel;

class Clap_plugin : public clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal> {
public:

    using Super = clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal>;
    Clap_plugin(const clap_host* host);
    ~Clap_plugin();

    static const inline clap_plugin_descriptor_t descriptor{
        .clap_version = CLAP_VERSION,
        .id = tiny::Plug_info::base_identifier,
        .name = tiny::Plug_info::product_name,
        .vendor = tiny::Plug_info::company_name,
        .url = tiny::Plug_info::company_website,
        .manual_url = tiny::Plug_info::company_website,
        .support_url = tiny::Plug_info::company_website,
        .version = tiny::Plug_info::version_string,
        .description = tiny::Plug_info::Clap::description,
        .features = tiny::Plug_info::Clap::features.data()
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
    const void *extension(const char *id) noexcept override;
    bool enableDraftExtensions() const noexcept override;    

    // audio ports
    bool implementsAudioPorts() const noexcept override { return true; }
    uint32_t audioPortsCount(bool /*isInput*/) const noexcept override { return 1; }
    bool audioPortsInfo(uint32_t index, bool /*isInput*/, clap_audio_port_info* info) const noexcept override
    {
        if (index > 0) return false;
        info->id = index;
        strcpy(info->name, "Audio Port");
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        return true;
    }

    bool implementsState() const noexcept override { return true; }
    bool stateSave(const clap_ostream* /*stream*/) noexcept override { return true; }
    bool stateLoad(const clap_istream* /*stream*/) noexcept override { return true; }

    // clap_plugin_params
    bool implementsParams() const noexcept override { return true; };
    uint32_t paramsCount() const noexcept override;
    bool paramsInfo(uint32_t paramIndex, clap_param_info* info) const noexcept override;
    bool paramsValue(clap_id paramId, double* value) noexcept override;
    bool paramsValueToText(clap_id paramId, double value, char* display, uint32_t size) noexcept override;
    bool paramsTextToValue(clap_id paramId, const char* display, double* value) noexcept override;
    void paramsFlush(const clap_input_events* in, const clap_output_events* out) noexcept override;
    // int32_t getParamIndexForParamId(clap_id paramId) const noexcept override;
    // bool isValidParamId(clap_id paramId) const noexcept override;
    // bool getParamInfoForParamId(clap_id paramId, clap_param_info* info) const noexcept override;

    // clap_plugin_gui 
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

protected:

    auto handle_event(const clap_event_header* event) -> void;

    std::shared_ptr<Graphics_delegate> _delegate = std::make_shared<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    std::unique_ptr<Platform_view> platform_view{nullptr};

    // Sorted by paramId.
    std::vector<tiny::Param_model::Spec> _specs{};

    // Values in host (linearized) space.
    tiny::Param_model::Param_values _hostvalues{};

    // Preferred platform GUI API.
    static constexpr auto gui_preferred_api = []() {
        if (Platform::resolved == Platform::Type::macos) {
            return CLAP_WINDOW_API_COCOA;
        } else if (Platform::resolved == Platform::Type::windows) {
            return CLAP_WINDOW_API_WIN32;
        }
    }();

};