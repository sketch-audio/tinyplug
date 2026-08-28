#include <tiny_platform/platform_dialogs.hpp>

#include "win_internal.hpp" // WM_TINY_SETCURSOR
#include "window_registry.hpp"

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <Windowsx.h>
#include <CommCtrl.h>
#include <commdlg.h> // File dialogs
#include <shellapi.h> // ShellExecute
#include <shlobj.h> // SHBrowseForFolderW (folder picker)

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace tiny {

// MARK: - Platform_dialogs

// Dialog element IDs
#define ID_TEXT 200
#define ID_EDIT 201

// Text input buffer.
static constexpr auto prompt_max_length = 256;
static wchar_t prompt_buffer[prompt_max_length] = L"";

static auto string_to_wstring(const std::string& str) -> std::wstring
{
    if (str.empty()) return {};
    const auto size_needed = MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), nullptr, 0);
    auto wstr = std::wstring(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.length()), &wstr[0], size_needed);
    return wstr;
}

static auto wstring_to_string(const std::wstring& wstr) -> std::string
{
    if (wstr.empty()) return {};
    const auto size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.length()), nullptr, 0, nullptr, nullptr);
    auto str = std::string(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.length()), &str[0], size_needed, nullptr, nullptr);
    return str;
}

// The window a dialog should be owned by: the view that asked (via its token),
// else the only view this binary has open, else none.
//
// nullptr is a usable answer — every dialog below still opens, just unowned —
// which is the point: the old class-name scan of the whole desktop returned the
// *first* matching window (wrong instance with two editors open), and every call
// site silently skipped the dialog *and* its callback when the scan missed.
inline auto owner_window(Window_token token) -> HWND
{
    auto* native = Window_registry::resolve(token);
    if (!native) native = Window_registry::sole();
    return static_cast<HWND>(native);
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
            
            // Center over our owner — the plug-in window we passed to
            // DialogBoxIndirectParamW, so no lookup is needed here.
            if (auto* owner = GetWindow(hdlg, GW_OWNER)) {
                auto rect = RECT{};
                auto dlg_rect = RECT{};
                GetWindowRect(owner, &rect);
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

// Measures wrapped text height when forced to fit within `wrap_w` pixels wide.
// Used by dialogs to size their static-text control when the message is too long
// for a single line.
inline auto measure_text_wrapped(const std::string& string, const Font_info& font, int wrap_w) -> int
{
    auto hdc = GetDC(nullptr);
    auto wname = string_to_wstring(font.name);
    auto hfont = CreateFontW(font.size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, wname.c_str());
    auto horiginal = reinterpret_cast<HFONT>(SelectObject(hdc, hfont));

    auto wstr = string_to_wstring(string);
    RECT rect = { 0, 0, wrap_w, 0 };
    DrawTextW(hdc, wstr.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    const auto height = static_cast<int>(rect.bottom - rect.top);

    SelectObject(hdc, horiginal);
    DeleteObject(hfont);
    ReleaseDC(nullptr, hdc);

    return height;
}

// Cap for dialog width before forcing word-wrap. Without this, a long message
// with no newlines produces a dialog as wide as the message — easily wider than
// the screen. macOS/iOS native alerts wrap automatically; this matches that.
constexpr auto k_max_dialog_w = 200;

// Thanks, GPT
inline auto align_dword(LPWORD lpIn) -> LPWORD
{
    auto ul = reinterpret_cast<uintptr_t>(lpIn);
    ul = (ul + 3) & ~static_cast<uintptr_t>(3); // align to 4 bytes
    return reinterpret_cast<LPWORD>(ul);
}

// MARK: - message 

auto Platform_dialogs::message(const std::string& title, const std::string& message, std::function<void()> on_done, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=, on_done=std::move(on_done)]() {
        auto* owner = owner_window(ctx.window);
        // calculate message size
        const auto font = Font_info{.name = "Segoe UI", .size = 9};
        auto [measured_w, text_h] = measure_text(message, font);

        const auto padding = 10;
        const auto button_h = 15;

        const auto unclamped_w = padding + measured_w + padding;
        const auto needs_wrap = unclamped_w > k_max_dialog_w;
        const auto dialog_w = needs_wrap ? k_max_dialog_w : std::max(120, unclamped_w);

        const auto text_w = dialog_w - 2 * padding; // So we center properly.
        const auto button_w = dialog_w - 2 * padding;

        if (needs_wrap) {
            text_h = measure_text_wrapped(message, font, text_w);
        }

        const auto dialog_h = padding + text_h + padding + button_h + padding;

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
                                    owner, 
                                    (DLGPROC)dialog_proc, 0); 
        GlobalFree(hgbl);

        ctx.tasks.on_main(on_done);

        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
    });
}

// MARK: - confirm

auto Platform_dialogs::confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_done, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=, on_done=std::move(on_done)]() {
        auto* owner = owner_window(ctx.window);
        
        // calculate message size
        const auto font = Font_info{.name = "Segoe UI", .size = 9};
        auto [measured_w, text_h] = measure_text(message, font);

        const auto padding = 10;
        const auto button_h = 15;

        const auto unclamped_w = padding + measured_w + padding;
        const auto needs_wrap = unclamped_w > k_max_dialog_w;
        const auto dialog_w = needs_wrap ? k_max_dialog_w : std::max(120, unclamped_w);

        const auto text_w = dialog_w - 2 * padding; // So we center properly.
        const auto button_w = (dialog_w - 3 * padding) / 2;

        if (needs_wrap) {
            text_h = measure_text_wrapped(message, font, text_w);
        }

        const auto dialog_h = padding + text_h + padding + button_h + padding;


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
                                    owner, 
                                    (DLGPROC)dialog_proc, 0); 
        GlobalFree(hgbl);

        ctx.tasks.on_main([on_done, ret]() {
            on_done(ret == IDOK);
        });

        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
    });
}

// MARK: - text_input

auto Platform_dialogs::text_input(const std::string& title, const std::string& message, std::function<void(std::string)> on_text, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=, on_text=std::move(on_text)]() {
        auto* owner = owner_window(ctx.window);
        // calculate message size
        const auto font = Font_info{.name = "Segoe UI", .size = 9};
        auto [measured_w, text_h] = measure_text(message, font);

        const auto padding = 10;
        const auto button_h = 15;
        const auto edit_h = font.size + 2;

        const auto unclamped_w = padding + measured_w + padding;
        const auto needs_wrap = unclamped_w > k_max_dialog_w;
        const auto dialog_w = needs_wrap ? k_max_dialog_w : std::max(160, unclamped_w);

        const auto text_w = dialog_w - 2 * padding; // So we center properly.
        const auto button_w = (dialog_w - 3 * padding) / 2;
        const auto edit_w = dialog_w - 2 * padding;

        if (needs_wrap) {
            text_h = measure_text_wrapped(message, font, text_w);
        }

        const auto dialog_h = padding + text_h + padding + button_h + padding + edit_h + padding;

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
                                    owner, 
                                    (DLGPROC)dialog_proc, 0); 
        GlobalFree(hgbl);

        // Cancel answers with nothing rather than dropping the callback.
        const auto text = (ret == IDOK) ? wstring_to_string(std::wstring{prompt_buffer}) : std::string{};
        ctx.tasks.on_main([on_text, text]() {
            on_text(text);
        });

        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
    });
}

auto Platform_dialogs::open_url(const std::string& url, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=]() {
        auto* owner = owner_window(ctx.window);
        const auto wurl = string_to_wstring(url);
        ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
    });
}

auto Platform_dialogs::save_file(const std::string& title, const std::string& default_path, const std::string& name, const std::string& extension, std::function<void(std::optional<std::string>)> on_save, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=, on_save = std::move(on_save)]() {
        auto* owner = owner_window(ctx.window);
        auto wtitle = string_to_wstring(title);
        auto wdefault_path = string_to_wstring(default_path);
        auto wname = string_to_wstring(name);
        auto wext = string_to_wstring(extension);

        // 1. Prepare the buffer with the initial filename
        auto file_buffer = std::array<wchar_t, 1024>{};
        std::fill_n(file_buffer.data(), file_buffer.size(), 0);
        if (!wname.empty()) {
            wcscpy_s(file_buffer.data(), file_buffer.size(), wname.c_str());
        }

        // 2. Construct the Filter (e.g., "Project Files (*.ext)\0*.ext\0All Files\0*.*\0\0")
        // Note: Must end with two null terminators.
        std::wstring filter = L"Supported Files (*." + wext + L")\0*." + wext + L"\0All Files (*.*)\0*.*\0";

        OPENFILENAMEW save_file_name = {};
        save_file_name.lStructSize = sizeof(OPENFILENAMEW);
        save_file_name.hwndOwner = owner;
        save_file_name.lpstrFile = file_buffer.data();
        save_file_name.nMaxFile = static_cast<DWORD>(file_buffer.size());
        save_file_name.lpstrFilter = filter.c_str();
        save_file_name.lpstrTitle = wtitle.c_str();
        save_file_name.lpstrInitialDir = wdefault_path.empty() ? nullptr : wdefault_path.c_str();
        
        // 3. Set the default extension (automatically appended if user doesn't type one)
        save_file_name.lpstrDefExt = wext.c_str();

        // 4. Flags: Added OFN_OVERWRITEPROMPT for safety
        save_file_name.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT | OFN_EXPLORER;

        const auto result = GetSaveFileNameW(&save_file_name);
        
        if (result) {
            const auto selected_path = wstring_to_string(std::wstring{save_file_name.lpstrFile});
            ctx.tasks.on_background([=, on_save = std::move(on_save)]() {
                on_save(selected_path);
            });
        }
        else {
            ctx.tasks.on_background([on_save = std::move(on_save)]() {
                on_save(std::nullopt);
            });
        }

        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); 
    });
}

// Shared by `open_file` -- a single-file GetOpenFileNameW pick, run on the
// caller's (background) thread.
static auto run_file_dialog(HWND owner, const std::wstring& wtitle, const std::wstring& wdefault_path) -> std::optional<std::string>
{
    auto open_file_name = OPENFILENAMEW{};
    auto file_buffer = std::array<wchar_t, 1024>{};
    std::fill_n(file_buffer.data(), file_buffer.size(), 0);

    open_file_name.lStructSize = sizeof(OPENFILENAMEW);
    open_file_name.hwndOwner = owner;
    open_file_name.lpstrFile = file_buffer.data();
    open_file_name.nMaxFile = static_cast<DWORD>(file_buffer.size());
    open_file_name.lpstrFilter = L"All Files\0*.*\0";
    open_file_name.lpstrTitle = wtitle.c_str();
    open_file_name.lpstrInitialDir = wdefault_path.empty() ? nullptr : wdefault_path.c_str();
    open_file_name.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&open_file_name)) return std::nullopt;
    return wstring_to_string(std::wstring{open_file_name.lpstrFile});
}

// Shared by `choose_dir` -- the classic SHBrowseForFolderW folder picker.
static auto run_dir_dialog(HWND owner, const std::wstring& wtitle) -> std::optional<std::string>
{
    auto path_buffer = std::array<wchar_t, MAX_PATH>{};
    std::fill_n(path_buffer.data(), path_buffer.size(), 0);

    auto browse_info = BROWSEINFOW{};
    browse_info.hwndOwner = owner;
    browse_info.pszDisplayName = path_buffer.data();
    browse_info.lpszTitle = wtitle.c_str();
    browse_info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&browse_info);
    if (!pidl) return std::nullopt;

    auto folder_buffer = std::array<wchar_t, MAX_PATH>{};
    auto selected_path = std::optional<std::string>{};
    if (SHGetPathFromIDListW(pidl, folder_buffer.data())) {
        selected_path = wstring_to_string(std::wstring{folder_buffer.data()});
    }
    CoTaskMemFree(pidl);
    return selected_path;
}

auto Platform_dialogs::open_file(const std::string& title, const std::string& default_path, std::function<void(std::optional<std::string>)> on_open, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=, on_open=std::move(on_open)]() {
        auto* owner = owner_window(ctx.window);
        const auto selected_path = run_file_dialog(owner, string_to_wstring(title), string_to_wstring(default_path));
        ctx.tasks.on_background([=, on_open=std::move(on_open)]() {
            on_open(selected_path);
        });

        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
    });
}

auto Platform_dialogs::choose_dir(const std::string& title, const std::string& /*default_path*/, std::function<void(std::optional<std::string>)> on_choose, Dialog_context ctx) -> void
{
    ctx.tasks.on_background([=, on_choose=std::move(on_choose)]() {
        auto* owner = owner_window(ctx.window);
        const auto selected_path = run_dir_dialog(owner, string_to_wstring(title));
        ctx.tasks.on_background([=, on_choose=std::move(on_choose)]() {
            on_choose(selected_path);
        });

        if (owner) SendMessageW(owner, WM_TINY_SETCURSOR, 0, 0); // Reset cursor.
    });
}

} // namespace tiny
