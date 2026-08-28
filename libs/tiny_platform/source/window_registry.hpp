#pragma once

#include <tinyplug/window_token.hpp>

namespace tiny {

// Token -> native view (NSView* / UIView* / HWND) for the views this binary has
// alive.
//
// Private to the platform library on purpose. Nothing outside it can ask "what is
// the current window" — only "is *this* token still live, and what view is it".
// That is the whole difference between this and the process-wide guessing
// (`[NSApp keyWindow]`, `EnumChildWindows`) it replaces: a caller has to name the
// window it means.
//
// One table per loaded plug-in binary, which is the right granularity — a token
// never has to mean anything to anyone else. Registration happens on the UI
// thread; Windows resolves from a background thread, so the table is locked.
struct Window_registry {
    static auto add(void* native) -> Window_token;
    static auto remove(Window_token token) -> void;

    // The view for `token`, or nullptr once it has been retired (or was never set).
    static auto resolve(Window_token token) -> void*;

    // The only registered view, or nullptr when there are none or more than one.
    // The fallback for a caller that has not been migrated to pass a token: being
    // unambiguous by construction, it can never pick the wrong instance.
    static auto sole() -> void*;
};

} // namespace tiny
