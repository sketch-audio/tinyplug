// gui.hpp on Windows: a layered window at zero alpha for an editor to live in, and synthetic mouse
// messages sent straight to the child window under the point, as Windows would route them.
#include "gui.hpp"

#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

namespace tiny::hosts {

namespace {

constexpr auto window_class = L"tiny_hosts_window";

// Windows dialogs are modal: a click that opens one would park the host inside its loop forever.
// Cancel every dialog as it activates. The editor still sees a dialog come and go, just a short one.
auto CALLBACK dismiss_dialogs(int code, WPARAM wparam, LPARAM lparam) -> LRESULT
{
    if (code == HCBT_ACTIVATE) {
        const auto window = reinterpret_cast<HWND>(wparam);
        wchar_t name[16]{};
        if (GetClassNameW(window, name, 16) && !wcscmp(name, L"#32770")) PostMessageW(window, WM_COMMAND, IDCANCEL, 0);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

auto hwnd(void* p) -> HWND { return static_cast<HWND>(p); }

} // namespace

auto gui_init() -> void
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto wc = WNDCLASSEXW{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = window_class;
    RegisterClassExW(&wc);
    SetWindowsHookExW(WH_CBT, &dismiss_dialogs, nullptr, GetCurrentThreadId());
}

Window::Window(double width, double height)
{
    // A tool window, off the taskbar; layered at alpha 0 so nothing flashes up, but still shown, so
    // the editor's child window is visible to itself and paints.
    auto rect = RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_TOOLWINDOW);
    const auto window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, window_class, L"tinyplug fake host",
                                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, 100, 100, rect.right - rect.left, rect.bottom - rect.top,
                                        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    _window = window;
}

Window::~Window() { DestroyWindow(hwnd(_window)); }

auto Window::content() const -> void* { return _window; }

// Newly created children go to the top of the Z order, so the first child is the newest.
auto Window::editor_view() const -> void* { return GetWindow(hwnd(_window), GW_CHILD); }

auto Window::add(void* view) -> void { SetParent(hwnd(view), hwnd(_window)); }
auto Window::remove(void* view) -> void { SetParent(hwnd(view), nullptr); }

namespace {

// Straight to the deepest child under `at` (client coordinates of `root`), with coordinates made
// local to it. Sent, not posted: the handler runs before this returns, as on macOS.
auto deliver(HWND root, UINT message, POINT at, WPARAM keys) -> void
{
    auto target = root;
    auto local = at;
    for (;;) {
        const auto child = ChildWindowFromPointEx(target, local, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        if (!child || child == target) break;
        MapWindowPoints(target, child, &local, 1);
        target = child;
    }
    SendMessageW(target, message, keys, MAKELPARAM(local.x, local.y));
}

} // namespace

auto send_input(void* view, Random& random, int events) -> void
{
    const auto v = hwnd(view);
    auto client = RECT{};
    if (!v || !IsWindow(v) || !GetClientRect(v, &client) || client.right <= 0 || client.bottom <= 0) return;
    auto point = [&] {
        return POINT{static_cast<LONG>(random.real(0, client.right)), static_cast<LONG>(random.real(0, client.bottom))};
    };
    for (auto n = 0; n < events; ++n) {
        switch (random.below(6)) {
            case 0: { const auto p = point(); deliver(v, WM_LBUTTONDOWN, p, MK_LBUTTON); deliver(v, WM_LBUTTONUP, p, 0); break; }
            case 1: {
                auto p = point();
                deliver(v, WM_LBUTTONDOWN, p, MK_LBUTTON);
                for (auto k = random.below(8); k > 0; --k) {
                    p.x += static_cast<LONG>(random.real(-40, 40));
                    p.y += static_cast<LONG>(random.real(-40, 40));
                    deliver(v, WM_MOUSEMOVE, p, MK_LBUTTON);
                }
                deliver(v, WM_LBUTTONUP, p, 0);
                break;
            }
            case 2: {
                // What Windows sends for a double click: down, up, double-click, up.
                const auto p = point();
                deliver(v, WM_LBUTTONDOWN, p, MK_LBUTTON);
                deliver(v, WM_LBUTTONUP, p, 0);
                deliver(v, WM_LBUTTONDBLCLK, p, MK_LBUTTON);
                deliver(v, WM_LBUTTONUP, p, 0);
                break;
            }
            case 3: { const auto p = point(); deliver(v, WM_RBUTTONDOWN, p, MK_RBUTTON); deliver(v, WM_RBUTTONUP, p, 0); break; }
            case 4: deliver(v, WM_MOUSEMOVE, point(), 0); break;
            default: {
                // Wheel messages carry screen coordinates and go to the focus window; send them to
                // the view under the point instead, which is what a host's forwarding amounts to.
                auto p = point();
                auto screen = p;
                ClientToScreen(v, &screen);
                const auto message = random.chance(0.5) ? WM_MOUSEWHEEL : WM_MOUSEHWHEEL;
                const auto delta = static_cast<short>(random.real(-2, 2) * WHEEL_DELTA);
                auto target = v;
                for (;;) {
                    const auto child = ChildWindowFromPointEx(target, p, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
                    if (!child || child == target) break;
                    MapWindowPoints(target, child, &p, 1);
                    target = child;
                }
                SendMessageW(target, message, MAKEWPARAM(0, delta), MAKELPARAM(screen.x, screen.y));
                break;
            }
        }
    }
}

// AUv2 only.
auto make_au_cocoa_view(void*) -> void* { return nullptr; }
auto release_view(void*) -> void {}

} // namespace tiny::hosts
