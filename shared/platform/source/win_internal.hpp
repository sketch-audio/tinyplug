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

namespace tiny {

// The editor window's registered class name. Dialogs use it to locate the
// editor window so they can parent modal dialogs to it. Defined in win_view.cpp.
auto view_window_class_name() -> const std::wstring&;

// Dark-mode helpers, shared by the view and the dialogs. Defined in win_view.cpp.
auto is_dark_mode() -> bool;
auto enable_dark_title_bar(HWND hwnd, bool dark) -> void;

} // namespace tiny
