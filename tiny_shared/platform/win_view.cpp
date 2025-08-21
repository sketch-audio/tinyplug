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
#include <Windowsx.h>

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
    auto* binder = reinterpret_cast<Platform_binder*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!binder) return DefWindowProcW(window, message, wParam, lParam);

    auto* delegate = binder->delegate;
    const auto h = delegate ? delegate->get_size().h : 0;
    
    switch (message)
    {
        case WM_PAINT: {
            if (delegate) {
                auto ps = PAINTSTRUCT{};
                BeginPaint(window, &ps);
                // Should we dwell?
                if (const auto over_pos = binder->over_pos; over_pos && !binder->dwelt) {
                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - binder->over_time);
                    if (elapsed.count() > 2000) {
                        binder->dwelt = try_set(binder->interaction.state, Dwell{*over_pos});
                    }
                }
                delegate->draw(binder->interaction); // Delegate window context handles everything.
                try_set(binder->interaction.state, Consumed{});
                if (binder->dwelt) {
                    binder->over_pos = std::nullopt;
                    binder->dwelt = false;
                }
                EndPaint(window, &ps);
            }
            return 0;
        }

        case WM_TIMER: {
            InvalidateRect(window, nullptr, TRUE);
            UpdateWindow(window);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(window);
            const auto x = static_cast<double>(GET_X_LPARAM(lParam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lParam));
            binder->left_pos = Coords{x, y};
            return 0;
        }

        case WM_LBUTTONUP: {
            const auto x = static_cast<double>(GET_X_LPARAM(lParam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lParam));
            const auto pos = Coords{x, y};
            if (const auto drag_start = binder->drag_start) {
                try_set(binder->interaction.state, Drag_end{*drag_start, pos});
                binder->over_pos = std::nullopt;
                binder->drag_start = std::nullopt;
            } 
            else if (const auto left_pos = binder->left_pos) {
                try_set(binder->interaction.state, Click{pos});
                binder->over_pos = std::nullopt;
            }
            binder->left_pos = std::nullopt;
            ReleaseCapture();
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            const auto x = static_cast<double>(GET_X_LPARAM(lParam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lParam));
            const auto pos = Coords{x, y};
            try_set(binder->interaction.state, tiny::Double_click{pos});
            binder->over_pos = std::nullopt; 
            return 0;
        }

        case WM_MOUSEMOVE: {
            const auto x = static_cast<double>(GET_X_LPARAM(lParam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lParam));
            const auto pos = Coords{x, y};
            // Drag or drag start.
            if (const auto left_pos = binder->left_pos) {
                if (const auto drag_start = binder->drag_start) {
                    try_set(binder->interaction.state, Drag{*drag_start, pos});
                    binder->over_pos = std::nullopt;
                }
                else {
                    binder->drag_start = *left_pos;
                    try_set(binder->interaction.state, Drag_start{*left_pos, pos});
                    binder->over_pos = std::nullopt;
                }
            }
            // Over.
            else {
                try_set(binder->interaction.state, Over{pos});

                // Update dwell.
                const auto& over_pos = binder->over_pos;
                if (!over_pos || *over_pos != pos) {
                    binder->over_pos = pos;
                    binder->over_time = std::chrono::steady_clock::now();
                    binder->dwelt = false;
                }
            }
            
            return 0;
        }

        case WM_RBUTTONDOWN: {
            SetCapture(window);
            const auto x = static_cast<double>(GET_X_LPARAM(lParam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lParam));
            binder->right_pos = Coords{x, y};
            return 0;
        }

        case WM_RBUTTONUP: {
            const auto x = static_cast<double>(GET_X_LPARAM(lParam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lParam));
            const auto pos = Coords{x, y};
            if (const auto right_pos = binder->right_pos) {
                try_set(binder->interaction.state, Right_click{pos});
                binder->over_pos = std::nullopt;
                binder->right_pos = std::nullopt;
            }
            ReleaseCapture();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const auto delta = GET_WHEEL_DELTA_WPARAM(wParam) * 1.f / WHEEL_DELTA;
            binder->interaction.scroll_deltas.y = delta;
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            const auto delta = GET_WHEEL_DELTA_WPARAM(wParam) * 1.f / WHEEL_DELTA;
            binder->interaction.scroll_deltas.x = delta;
            return 0;
        }

        case WM_MOUSELEAVE: {
            binder->interaction.state = Consumed{};
            binder->over_pos = std::nullopt;
            binder->left_pos = std::nullopt;
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

    _binder.delegate = _delegate.get();
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&_binder));

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