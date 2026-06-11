#include "../platform_view.hpp"

#include <algorithm>
#include <random>
#include <ranges>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Windowsx.h>

#include <CommCtrl.h>
#include <commdlg.h> // Dialogs
#include <dwmapi.h>
#include <shellapi.h>

#pragma comment(lib,"comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#include "../window_context.hpp"
#include "win_config.hpp" // WIN_GRAPHICS_GPU
#include "win_internal.hpp"

namespace tiny {

// MARK: - dark mode

auto is_dark_mode() -> bool
{
    auto value = DWORD{1};
    auto size = DWORD{sizeof(value)};

    const auto* path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    const auto* name = L"AppsUseLightTheme";

    if (RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        return value == 0; // 0 = dark
    }

    return false;
}

auto enable_dark_title_bar(HWND hwnd, bool dark) -> void
{
    auto useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
}

class Dark_mode_watcher {
public:

    explicit Dark_mode_watcher(std::function<void(bool)> callback) : _callback{std::move(callback)} {
        _worker = std::thread([this]() { watch_loop(); });
    }

    ~Dark_mode_watcher() {
        _stop.store(true, std::memory_order_release);
        if (_hevent) {
            SetEvent(_hevent); // Don't hang.
        }
        if (_worker.joinable()) {
            _worker.join();
        }
    }

    // no copy no move
    Dark_mode_watcher(const Dark_mode_watcher&) = delete;
    Dark_mode_watcher& operator=(const Dark_mode_watcher&) = delete;
    Dark_mode_watcher(Dark_mode_watcher&&) = delete;
    Dark_mode_watcher& operator=(Dark_mode_watcher&&) = delete;

private:

    auto watch_loop() -> void
    {
        auto hkey = HKEY{};
        const auto* path = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
        if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &hkey) != ERROR_SUCCESS)
            return;

        _hevent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!_hevent) {
            RegCloseKey(hkey);
            return;
        }

        while (!_stop.load(std::memory_order_acquire)) {

            if (RegNotifyChangeKeyValue(hkey, FALSE, REG_NOTIFY_CHANGE_LAST_SET, _hevent, TRUE) != ERROR_SUCCESS)
                break;

            const auto wait = WaitForSingleObject(_hevent, INFINITE);

            if (wait == WAIT_OBJECT_0 && !_stop.load(std::memory_order_acquire))
            {
                const auto dark = is_dark_mode();
                _callback(dark);
            }
        }

        CloseHandle(_hevent);
        _hevent = nullptr;
        RegCloseKey(hkey);
    }

    HANDLE _hevent{nullptr}; // So we can wake up the thread on destruction.
    std::atomic<bool> _stop{false};
    std::function<void(bool)> _callback;
    std::thread _worker;

};

// MARK: - vsync

class Vsync_loop {
public:

    explicit Vsync_loop(std::function<void()> callback) : _thread{[this, callback]() { run(callback); }} {}

    ~Vsync_loop() {
        stop();
    }

    // no copy no move
    Vsync_loop(const Vsync_loop&) = delete;
    Vsync_loop& operator=(const Vsync_loop&) = delete;
    Vsync_loop(Vsync_loop&& other) noexcept = delete;
    Vsync_loop& operator=(Vsync_loop&& other) noexcept = delete;

private:

    std::atomic<bool> _stop{false};
    std::thread _thread;

    auto run(std::function<void()> callback) -> void
    {
        while (!_stop.load(std::memory_order_acquire)) {
            auto hr = DwmFlush();
            if (SUCCEEDED(hr)) {
                callback();
            } 
            else {
                Sleep(16); // Fallback to ~60 Hz sleep
                callback();
            }
        }
    }

    auto stop() -> void
    {
        _stop.store(true, std::memory_order_release);
        if (_thread.joinable()) {
            _thread.join();
        }
    }
};

// MARK: - random name

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

// MARK: - modifiers

inline auto resolve_modifiers() -> Modifier_keys
{
    return {
        .primary = GetKeyState(VK_CONTROL) < 0,
        .alt = GetKeyState(VK_MENU) < 0,
        .shift = GetKeyState(VK_SHIFT) < 0,
    };
}

// MARK: - window callback

LRESULT CALLBACK window_callback(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    // Retrieve the graphics delegate stored in window's user data.
    auto* binder = reinterpret_cast<Platform_binder*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!binder) return DefWindowProcW(window, message, wparam, lparam);

    auto* delegate = binder->delegate;
    //const auto h = delegate ? delegate->get_size().h : 0;
    
    switch (message) {
        case WM_PAINT: {
            if (delegate) {
                const auto time_now = System_clock::now();

                auto ps = PAINTSTRUCT{};
                [[maybe_unused]] auto hdc = BeginPaint(window, &ps);

#if !WIN_GRAPHICS_GPU
                delegate->set_drawable(hdc);
#endif

                binder->interaction.modifier_keys = resolve_modifiers();
                binder->interaction.events = binder->events.consume(Steady_clock::now());
                delegate->draw(binder->interaction, time_now); // Delegate window context handles everything.
                binder->interaction.scroll_deltas = {};

#if !WIN_GRAPHICS_GPU
                ReleaseDC(window, hdc);
#endif

                EndPaint(window, &ps);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(window);
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};

            binder->events.push(Event{
                .event = Pointer_down{Pointer_button::left, pos}
            });
            binder->left_down = pos;

            return 0;
        }

        case WM_LBUTTONUP: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};

            binder->events.push(Event{
                .event = Pointer_up{Pointer_button::left, pos}
            });
            if (binder->left_down) {
                // Compute movement
                const auto start = *binder->left_down;
                const double dx = std::abs(pos.x - start.x);
                const double dy = std::abs(pos.y - start.y);

                // Windows system drag threshold
                const int cxDrag = GetSystemMetrics(SM_CXDRAG);
                const int cyDrag = GetSystemMetrics(SM_CYDRAG);

                const bool withinThreshold = dx < cxDrag && dy < cyDrag;

                if (withinThreshold) {
                    const auto count = binder->double_click ? 2u : 1u;
                    binder->events.push(Event{
                        .event = Pointer_click{Pointer_button::left, count, pos}
                    });
                }
                binder->left_down = std::nullopt;
                binder->double_click = false;
            }

            ReleaseCapture();
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};

            // This is second down click in a double click
            binder->events.push(Event{
                .event = Pointer_down{Pointer_button::left, pos}
            });
            binder->left_down = pos; // According to Windows we get DOWN UP DBLCLK UP
            binder->double_click = true;
            
            return 0;
        }

        case WM_MOUSEMOVE: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};

            if (!binder->mouse_in) {
                binder->events.push(Event{
                    .event = Pointer_enter{pos}
                });
                binder->mouse_in = true;
            }
            binder->events.push(Event{
                .event = Pointer_move{pos}
            });
            binder->last_pos = pos;

            return 0;
        }

        case WM_RBUTTONDOWN: {
            SetCapture(window);
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};

            binder->events.push(Event{
                .event = Pointer_down{Pointer_button::right, pos}
            });
            binder->right_down = pos;

            return 0;
        }

        case WM_RBUTTONUP: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};

            binder->events.push(Event{
                .event = Pointer_up{Pointer_button::right, pos}
            });
            if (binder->right_down) {
                // Compute movement
                const auto start = *binder->right_down;
                const double dx = std::abs(pos.x - start.x);
                const double dy = std::abs(pos.y - start.y);

                // Windows system drag threshold
                const int cxDrag = GetSystemMetrics(SM_CXDRAG);
                const int cyDrag = GetSystemMetrics(SM_CYDRAG);

                const bool withinThreshold = dx < cxDrag && dy < cyDrag;

                if (withinThreshold) {
                    binder->events.push(Event{
                        .event = Pointer_click{Pointer_button::right, 1, pos}
                    });
                }
                binder->right_down = std::nullopt;
            }

            ReleaseCapture();
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) * 20.f / WHEEL_DELTA;
            binder->interaction.scroll_deltas.y = delta;
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) * 20.f / WHEEL_DELTA;
            binder->interaction.scroll_deltas.x = delta;
            return 0;
        }

        case WM_MOUSELEAVE: {
            binder->interaction.modifier_keys = {};

            binder->events.push(Event{
                .event = Pointer_exit{binder->last_pos}
            });
            binder->mouse_in = false;

            return 0;
        }

        case WM_SETCURSOR: {
            // lParam tells you what region: low word = hit-test code
            const auto hit = LOWORD(lparam);

            if (hit == HTCLIENT) {
                // Set YOUR normal cursor
                SetCursor(LoadCursor(nullptr, IDC_ARROW));
                return TRUE;   // tell Windows you handled it
            }

            // For non-client areas (resize borders, title bar), let Windows do its default
            return DefWindowProcW(window, message, wparam, lparam);
        }

        case WM_TINY_SETCURSOR: {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return 0;
        }
        
        // Add other message handlers as needed
        
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

// MARK: - window registrar

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

// MARK: - platform view

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()> /*unused on windows*/) : _delegate{delegate}, _owns_view{owns_view}
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

    // Dark mode
    const auto dark = is_dark_mode();
    _binder.dark_mode = dark;
    enable_dark_title_bar(window, dark);
    _dark_watcher = std::make_unique<Dark_mode_watcher>([this](auto dark) { 
        _delegate->notify(Dark_mode_changed{dark});
    });
    _delegate->notify(Dark_mode_changed{dark});

    _binder.delegate = _delegate.get();
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&_binder));

    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = window});
    _delegate->assign_context(std::move(context));

    _view = window;
}

Platform_view::~Platform_view()
{
    auto* window = static_cast<HWND>(_view);
    SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    DestroyWindow(window);
}

auto Platform_view::on_create() -> void
{

}

auto Platform_view::on_show() -> void
{
    if (!_vsync_loop) {
        _vsync_loop = std::make_unique<Vsync_loop>([this]() { InvalidateRect(static_cast<HWND>(_view), nullptr, TRUE); });
    }
}

auto Platform_view::on_hide() -> void
{
    _vsync_loop = nullptr;
}

auto Platform_view::on_destroy() -> void
{

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

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    auto window = static_cast<HWND>(_view);
    SetWindowPos(window, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    _delegate->on_resize({w, h});
}

// MARK: - dialog support

// Exposes the editor window class name to win_dialogs.cpp so dialogs can
// locate the editor window and parent modal dialogs to it.
auto view_window_class_name() -> const std::wstring&
{
    return Window_registrar::instance().class_name;
}

} // namespace tiny
