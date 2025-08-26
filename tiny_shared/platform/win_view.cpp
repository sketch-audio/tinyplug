#include "platform.h"
#include "platform_view.h"

#if PLATFORM_WINDOWS

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
#include <dwmapi.h>
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define SK_DIRECT3D
#include "../skia/win/WindowContextFactory_win.h"

#define CALLBACK_TIMER_ID 1001

namespace tiny {

inline auto is_dark_mode() -> bool
{
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD,
                     nullptr,
                     &value,
                     &size) == ERROR_SUCCESS) {
        return value == 0; // 0 = dark
    }
    return false;
}

void enable_dark_title_bar(HWND hwnd, bool dark) {
    BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd,
        DWMWA_USE_IMMERSIVE_DARK_MODE, // value = 20 on 1809, 19 on 1903+
        &useDark,
        sizeof(useDark));
}

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

#define WM_TINY_SETCURSOR (WM_APP + 1)

inline auto resolve_modifiers() -> Modifier_keys
{
    return {
        .primary = GetKeyState(VK_CONTROL) < 0,
        .alt = GetKeyState(VK_MENU) < 0,
        .shift = GetKeyState(VK_SHIFT) < 0,
    };
}

// C-style window callback
LRESULT CALLBACK window_callback(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    // Retrieve the graphics delegate stored in window's user data.
    auto* binder = reinterpret_cast<Platform_binder*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!binder) return DefWindowProcW(window, message, wparam, lparam);

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
                binder->interaction.modifier_keys = resolve_modifiers();
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
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            binder->left_pos = Coords{x, y};
            return 0;
        }

        case WM_LBUTTONUP: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
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
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            const auto pos = Coords{x, y};
            try_set(binder->interaction.state, tiny::Double_click{pos});
            binder->over_pos = std::nullopt;
            return 0;
        }

        case WM_MOUSEMOVE: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
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
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
            binder->right_pos = Coords{x, y};
            return 0;
        }

        case WM_RBUTTONUP: {
            const auto x = static_cast<double>(GET_X_LPARAM(lparam));
            const auto y = static_cast<double>(GET_Y_LPARAM(lparam));
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
            const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) * 1.f / WHEEL_DELTA;
            binder->interaction.scroll_deltas.y = delta;
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            const auto delta = GET_WHEEL_DELTA_WPARAM(wparam) * 1.f / WHEEL_DELTA;
            binder->interaction.scroll_deltas.x = delta;
            return 0;
        }

        case WM_MOUSELEAVE: {
            binder->interaction.state = Consumed{};
            binder->over_pos = std::nullopt;
            binder->left_pos = std::nullopt;
            binder->interaction.modifier_keys = {};
            return 0;
        }

        case WM_TINY_SETCURSOR: {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        
        // Add other message handlers as needed
        
        default:
            return DefWindowProcW(window, message, wparam, lparam);
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
    GetWindowTextW(draw_item->hwndItem, button_text.data(), button_text.size());
    DrawTextW(hdc, button_text.data(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if (draw_item->itemState & ODS_FOCUS) {
        DrawFocusRect(hdc, &rc);
    }

    DeleteBrush(button_bg);
}

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
					EndDialog(hdlg, 1);
					return TRUE;
				}

				case IDCANCEL: {
					EndDialog(hdlg, 0);
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


LPWORD lpwAlign(LPWORD lpIn)
{
    uintptr_t ul = reinterpret_cast<uintptr_t>(lpIn);
    ul = (ul + 3) & ~static_cast<uintptr_t>(3); // align to 4 bytes
    return reinterpret_cast<LPWORD>(ul);
}

// MARK: - alert

auto Platform_dialogs::alert(const std::string& title, const std::string& message) -> void
{
    auto thread = std::thread([title, message](){
        HINSTANCE hInstance = GetModuleHandle(nullptr);

        if (const auto plugin_window = find_plugin_window()) {
            // See: https://learn.microsoft.com/en-us/windows/win32/dlgbox/using-dialog-boxes
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
            lpdt->cx = 120; lpdt->cy = 60;

            lpw = (LPWORD)(lpdt + 1);
            *lpw++ = 0;             // No menu
            *lpw++ = 0;             // Predefined dialog box class (by default)

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_ACP, 0, title.c_str(), -1, lpwsz, 50);
            lpw += nchar;
            //if (nchar % 2 == 0) *lpw++ = 0;
            

            *lpw++ = 9;             // Font size
            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_ACP, 0, "Segoe UI", -1, lpwsz, 50);
            lpw += nchar;
            //if (nchar % 2 == 0) *lpw++ = 0;

            //-----------------------
            // Define an OK button.
            //-----------------------
            lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = 10; lpdit->y  = 35;
            lpdit->cx = 100; lpdit->cy = 15;
            lpdit->id = IDOK;       // OK button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = 1 + MultiByteToWideChar(CP_ACP, 0, "OK", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a static text control.
            //-----------------------
            lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = 10; lpdit->y  = 10;
            lpdit->cx = 100; lpdit->cy = 20;
            lpdit->id = ID_TEXT;    // Text identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | SS_CENTER;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0082;        // Static class

            lpwsz = (LPWSTR)lpw;
            nchar = 1 + MultiByteToWideChar(CP_ACP, 0, message.c_str(), -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            GlobalUnlock(hgbl); 
            ret = DialogBoxIndirectParamW(hInstance, 
                                        (LPDLGTEMPLATE)hgbl, 
                                        plugin_window->hwnd, 
                                        (DLGPROC)dialog_proc, 0); 
            GlobalFree(hgbl);

            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }        
    });
    thread.detach();
}

// MARK: - text_input

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, Callback callback) -> void
{
    auto thread = std::thread([title, message, callback](){
        HINSTANCE hInstance = GetModuleHandle(nullptr);

        if (const auto plugin_window = find_plugin_window()) {
            // See: https://learn.microsoft.com/en-us/windows/win32/dlgbox/using-dialog-boxes
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
            lpdt->cx = 160; lpdt->cy = 76;

            lpw = (LPWORD)(lpdt + 1);
            *lpw++ = 0;             // No menu
            *lpw++ = 0;             // Predefined dialog box class (by default)

            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_ACP, 0, title.c_str(), -1, lpwsz, 50);
            lpw += nchar;
            //if (nchar % 2 == 0) *lpw++ = 0;
            

            *lpw++ = 9;             // Font size
            lpwsz = (LPWSTR)lpw;
            nchar = MultiByteToWideChar(CP_ACP, 0, "Segoe UI", -1, lpwsz, 50);
            lpw += nchar;
            //if (nchar % 2 == 0) *lpw++ = 0;

            //-----------------------
            // Define an OK button.
            //-----------------------
            lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = 10; lpdit->y  = 51;
            lpdit->cx = 65; lpdit->cy = 15;
            lpdit->id = IDOK;       // OK button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = 1 + MultiByteToWideChar(CP_ACP, 0, "OK", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a cancel button.
            //-----------------------
            lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = 85; lpdit->y  = 51;
            lpdit->cx = 65; lpdit->cy = 15;
            lpdit->id = IDCANCEL;       // Cancel button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0080;        // Button class

            lpwsz = (LPWSTR)lpw;
            nchar = 1 + MultiByteToWideChar(CP_ACP, 0, "Cancel", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define an edit control.
            //-----------------------
            lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = 10; lpdit->y  = 30;
            lpdit->cx = 140; lpdit->cy = 11;
            lpdit->id = ID_EDIT;    // Help button identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0081;        // Button class atom

            lpwsz = (LPWSTR)lpw;
            nchar = 1 + MultiByteToWideChar(CP_ACP, 0, "", -1, lpwsz, 50);
            lpw += nchar;
            *lpw++ = 0;             // No creation data

            //-----------------------
            // Define a static text control.
            //-----------------------
            lpw = lpwAlign(lpw);    // Align DLGITEMTEMPLATE on DWORD boundary
            lpdit = (LPDLGITEMTEMPLATE)lpw;
            lpdit->x  = 10; lpdit->y  = 10;
            lpdit->cx = 140; lpdit->cy = 20;
            lpdit->id = ID_TEXT;    // Text identifier
            lpdit->style = WS_CHILD | WS_VISIBLE | SS_LEFT;

            lpw = (LPWORD)(lpdit + 1);
            *lpw++ = 0xFFFF;
            *lpw++ = 0x0082;        // Static class

            lpwsz = (LPWSTR)lpw;
            nchar = 1 + MultiByteToWideChar(CP_ACP, 0, message.c_str(), -1, lpwsz, 50);
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

            if (ret) {
                const auto text = wstring_to_string(prompt_buffer);
                callback(true, text);
            }

            SendMessageW(plugin_window->hwnd, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
        }        
    });
    thread.detach();
}

} // namespace tiny

#endif