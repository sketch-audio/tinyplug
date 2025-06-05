#include "clap/helpers/plugin.hh"
#include "clap/helpers/plugin.hxx"
#include "clap/helpers/host-proxy.hh"
#include "clap/helpers/host-proxy.hxx"

#include "plugin.h"

#include "platform_view.h"

namespace tiny {

Plugin::Plugin(const clap_host* host)
    : clap::helpers::Plugin<MisbehaviourHandler::Terminate, CheckingLevel::Maximal>(&descriptor, host)
{
}

Plugin::~Plugin()
{
}

bool Plugin::guiIsApiSupported(const char* api, bool isFloating) noexcept
{
    return !isFloating && strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}

bool Plugin::guiGetPreferredApi(const char** api, bool* isFloating) noexcept
{
    *api = CLAP_WINDOW_API_COCOA;
    *isFloating = false;
    return true;
}

bool Plugin::guiCreate(const char* api, bool isFloating) noexcept
{
    platform_view = CreatePlatformView(800, 600);
    return true;
}

void Plugin::guiDestroy() noexcept
{
    DestroyPlatformView(platform_view);
    platform_view = nullptr;
}

bool Plugin::guiSetScale(double scale) noexcept
{
    return true;
}

bool Plugin::guiShow() noexcept
{
    return true;
}

bool Plugin::guiHide() noexcept
{
    return true;
}

bool Plugin::guiGetSize(uint32_t* width, uint32_t* height) noexcept
{
    *width = 800;
    *height = 600;
    return true;
}

bool Plugin::guiCanResize() const noexcept
{
    return true;
}

bool Plugin::guiGetResizeHints(clap_gui_resize_hints_t* hints) noexcept
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

bool Plugin::guiAdjustSize(uint32_t* width, uint32_t* height) noexcept
{
    return true;
}

bool Plugin::guiSetSize(uint32_t width, uint32_t height) noexcept
{
    return true;
}

void Plugin::guiSuggestTitle(const char* title) noexcept
{
    // floating only
}

bool Plugin::guiSetParent(const clap_window* window) noexcept
{
    AttachPlatformView((void*)window->cocoa, platform_view);
    return true;
}

bool Plugin::guiSetTransient(const clap_window* window) noexcept
{
    return false; // floating only
}

}