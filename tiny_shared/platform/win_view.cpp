#include "platform.h"
#include "platform_view.h"

#if PLATFORM_WINDOWS

#include <random>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#define SK_DIRECT3D
#include "../skia/win/WindowContextFactory_win.h"

#define CALLBACK_TIMER_ID 1001

namespace tiny {

inline auto gen_random_name() -> std::wstring
{
    static constexpr const wchar_t prefix[] = L"tiny_";
    static constexpr auto body_len = size_t{16};

    // Random engine
    static thread_local auto rng = std::mt19937{std::random_device{}()};
    static constexpr const wchar_t charset[] = L"abcdefghijklmnopqrstuvwxyz0123456789";
    static constexpr auto charset_size = sizeof(charset) / sizeof(charset[0]) - 1;

   auto dist = std::uniform_int_distribution<size_t>{0, charset_size - 1};

    // Build the result
    auto result = std::wstring{prefix};
    result.reserve(result.size() + body_len);
    for (size_t i = 0; i < body_len; ++i) {
        result.push_back(charset[dist(rng)]);
    }

    return result;
}

inline auto get_monitor_refresh_rate(HWND window) -> int32_t
{
    auto monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    auto info = MONITORINFOEXW{sizeof(MONITORINFOEXW)};
    if (GetMonitorInfoW(monitor, &info)) {
        auto dev_mode = DEVMODEW{};
        dev_mode.dmSize = sizeof(DEVMODEW);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &dev_mode)) {
            return dev_mode.dmDisplayFrequency;
        }
    }
    return 60; // fallback
}

// C-style window callback
LRESULT CALLBACK window_callback(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    // Retrieve the graphics delegate stored in window's user data.
    auto* delegate = reinterpret_cast<View_delegate*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!delegate) return DefWindowProcW(window, message, wParam, lParam);
    
    switch (message)
    {
        case WM_PAINT: {
            if (delegate) {
                auto ps = PAINTSTRUCT{};
                BeginPaint(window, &ps);
                delegate->draw(User_interaction{}); // Delegate window context handles everything.
                EndPaint(window, &ps);
            }
            return 0;
        }

        case WM_TIMER: {
            InvalidateRect(window, nullptr, TRUE);
            UpdateWindow(window);
            return 0;
        }
        
        // Add other message handlers as needed
        
        default:
            return DefWindowProcW(window, message, wParam, lParam);
    }
}

// Singleton to handle registration/unregistration of window class.
struct Window_registrar {

    static auto instance() -> Window_registrar&
    {
        static auto _instance = Window_registrar{};
        return _instance;
    }
    
    const std::wstring class_name = gen_random_name();
    const std::wstring window_name = gen_random_name();

private:

    Window_registrar() 
    {
        const auto window_class = WNDCLASSW{
            .style = CS_DBLCLKS | CS_OWNDC,
            .lpfnWndProc = window_callback,
            .lpszClassName = class_name.c_str(),
        };
		RegisterClassW(&window_class);
    };

public:

    ~Window_registrar()
    {
        UnregisterClassW(class_name.c_str(), 0);
    }

};

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view) : _delegate{delegate}, _owns_view{owns_view}
{
    const auto& registrar = Window_registrar::instance(); // Register/unregisters the window class.

    const auto size = delegate->get_size();

    auto* window = CreateWindowW(
        registrar.class_name.c_str(), 
        registrar.window_name.c_str(), 
        WS_CHILD | WS_CLIPSIBLINGS, 
        CW_USEDEFAULT, 
        CW_USEDEFAULT, 
        size.w,
        size.h, 
        GetDesktopWindow(), 
        nullptr, 
        nullptr, // hInstance optional per Microsoft Docs.
        nullptr
    );
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(_delegate.get()));

    auto display_params = std::make_unique<const skwindow::DisplayParams>();
    auto context = skwindow::MakeD3D12ForWin(window, std::move(display_params));
    _delegate->set_context(std::move(context));

    // Start timer for refresh rate synchronization
    const auto refreshRate = get_monitor_refresh_rate(window);
    const auto timerInterval = 1000 / refreshRate;
    SetTimer(window, CALLBACK_TIMER_ID, timerInterval, nullptr);

    _view = window;
}

Platform_view::~Platform_view()
{
    auto* window = static_cast<HWND>(_view);
    KillTimer(window, CALLBACK_TIMER_ID);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
}

auto Platform_view::receive_parent(void* parent) -> void
{
    auto parent_window = static_cast<HWND>(parent);
    auto window = static_cast<HWND>(_view);
    
    // Set parent window
    SetParent(window, parent_window);
    
    // Show the window
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
}

auto Platform_view::teardown() -> void
{
    //
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    auto window = static_cast<HWND>(_view);
    // Update window size based on delegate
    const auto size = _delegate->get_size();
    SetWindowPos(window, nullptr, 0, 0, size.w, size.h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

auto Platform_view::redraw() -> void
{
    auto window = static_cast<HWND>(_view);
    InvalidateRect(window, nullptr, TRUE);
    UpdateWindow(window); 
}

} // namespace tiny

#endif