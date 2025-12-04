#include "../platform_view.h"
#include "../platform_dialogs.h"

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

#include "../window_context.h"

#define WM_TINY_SETCURSOR (WM_APP + 1) // Reset cursor message for dialogs.

namespace tiny {

// MARK: - dark mode

inline auto is_dark_mode() -> bool
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

inline auto enable_dark_title_bar(HWND hwnd, bool dark) -> void
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
                BeginPaint(window, &ps);
                binder->interaction.modifier_keys = resolve_modifiers();
                binder->interaction.events = binder->events.consume(Steady_clock::now());
                delegate->draw(binder->interaction, time_now); // Delegate window context handles everything.
                binder->interaction.scroll_deltas = {};
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

// MARK: - Platform_dialogs

// Dialog element IDs
#define ID_TEXT 200
#define ID_EDIT 201

// Text input buffer.
static constexpr auto prompt_max_length = 256;
static wchar_t prompt_buffer[prompt_max_length] = L"";

inline auto string_to_wstring(const std::string& str) -> std::wstring
{
    if (str.empty()) return {};
    const auto size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    auto wstr = std::wstring(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], size_needed);
    return wstr;
}

inline auto wstring_to_string(const std::wstring& wstr) -> std::string
{
    if (wstr.empty()) return {};
    const auto size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    auto str = std::string(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size_needed, nullptr, nullptr);
    return str;
}

struct Window_info {
    HWND hwnd;
    RECT rect;
    std::wstring class_name;
};

BOOL CALLBACK find_window_proc(HWND hwnd, LPARAM lparam) 
{
    const auto& registrar = Window_registrar::instance();

    static constexpr auto max_count = 256;
    auto class_name = std::array<wchar_t, max_count>{};
    auto window_name = std::array<wchar_t, max_count>{};
    auto rect = RECT{0};

    GetClassNameW(hwnd, class_name.data(), max_count);
    GetWindowRect(hwnd, &rect);

    if (registrar.class_name == std::wstring{class_name.data()}) {
        auto* info = reinterpret_cast<std::optional<Window_info>*>(lparam);
        *info = Window_info{
            .hwnd = hwnd,
            .rect = rect,
            .class_name = class_name.data(),
        };
        return FALSE;
    }

    return TRUE;
}

inline auto find_plugin_window() -> std::optional<Window_info>
{   
    auto info = std::optional<Window_info>{};
    EnumChildWindows(GetDesktopWindow(), find_window_proc, reinterpret_cast<LPARAM>(&info));
    return info;
}

// Handle owner draw buttons.
inline auto draw_button(DRAWITEMSTRUCT* draw_item) -> void
{
    auto create_brush_for = [](bool dark_mode, int state) {
        if (state & ODS_SELECTED) {
            return dark_mode ? CreateSolidBrush(RGB(150, 150, 175)) : CreateSolidBrush(RGB(120, 120, 140));
        }
        else {
            return dark_mode ? CreateSolidBrush(RGB(80, 80, 93)) : CreateSolidBrush(RGB(180, 180, 210));
        }
    };

    // Custom draw the button
    const auto is_dark = is_dark_mode();
    auto button_bg = create_brush_for(is_dark, draw_item->itemState);

    auto hdc = draw_item->hDC;
    auto rc = draw_item->rcItem;
    FillRect(hdc, &rc, button_bg);

    SetTextColor(hdc, is_dark ? RGB(230, 230, 230) : RGB(0, 0, 0));
    SetBkMode(hdc, TRANSPARENT);

    auto button_text = std::array<wchar_t, 256>{};
    GetWindowTextW(draw_item->hwndItem, button_text.data(), static_cast<int>(button_text.size()));
    DrawTextW(hdc, button_text.data(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (draw_item->itemState & ODS_FOCUS) {
        DrawFocusRect(hdc, &rc);
    }

    DeleteBrush(button_bg);
}

// MARK: - dialog_proc 

static INT_PTR CALLBACK dialog_proc(HWND hdlg, UINT message, WPARAM wparam, LPARAM lparam) 
{
	switch (message) {
		case WM_INITDIALOG: {
			SendDlgItemMessageW(hdlg, ID_EDIT, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(prompt_buffer));
            
            // Center the dialog over the plugin window
            if (const auto plugin_window = find_plugin_window()) {
                auto rect = plugin_window->rect;
                auto dlg_rect = RECT{};
                GetWindowRect(hdlg, &dlg_rect);
                const auto x = rect.left + (rect.right - rect.left - (dlg_rect.right - dlg_rect.left)) / 2;
                const auto y = rect.top + (rect.bottom - rect.top - (dlg_rect.bottom - dlg_rect.top)) / 2;
                SetWindowPos(hdlg, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }

            // Set dark mode.
            enable_dark_title_bar(hdlg, is_dark_mode());

			return TRUE;
		}

		case WM_DESTROY: {
			EndDialog(hdlg, 0);
			return TRUE;
		}

		case WM_COMMAND: {
			switch (wparam) {
				case IDOK: {
					SendDlgItemMessageW(hdlg, ID_EDIT, WM_GETTEXT, prompt_max_length, reinterpret_cast<LPARAM>(prompt_buffer));
					EndDialog(hdlg, IDOK);
					return TRUE;
				}

				case IDCANCEL: {
					EndDialog(hdlg, IDCANCEL);
					return TRUE;
				}

                default: return FALSE;
			}
		}

        case WM_DRAWITEM: {
            auto* draw_item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            draw_button(draw_item);
            break;
        }

        // set background color
        case WM_CTLCOLORDLG: 
        case WM_CTLCOLORSTATIC: {
            if (is_dark_mode()) {
                auto hdc = reinterpret_cast<HDC>(wparam);
                SetTextColor(hdc, RGB(230, 230, 230));
                SetBkColor(hdc, RGB(36, 36, 42));
                static auto hbrush = CreateSolidBrush(RGB(36, 36, 42));
                return reinterpret_cast<INT_PTR>(hbrush);
            }
            break;
        }

        case WM_CTLCOLOREDIT: {
            if (is_dark_mode()) {
                auto hdc = reinterpret_cast<HDC>(wparam);
                SetTextColor(hdc, RGB(230, 230, 230));
                SetBkColor(hdc, RGB(48, 48, 56));
                static auto hbrush = CreateSolidBrush(RGB(48, 48, 56));
                return reinterpret_cast<INT_PTR>(hbrush);
            }
            break;
        }

        // Respond to light/dark mode changes.
        case WM_SETTINGCHANGE: {
            if (lparam && lstrcmpiW(reinterpret_cast<LPCWSTR>(lparam), L"ImmersiveColorSet") == 0) {
                const auto dark = is_dark_mode();
                enable_dark_title_bar(hdlg, dark);
                InvalidateRect(hdlg, nullptr, TRUE);
            }
            break;
        }
    }

	return FALSE;
}

// MARK: - measure text

// Splits a string into vector of strings splitting by newline charachter.
inline auto split_newline(const std::string& string) -> std::vector<std::string>
{
    auto result = std::vector<std::string>{};
    auto stream = std::istringstream{string};
    auto line = std::string{};
    while (std::getline(stream, line)) {
        result.push_back(line);
    }
    return result;
}

// Returns the longest line and the number of lines in a string containing 0 or more newline characters.
inline auto string_extent(const std::string& string) -> std::pair<std::string, size_t>
{
    auto lines = split_newline(string);
    auto longest = std::string{};
    for (const auto& line : lines) {
        if (line.size() > longest.size()) {
            longest = line;
        }
    }
    return {longest, lines.size()};
}

struct Font_info {
    std::string name;
    int size;
};

inline auto measure_text(const std::string& string, const Font_info& font) -> std::pair<int, int>
{
    const auto [longest_line, line_count] = string_extent(string);

    auto hdc = GetDC(nullptr);
    auto wname = string_to_wstring(font.name);
    auto hfont = CreateFontW(font.size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, wname.c_str());
    auto horiginal = reinterpret_cast<HFONT>(SelectObject(hdc, hfont));

    auto wline = string_to_wstring(longest_line);
    auto size = SIZE{};
    GetTextExtentPoint32W(hdc, wline.c_str(), static_cast<int>(wline.size()), &size);

    SelectObject(hdc, horiginal);
    DeleteObject(hfont);
    ReleaseDC(nullptr, hdc);

    return {size.cx, static_cast<int>(line_count) * size.cy};
}

// Thanks, GPT
inline auto align_dword(LPWORD lpIn) -> LPWORD
{
    auto ul = reinterpret_cast<uintptr_t>(lpIn);
    ul = (ul + 3) & ~static_cast<uintptr_t>(3); // align to 4 bytes
    return reinterpret_cast<LPWORD>(ul);
}

// MARK: - message 

auto Platform_dialogs::message(const std::string& title, const std::string& message, Later<> on_done) -> void
{
    auto thread = std::thread([title, message, on_done]() {
        if (const auto plugin_window = find_plugin_window()) {
            // calculate message size
            const auto font = Font_info{.name = "Segoe UI", .size = 9};
            auto [measured_w, text_h] = measure_text(message, font);

            const auto padding = 10;
            const auto button_h = 15;

            const auto dialog_w = std::max(120, padding + measured_w + padding);
            const auto dialog_h = padding + text_h + padding + button_h + padding;

            const auto text_w = dialog_w - 2 * padding; // So we center properly.
            const auto button_w = dialog_w - 2 * padding;

            // See: https://learn.microsoft.com/en-us/windows/win32/dlgbox/using-dialog-boxes
            HINSTANCE hInstance = GetModuleHandle(nullptr);
            HGLOBAL hgbl;
            LPDLGTEMPLATE lpdt;
            LPDLGITEMTEMPLATE lpdit;
            LPWORD lpw;
            LPWSTR lpwsz;
            LRESULT ret;
            int nchar;

            hgbl = GlobalAlloc(GMEM_ZEROINIT, 4096);
            if (!hgbl) return;
        
            lpdt = (LPDLGTEMPLATE)GlobalLock(hgbl);
        
            // Define a dialog box.
        
            lpdt->style = WS_POPUP | WS_BORDER | WS_SYSMENU | DS_MODALFRAME | WS_CAPTION | DS_SETFONT;
            lpdt->dwExtendedStyle = WS_EX_NOPARENTNOTIFY;
            lpdt->cdit = 2;         // Number of controls
            lpdt->x  = 0;  lpdt->y  = 0;
            lpdt->cx = static_cast<short>(dialog_w); lpdt->cy = static_cast<short>(dialog_h);

            lpw = (LPWORD)(lpdt + 1);
            *lpw++ = 0;             // No menu
            *lpw++ = 0;             // Predefined dialog box class (by default)

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, lpwsz, static_cast<int>(title.size()) + 1);
            lpw += nchar;
            

            *lpw++ = static_cast<WORD>(font.size);             // Font size
            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, font.name.c_str(), -1, lpwsz, static_cast<int>(font.name.size()) + 1);
            lpw += nchar;

            //-----------------------
            // Define an OK button.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = static_cast<short>(padding); lpdit->y  = static_cast<short>(padding + text_h + padding);
            lpdit->cx = static_cast<short>(button_w); lpdit->cy = static_cast<short>(button_h);
            lpdit->id = IDOK;       // OK button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, "OK", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a static text control.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = static_cast<short>(padding); lpdit->y  = static_cast<short>(padding);
            lpdit->cx = static_cast<short>(text_w); lpdit->cy = static_cast<short>(text_h);
            lpdit->id = ID_TEXT;    // Text identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | SS_CENTER;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0082;        // Static class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, lpwsz, static_cast<int>(message.size()) + 1);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            GlobalUnlock(hgbl); 
            ret = DialogBoxIndirectParamW(hInstance, 
                                        (LPDLGTEMPLATE)hgbl, 
                                        plugin_window->hwnd, 
                                        (DLGPROC)dialog_proc, 0); 
            GlobalFree(hgbl);

            on_done();

            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }        
    });
    thread.detach();
}

// MARK: - confirm

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, Later<bool> on_done) -> void
{
    auto thread = std::thread([title, message, on_done]() {
        if (const auto plugin_window = find_plugin_window()) {
            
            // calculate message size
            const auto font = Font_info{.name = "Segoe UI", .size = 9};
            auto [measured_w, text_h] = measure_text(message, font);

            const auto padding = 10;
            const auto button_h = 15;

            const auto dialog_w = std::max(120, padding + measured_w + padding);
            const auto dialog_h = padding + text_h + padding + button_h + padding;

            const auto text_w = dialog_w - 2 * padding; // So we center properly.
            const auto button_w = (dialog_w - 3 * padding) / 2;


            // See: https://learn.microsoft.com/en-us/windows/win32/dlgbox/using-dialog-boxes
            HINSTANCE hInstance = GetModuleHandle(nullptr);
            HGLOBAL hgbl;
            LPDLGTEMPLATE lpdt;
            LPDLGITEMTEMPLATE lpdit;
            LPWORD lpw;
            LPWSTR lpwsz;
            LRESULT ret;
            int nchar;

            hgbl = GlobalAlloc(GMEM_ZEROINIT, 1024);
            if (!hgbl) return;
        
            lpdt = (LPDLGTEMPLATE)GlobalLock(hgbl);
        
            // Define a dialog box.
        
            lpdt->style = WS_POPUP | WS_BORDER | WS_SYSMENU | DS_MODALFRAME | WS_CAPTION | DS_SETFONT;
            lpdt->dwExtendedStyle = WS_EX_NOPARENTNOTIFY;
            lpdt->cdit = 3;         // Number of controls
            lpdt->x  = 0;  lpdt->y  = 0;
            lpdt->cx = static_cast<short>(dialog_w); lpdt->cy = static_cast<short>(dialog_h);

            lpw = (LPWORD)(lpdt + 1);
            *lpw++ = 0;             // No menu
            *lpw++ = 0;             // Predefined dialog box class (by default)

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, lpwsz, static_cast<int>(title.size()) + 1);
            lpw += nchar;
            

            *lpw++ = static_cast<WORD>(font.size);             // Font size
            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, font.name.c_str(), -1, lpwsz, static_cast<int>(font.name.size()) + 1);
            lpw += nchar;

            //-----------------------
            // Define an OK button.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = static_cast<short>(padding); lpdit->y  = static_cast<short>(padding + text_h + padding);
            lpdit->cx = static_cast<short>(button_w); lpdit->cy = static_cast<short>(button_h);
            lpdit->id = IDOK;       // OK button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, "OK", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a cancel button.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = static_cast<short>(padding + button_w + padding); lpdit->y  = static_cast<short>(padding + text_h + padding);
            lpdit->cx = static_cast<short>(button_w); lpdit->cy = static_cast<short>(button_h);
            lpdit->id = IDCANCEL;       // Cancel button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, "Cancel", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a static text control.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = static_cast<short>(padding); lpdit->y  = static_cast<short>(padding);
            lpdit->cx = static_cast<short>(text_w); lpdit->cy = static_cast<short>(text_h);
            lpdit->id = ID_TEXT;    // Text identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | SS_CENTER;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0082;        // Static class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, lpwsz, static_cast<int>(message.size()) + 1);
            lpw += nchar;
            *lpw++ = 0;             // No creation data


            GlobalUnlock(hgbl); 
            ret = DialogBoxIndirectParamW(hInstance, 
                                        (LPDLGTEMPLATE)hgbl, 
                                        plugin_window->hwnd, 
                                        (DLGPROC)dialog_proc, 0); 
            GlobalFree(hgbl);

            on_done(ret == IDOK);

            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }
    });
    thread.detach();
}

// MARK: - text_input

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, Later<std::string> on_text) -> void
{
    auto thread = std::thread([title, message, on_text]() {
        if (const auto plugin_window = find_plugin_window()) {
            // calculate message size
            const auto font = Font_info{.name = "Segoe UI", .size = 9};
            auto [measured_w, text_h] = measure_text(message, font);

            const auto padding = 10;
            const auto button_h = 15;
            const auto edit_h = font.size + 2;

            const auto dialog_w = std::max(160, padding + measured_w + padding);
            const auto dialog_h = padding + text_h + padding + button_h + padding + edit_h + padding;

            const auto text_w = dialog_w - 2 * padding; // So we center properly.
            const auto button_w = (dialog_w - 3 * padding) / 2;
            const auto edit_w = dialog_w - 2 * padding;

            // See: https://learn.microsoft.com/en-us/windows/win32/dlgbox/using-dialog-boxes
            HINSTANCE hInstance = GetModuleHandle(nullptr);
            HGLOBAL hgbl;
            LPDLGTEMPLATE lpdt;
            LPDLGITEMTEMPLATE lpdit;
            LPWORD lpw;
            LPWSTR lpwsz;
            LRESULT ret;
            int nchar;

            hgbl = GlobalAlloc(GMEM_ZEROINIT, 1024);
            if (!hgbl) return;
        
            lpdt = (LPDLGTEMPLATE)GlobalLock(hgbl);
        
            // Define a dialog box.
        
            lpdt->style = WS_POPUP | WS_BORDER | WS_SYSMENU | DS_MODALFRAME | WS_CAPTION | DS_SETFONT;
            lpdt->dwExtendedStyle = WS_EX_NOPARENTNOTIFY;
            lpdt->cdit = 4;         // Number of controls
            lpdt->x  = 0;  lpdt->y  = 0;
            lpdt->cx = static_cast<short>(dialog_w); lpdt->cy = static_cast<short>(dialog_h);

            lpw = (LPWORD)(lpdt + 1);
            *lpw++ = 0;             // No menu
            *lpw++ = 0;             // Predefined dialog box class (by default)

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, lpwsz, static_cast<int>(title.size()) + 1);
            lpw += nchar;

            *lpw++ = static_cast<WORD>(font.size);             // Font size
            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, font.name.c_str(), -1, lpwsz, static_cast<int>(font.name.size()) + 1);
            lpw += nchar;

            //-----------------------
            // Define an OK button.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x = static_cast<short>(padding); 
            lpdit->y = static_cast<short>(padding + text_h + padding + edit_h + padding);
            lpdit->cx = static_cast<short>(button_w); 
            lpdit->cy = static_cast<short>(button_h)    ;
            lpdit->id = IDOK;       // OK button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, "OK", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a cancel button.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x = static_cast<short>(padding + button_w + padding); 
            lpdit->y = static_cast<short>(padding + text_h + padding + edit_h + padding);
            lpdit->cx = static_cast<short>(button_w); 
            lpdit->cy = static_cast<short>(button_h);
            lpdit->id = IDCANCEL;       // Cancel button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, "Cancel", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define an edit control.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x = static_cast<short>(padding); 
            lpdit->y = static_cast<short>(padding + text_h + padding);
            lpdit->cx = static_cast<short>(edit_w);
            lpdit->cy = static_cast<short>(edit_h);
            lpdit->id = ID_EDIT;    // Help button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0081;        // Button class atom

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, "", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a static text control.
            //-----------------------
            lpw = align_dword(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x = static_cast<short>(padding);
            lpdit->y = static_cast<short>(padding);
            lpdit->cx = static_cast<short>(text_w);
            lpdit->cy = static_cast<short>(text_h);
            lpdit->id = ID_TEXT;    // Text identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | SS_CENTER;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0082;        // Static class

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, lpwsz, static_cast<int>(message.size()) + 1);
            lpw += nchar;
            *lpw++ = 0;             // No creation data


            // 
            std::fill_n(prompt_buffer, prompt_max_length, 0);

            GlobalUnlock(hgbl); 
            ret = DialogBoxIndirectParamW(hInstance, 
                                        (LPDLGTEMPLATE)hgbl, 
                                        plugin_window->hwnd, 
                                        (DLGPROC)dialog_proc, 0); 
            GlobalFree(hgbl);

            if (ret == IDOK) {
                const auto text = wstring_to_string(prompt_buffer);
                on_text(text);
            }

            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }        
    });
    thread.detach();
}

auto Platform_dialogs::open_url(const std::string& url) -> void
{
    auto thread = std::thread([url]() {
        if (const auto plugin_window = find_plugin_window()) {
            const auto wurl = string_to_wstring(url);
            ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }
    });
    thread.detach();
}

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, Later<std::optional<std::string>> on_open) -> void
{
    auto thread = std::thread([title, default_path, on_open]() {
        if (const auto plugin_window = find_plugin_window()) {
            auto wtitle = string_to_wstring(title);
            auto wdefault_path = string_to_wstring(default_path);

            auto open_file_name = OPENFILENAMEW{};
            auto file_buffer = std::array<wchar_t, 1024>{};
            std::fill_n(file_buffer.data(), file_buffer.size(), 0);

            open_file_name.lStructSize = sizeof(OPENFILENAMEW);
            open_file_name.hwndOwner = plugin_window->hwnd;
            open_file_name.lpstrFile = file_buffer.data();
            open_file_name.nMaxFile = static_cast<DWORD>(file_buffer.size());
            open_file_name.lpstrFilter = L"All Files\0*.*\0";
            open_file_name.lpstrTitle = wtitle.c_str();
            open_file_name.lpstrInitialDir = wdefault_path.empty() ? nullptr : wdefault_path.c_str();
            open_file_name.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

            const auto result = GetOpenFileNameW(&open_file_name);
            if (result) {
                const auto selected_path = wstring_to_string(std::wstring{open_file_name.lpstrFile});
                on_open(selected_path);
            }
            else {
                on_open(std::nullopt);
            }

            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }
    });
    thread.detach();
}

} // namespace tiny