#include <tiny_platform/platform_view.hpp>

#include "window_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
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

#include <tiny_platform/platform_text.hpp>
#include <tiny_platform/window_context.hpp>
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

// MARK: - scroll

// Windows exposes no "is this a precision device" flag, the way AppKit's
// `hasPreciseScrollingDeltas` does. The one usable signal is the delta itself: a
// wheel only ever reports whole notches, while a precision touchpad reports
// sub-notch deltas continuously as the finger moves.
//
// So sub-notch deltas are treated as travel — converted to points and marked
// precise, the same unit the macOS path delivers — while whole notches keep the
// detent scale the scroll views were already tuned against.
//
// `touch_points_per_notch` is the one number here that cannot be derived: Windows
// gives no physical scale for a touchpad delta. Tune it by feel.
struct Wheel_delta {
    float units{};
    bool precise{};
};

auto resolve_wheel_delta(SHORT raw) -> Wheel_delta
{
    constexpr auto notch_units = 20.f;             // a notch's worth of list scrolling
    constexpr auto touch_points_per_notch = 40.f;  // finger travel one notch spans

    const auto precise = (raw > -WHEEL_DELTA && raw < WHEEL_DELTA);
    const auto scale = precise ? touch_points_per_notch : notch_units;
    return {static_cast<float>(raw) * scale / static_cast<float>(WHEEL_DELTA), precise};
}

// MARK: - text fields

// A field is an EDIT inside a container child of the editor window. The container paints the background (an EDIT
// draws its single line at the top, so it's centred inside), colours the EDIT (WM_CTLCOLOREDIT goes to the parent),
// and closes the pair once the field has ended.
#define WM_TINY_FIELD_CLOSE (WM_APP + 3)

namespace {

struct Win_field {
    HWND editor{};
    HWND container{};
    HWND edit{};
    HFONT font{};
    HBRUSH brush{};
    COLORREF ink{};
    COLORREF background{};
    HWND previous{}; // Who had the keyboard before us, given it back after.
    Text_field::Blur blur{};
    std::function<void(Text_result)> done{};
    bool finished{};
};

std::unordered_map<HWND, Win_field*> open_fields{}; // By editor window; UI thread only.

auto widen(const std::string& s) -> std::wstring
{
    if (s.empty()) return {};
    const auto n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    auto w = std::wstring(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

auto narrow(const std::wstring& w) -> std::string
{
    if (w.empty()) return {};
    const auto n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    auto s = std::string(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

auto colorref(std::uint32_t argb) -> COLORREF
{
    return RGB((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff);
}

// One of the font's names (1: legacy family, 16: typographic family), from its Windows-platform strings, US English
// first: the names GDI knows a font by.
auto font_name(std::span<const std::byte> data, std::uint32_t wanted) -> std::wstring
{
    const auto u8 = [&](size_t at) -> std::uint32_t { return at < data.size() ? std::to_integer<std::uint32_t>(data[at]) : 0u; };
    const auto u16 = [&](size_t at) -> std::uint32_t { return (u8(at) << 8) | u8(at + 1); };
    const auto u32 = [&](size_t at) -> std::uint32_t { return (u16(at) << 16) | u16(at + 2); };
    const auto tables = u16(4);
    for (auto i = std::uint32_t{}; i < tables; ++i) {
        const auto record = 12 + 16 * size_t{i};
        if (u32(record) != 0x6E616D65) continue; // 'name'
        const auto name = size_t{u32(record + 8)};
        const auto count = u16(name + 2);
        const auto strings = name + u16(name + 4);
        auto best = std::wstring{};
        auto best_rank = 0;
        for (auto j = std::uint32_t{}; j < count; ++j) {
            const auto r = name + 6 + 12 * size_t{j};
            const auto platform = u16(r), encoding = u16(r + 2), language = u16(r + 4), id = u16(r + 6), length = u16(r + 8), offset = u16(r + 10);
            if (platform != 3 || (encoding != 1 && encoding != 10) || id != wanted) continue;
            const auto rank = language == 0x409 ? 2 : 1;
            if (rank <= best_rank) continue;
            auto text = std::wstring{};
            for (auto k = size_t{}; k + 1 < length; k += 2) text.push_back(static_cast<wchar_t>(u16(strings + offset + k)));
            best = std::move(text);
            best_rank = rank;
        }
        return best;
    }
    return {};
}

// The font's bytes added to this process only: from memory, else from a temporary file with FR_PRIVATE. Never the
// system's fonts. Once per font.
auto add_font(std::span<const std::byte> data) -> bool
{
    static auto added = std::unordered_map<const void*, bool>{};
    if (const auto found = added.find(data.data()); found != added.end()) return found->second;
    auto count = DWORD{};
    auto ok = AddFontMemResourceEx(const_cast<std::byte*>(data.data()), static_cast<DWORD>(data.size()), nullptr, &count) != nullptr && count > 0;
    if (!ok) {
        const auto bytes = std::string_view{reinterpret_cast<const char*>(data.data()), data.size()};
        auto error = std::error_code{};
        const auto path = std::filesystem::temp_directory_path(error) / (L"tinyplug-font-" + std::to_wstring(std::hash<std::string_view>{}(bytes)) + L".ttf");
        if (!error) {
            if (auto file = std::ofstream{path, std::ios::binary}) file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            ok = AddFontResourceExW(path.c_str(), FR_PRIVATE, nullptr) > 0;
        }
    }
    added[data.data()] = ok;
    return ok;
}

// The face GDI actually chose for `font`, which may not be the one asked for.
auto chosen_face(HFONT font) -> std::wstring
{
    auto face = std::wstring(LF_FACESIZE, L'\0');
    if (auto* dc = CreateCompatibleDC(nullptr)) {
        auto* old = SelectObject(dc, font);
        GetTextFaceW(dc, LF_FACESIZE, face.data());
        SelectObject(dc, old);
        DeleteDC(dc);
    }
    face.resize(std::wcslen(face.c_str()));
    return face;
}

auto make_font(const std::wstring& face, LONG height, LONG weight) -> HFONT
{
    auto lf = LOGFONTW{};
    lf.lfHeight = height;
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

// The editor's font for GDI, private to this process. GDI matches by name, and lists a variable font's weights under
// named instances ("Inter Variable Medium"); each candidate is kept only if GDI really chose it. Else the system's
// message font, as without bytes.
auto field_font(const Text_field::Font& font, double scale) -> HFONT
{
    const auto height = -static_cast<LONG>(std::lround(font.size * scale));
    const auto weight = std::clamp(static_cast<LONG>(std::lround(font.weight)), LONG{1}, LONG{1000});
    if (!font.data.empty() && add_font(font.data)) {
        static constexpr const wchar_t* instance_names[] = {L"Thin", L"ExtraLight", L"Light", L"Regular", L"Medium", L"SemiBold", L"Bold", L"ExtraBold", L"Black"};
        const auto hundred = std::clamp((weight + 50) / 100, LONG{1}, LONG{9});
        const auto family = font_name(font.data, 1);
        const auto typographic = font_name(font.data, 16);
        auto candidates = std::vector<std::pair<std::wstring, LONG>>{};
        if (!family.empty() && hundred != 4 && hundred != 7) candidates.emplace_back(family + L" " + instance_names[hundred - 1], LONG{400});
        if (!family.empty()) candidates.emplace_back(family, weight);
        if (!typographic.empty() && typographic != family) candidates.emplace_back(typographic, weight);
        for (const auto& [face, w] : candidates) {
            auto* candidate = make_font(face, height, w);
            if (candidate && _wcsicmp(chosen_face(candidate).c_str(), face.c_str()) == 0) {
#ifndef NDEBUG
                OutputDebugStringW((L"tinyplug: text field font " + face + L"\n").c_str());
#endif
                return candidate;
            }
            if (candidate) DeleteObject(candidate);
        }
#ifndef NDEBUG
        OutputDebugStringW((L"tinyplug: text field font not matched (" + family + L"), the system's instead\n").c_str());
#endif
    }
    auto face = std::wstring{L"Segoe UI"};
    auto metrics = NONCLIENTMETRICSW{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) face = metrics.lfMessageFont.lfFaceName;
    return make_font(face, height, weight);
}

auto field_text(HWND edit) -> std::string
{
    const auto length = GetWindowTextLengthW(edit);
    auto text = std::wstring(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return narrow(text);
}

// Ends `field` once: the answer goes out now, the windows close after this message.
auto finish_field(Win_field* field, Text_result::Kind kind) -> void
{
    if (field->finished) return;
    field->finished = true;
    if (const auto open = open_fields.find(field->editor); open != open_fields.end() && open->second == field) open_fields.erase(open);
    auto result = Text_result{.kind = kind, .text = field_text(field->edit)};
    auto done = std::move(field->done);
    field->done = {};
    ShowWindow(field->container, SW_HIDE);
    if (field->previous && IsWindow(field->previous)) SetFocus(field->previous);
    PostMessageW(field->container, WM_TINY_FIELD_CLOSE, 0, 0);
    if (done) done(std::move(result));
}

auto blur_kind(const Win_field* field) -> Text_result::Kind
{
    return field->blur == Text_field::Blur::Commit ? Text_result::Kind::Committed : Text_result::Kind::Cancelled;
}

// The field open over `editor`, if any, ends as its blur says: a click elsewhere in the editor, which takes no focus.
auto blur_field(HWND editor) -> void
{
    if (const auto open = open_fields.find(editor); open != open_fields.end()) finish_field(open->second, blur_kind(open->second));
}

LRESULT CALLBACK edit_callback(HWND edit, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data)
{
    auto* field = reinterpret_cast<Win_field*>(data);
    switch (message) {
        case WM_GETDLGCODE: // A host's dialog handling must not take Enter, Escape or Tab from us.
            return DefSubclassProc(edit, message, wparam, lparam) | DLGC_WANTALLKEYS;
        case WM_KEYDOWN:
            if (wparam == VK_RETURN) { finish_field(field, Text_result::Kind::Committed); return 0; }
            if (wparam == VK_ESCAPE) { finish_field(field, Text_result::Kind::Cancelled); return 0; }
            break;
        case WM_CHAR: // Their characters would beep in a single-line edit.
            if (wparam == L'\r' || wparam == 0x1B || wparam == L'\t') return 0;
            break;
        case WM_KILLFOCUS: {
            const auto result = DefSubclassProc(edit, message, wparam, lparam);
            finish_field(field, blur_kind(field));
            return result;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(edit, edit_callback, 1);
            break;
        default:
            break;
    }
    return DefSubclassProc(edit, message, wparam, lparam);
}

LRESULT CALLBACK field_callback(HWND container, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* field = reinterpret_cast<Win_field*>(GetWindowLongPtrW(container, GWLP_USERDATA));
    if (!field) return DefWindowProcW(container, message, wparam, lparam);
    switch (message) {
        case WM_CTLCOLOREDIT: {
            auto* dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, field->ink);
            SetBkColor(dc, field->background);
            return reinterpret_cast<LRESULT>(field->brush);
        }
        case WM_ERASEBKGND: {
            auto rect = RECT{};
            GetClientRect(container, &rect);
            FillRect(reinterpret_cast<HDC>(wparam), &rect, field->brush);
            return 1;
        }
        case WM_TINY_FIELD_CLOSE:
            DestroyWindow(container);
            return 0;
        case WM_NCDESTROY: {
            // Destroyed with its editor before it ended: it still answers, once.
            if (!field->finished) {
                field->finished = true;
                if (const auto open = open_fields.find(field->editor); open != open_fields.end() && open->second == field) open_fields.erase(open);
                if (auto done = std::move(field->done)) done({.kind = Text_result::Kind::Cancelled});
            }
            SetWindowLongPtrW(container, GWLP_USERDATA, 0);
            DeleteObject(field->font);
            DeleteObject(field->brush);
            delete field;
            return DefWindowProcW(container, message, wparam, lparam);
        }
        default:
            return DefWindowProcW(container, message, wparam, lparam);
    }
}

auto field_class() -> const std::wstring&
{
    static const auto name = [] {
        auto n = gen_random_name();
        const auto window_class = WNDCLASSW{
            .lpfnWndProc = field_callback,
            .hCursor = LoadCursor(nullptr, IDC_IBEAM),
            .lpszClassName = n.c_str(),
        };
        RegisterClassW(&window_class);
        return n;
    }();
    return name;
}

} // namespace

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
                auto ps = PAINTSTRUCT{};
                [[maybe_unused]] auto hdc = BeginPaint(window, &ps);

                // Reentrancy guard.
                if (!binder->painting) {
                    binder->painting = true;
                    const auto reset_painting = Deferred([binder]() { binder->painting = false; });

                    const auto time_now = System_clock::now();

#if !WIN_GRAPHICS_GPU
                    delegate->set_drawable(hdc);
#endif

                    binder->interaction.modifier_keys = resolve_modifiers();
                    binder->interaction.events = binder->events.consume(Steady_clock::now());

                    // Pointer absolute position.
                    if (auto cursor = POINT{}; GetCursorPos(&cursor))
                        binder->interaction.pointer_abs = {static_cast<double>(cursor.x), static_cast<double>(cursor.y)};

                    delegate->draw(binder->interaction, time_now); // Delegate window context handles everything.
                    binder->interaction.scroll_deltas = {};
                    binder->interaction.precise_scroll = false;
                }

#if !WIN_GRAPHICS_GPU
                ReleaseDC(window, hdc);
#endif

                EndPaint(window, &ps);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            blur_field(window);
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
            blur_field(window);
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

        // Accumulate: several wheel messages can land between two paints, and the
        // whole frame's travel matters when the deltas edit a parameter.
        case WM_MOUSEWHEEL: {
            const auto delta = resolve_wheel_delta(GET_WHEEL_DELTA_WPARAM(wparam));
            binder->interaction.scroll_deltas.y += delta.units;
            if (delta.precise) binder->interaction.precise_scroll = true;
            return 0;
        }

        case WM_MOUSEHWHEEL: {
            const auto delta = resolve_wheel_delta(GET_WHEEL_DELTA_WPARAM(wparam));
            binder->interaction.scroll_deltas.x += delta.units;
            if (delta.precise) binder->interaction.precise_scroll = true;
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

        // Opened here rather than where it was requested: see WM_TINY_RUN_DIALOG.
        // We are outside BeginPaint/EndPaint at this point, so the nested modal
        // loop can dispatch repaints normally and the editor keeps drawing.
        case WM_TINY_RUN_DIALOG: {
            run_queued_dialog(lparam);
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
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, // Children: a text field, which our paint mustn't cover.
        
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
    _token = Window_registry::add(_view); // Names this window for Platform_dialogs.
}

Platform_view::~Platform_view()
{
    Platform_text::cancel(_token);
    Window_registry::remove(_token);
    discard_queued_dialogs(static_cast<HWND>(_view)); // Posted but not yet dispatched.

    // Stop vsync thread before we destroy the window (don't want to call InvalidateRect on a destroyed window).
    _vsync_loop.reset();

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

// MARK: - Platform text

auto Platform_text::edit(const Text_field& spec, std::function<void(Text_result)> on_done, Dialog_context ctx) -> void
{
    // As the dialogs: the answer arrives through the task manager, on main, or not at all once it's shut down.
    auto deliver = [ctx, on_done = std::move(on_done)](Text_result result) {
        if (on_done) ctx.tasks.on_main([on_done, result = std::move(result)] { on_done(result); });
    };
    auto* editor = static_cast<HWND>(Window_registry::resolve(ctx.window));
    const auto* binder = editor ? reinterpret_cast<Platform_binder*>(GetWindowLongPtrW(editor, GWLP_USERDATA)) : nullptr;
    if (!binder || !binder->delegate) {
        deliver({.kind = Text_result::Kind::Cancelled});
        return;
    }
    blur_field(editor); // One per view.

    // Logical to client pixels, as the drawing is scaled.
    auto client = RECT{};
    GetClientRect(editor, &client);
    const auto logical = binder->delegate->get_size();
    const auto scale = logical.w > 0 && client.right > 0 ? static_cast<double>(client.right) / logical.w : 1.0;
    const auto px = [scale](double v) { return static_cast<int>(std::lround(v * scale)); };
    // Outwards to whole pixels: the field covers every pixel the frame touches, so no antialiased edge of what's
    // drawn under it shows (a slider at x 358.67 left a sliver).
    const auto x = static_cast<int>(std::floor(spec.frame.x * scale)), y = static_cast<int>(std::floor(spec.frame.y * scale));
    const auto w = static_cast<int>(std::ceil((spec.frame.x + spec.frame.w) * scale)) - x;
    const auto h = static_cast<int>(std::ceil((spec.frame.y + spec.frame.h) * scale)) - y;

    auto* field = new Win_field{
        .editor = editor,
        .font = field_font(spec.font, scale),
        .brush = CreateSolidBrush(colorref(spec.background)), // Opaque even at alpha 0: a native control can't show our pixels through.
        .ink = colorref(spec.ink),
        .background = colorref(spec.background),
        .previous = GetFocus(),
        .blur = spec.blur,
        .done = std::move(deliver),
    };
    field->container = CreateWindowExW(0, field_class().c_str(), widen(spec.label).c_str(), WS_CHILD | WS_CLIPCHILDREN, x, y, w, h, editor, nullptr, nullptr, nullptr);
    if (!field->container) {
        auto done = std::move(field->done);
        DeleteObject(field->font);
        DeleteObject(field->brush);
        delete field;
        done({.kind = Text_result::Kind::Cancelled});
        return;
    }
    SetWindowLongPtrW(field->container, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(field));

    // One line of the font, centred; `inset` across as the EDIT's margins.
    auto line = h;
    if (auto* dc = GetDC(field->container)) {
        auto* old = SelectObject(dc, field->font);
        auto tm = TEXTMETRICW{};
        if (GetTextMetricsW(dc, &tm)) line = std::min(h, static_cast<int>(tm.tmHeight) + 2);
        SelectObject(dc, old);
        ReleaseDC(field->container, dc);
    }
    const auto align = spec.align == Text_field::Align::Left ? ES_LEFT : spec.align == Text_field::Align::Right ? ES_RIGHT : ES_CENTER;
    field->edit = CreateWindowExW(0, WC_EDITW, widen(spec.initial).c_str(), WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | static_cast<DWORD>(align),
                                  0, (h - line) / 2, w, line, field->container, nullptr, nullptr, nullptr);
    if (!field->edit) {
        finish_field(field, Text_result::Kind::Cancelled);
        return;
    }
    SendMessageW(field->edit, WM_SETFONT, reinterpret_cast<WPARAM>(field->font), FALSE);
    const auto inset = static_cast<WORD>(std::max(0, px(spec.inset)));
    SendMessageW(field->edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(inset, inset));
    SetWindowSubclass(field->edit, edit_callback, 1, reinterpret_cast<DWORD_PTR>(field));

    open_fields[editor] = field;
    ShowWindow(field->container, SW_SHOW);
    SetFocus(field->edit);
    const auto end = static_cast<LPARAM>(GetWindowTextLengthW(field->edit));
    SendMessageW(field->edit, EM_SETSEL, spec.select_all ? 0 : static_cast<WPARAM>(end), end);
}

auto Platform_text::cancel(Window_token window) -> void
{
    auto* editor = static_cast<HWND>(Window_registry::resolve(window));
    if (const auto open = open_fields.find(editor); editor && open != open_fields.end()) finish_field(open->second, Text_result::Kind::Cancelled);
}

// MARK: - dialog support

// Exposes the editor window class name to win_dialogs.cpp so dialogs can
// locate the editor window and parent modal dialogs to it.
auto view_window_class_name() -> const std::wstring&
{
    return Window_registrar::instance().class_name;
}

} // namespace tiny
