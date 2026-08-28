#pragma once

// Internal interface shared between win_view.cpp and win_dialogs.cpp.

#include <string>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

// Reset-cursor message: dialogs post this to the editor window after closing.
#define WM_TINY_SETCURSOR (WM_APP + 1)

// Run-a-dialog message. A modal dialog cannot be opened from inside the editor's
// WM_PAINT: DialogBox* spins a nested message loop while that paint is still on
// the stack, so every repaint the loop dispatches hits the reentrancy guard and
// the editor draws nothing until the dialog closes. Gesture callbacks *do* run
// inside WM_PAINT (via delegate->draw), so dialogs post this to themselves and
// open from the window proc instead, off the paint stack. LPARAM owns a heap
// request; the handler frees it.
#define WM_TINY_RUN_DIALOG (WM_APP + 2)

namespace tiny {

// Runs and frees the request in `lparam`. Defined in win_dialogs.cpp.
auto run_queued_dialog(LPARAM lparam) -> void;

// Frees any queued requests still posted to `hwnd`, for teardown. Their
// callbacks do not fire — the window they belong to is going away.
auto discard_queued_dialogs(HWND hwnd) -> void;

// The editor window's registered class name. Defined in win_view.cpp.
auto view_window_class_name() -> const std::wstring&;

// Dark-mode helpers, shared by the view and the dialogs. Defined in win_view.cpp.
auto is_dark_mode() -> bool;
auto enable_dark_title_bar(HWND hwnd, bool dark) -> void;

} // namespace tiny
