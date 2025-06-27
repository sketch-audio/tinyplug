#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "clap_plugin.h"

#include "platform_view.h"

Clap_plugin::Clap_plugin(const clap_host* host)
    : clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal>(&descriptor, host)
{
}

Clap_plugin::~Clap_plugin()
{
}

bool Clap_plugin::guiIsApiSupported(const char* api, bool isFloating) noexcept
{
    return !isFloating && strcmp(api, gui_preferred_api) == 0;
}

bool Clap_plugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept
{
    *api = gui_preferred_api;
    *isFloating = false;
    return true;
}

bool Clap_plugin::guiCreate(const char* /*api*/, bool /*isFloating*/) noexcept
{
    platform_view = std::make_unique<Platform_view>(_delegate);
    return true;
}

void Clap_plugin::guiDestroy() noexcept
{
    platform_view = nullptr;
}

bool Clap_plugin::guiSetScale(double /*scale*/) noexcept
{
    return true;
}

bool Clap_plugin::guiShow() noexcept
{
    return true;
}

bool Clap_plugin::guiHide() noexcept
{
    return true;
}

bool Clap_plugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept
{
    const auto delegate_size = _delegate->getSize();
    *width = delegate_size.width;
    *height = delegate_size.height;
    return true;
}

bool Clap_plugin::guiCanResize() const noexcept
{
    return true;
}

bool Clap_plugin::guiGetResizeHints(clap_gui_resize_hints_t* /*hints*/) noexcept
{
    // *hints = {
    //     .can_resize_horizontally = true,
    //     .can_resize_vertically = true,
    //     .preserve_aspect_ratio = false,
    //     .aspect_ratio_width = 0,
    //     .aspect_ratio_height = 0
    // };
    return true;
}

bool Clap_plugin::guiAdjustSize(uint32_t* /*width*/, uint32_t* /*height*/) noexcept
{
    return true;
}

bool Clap_plugin::guiSetSize(uint32_t width, uint32_t height) noexcept
{
    _delegate->onResize({static_cast<int>(width), static_cast<int>(height)});
    platform_view->resize(width, height);
    return true;
}

void Clap_plugin::guiSuggestTitle(const char* /*title*/) noexcept
{
    // floating only
}

bool Clap_plugin::guiSetParent(const clap_window* window) noexcept
{
    // Resolve the platform window type.
    auto* platform_window = [=]() {
        if (Platform::resolved == Platform::Type::macos) {
            return window->cocoa;
        } else if (Platform::resolved == Platform::Type::windows) {
            return window->win32;
        }
    }();
    platform_view->receive_parent(platform_window);
    return true;
}

bool Clap_plugin::guiSetTransient(const clap_window* /*window*/) noexcept
{
    return false; // floating only
}