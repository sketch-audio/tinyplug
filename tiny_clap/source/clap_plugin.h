#pragma once

#include <memory>

#include "clap/helpers/plugin.hh"

#include "cmake_defines.h"
#include "platform_view.h"
#include "user_plug.h"

using MisbehaviourHandler = clap::helpers::MisbehaviourHandler;
using CheckingLevel = clap::helpers::CheckingLevel;

class Clap_plugin : public clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal> {
public:

    Clap_plugin(const clap_host* host);
    ~Clap_plugin();

    static const inline clap_plugin_descriptor_t descriptor{
        .clap_version = CLAP_VERSION,
        .id = tiny::Cmake_defines::base_identifier.c_str(),
        .name = tiny::Cmake_defines::product_name.c_str(),
        .vendor = tiny::User_plug::info.company_name.c_str(),
        .url = tiny::User_plug::info.company_website.c_str(),
        .manual_url = tiny::User_plug::info.company_website.c_str(),
        .support_url = tiny::User_plug::info.company_website.c_str(),
        .version = tiny::Cmake_defines::version_string.c_str(),
        .description = tiny::User_plug::info.clap_description.c_str(),
        .features = tiny::User_plug::info.clap_features.data()
    };

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
    
    std::unique_ptr<Graphics_delegate> _delegate = std::make_unique<Graphics_delegate>(Graphics_delegate::Size{800, 600});
    void* platform_view;

};