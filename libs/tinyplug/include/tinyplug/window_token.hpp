#pragma once

#include <cstdint>

namespace tiny {

// An opaque handle to a live plug-in view. Minted by `Platform_view` when the
// window opens, retired when it closes, and threaded down to `Platform_dialogs`
// so a dialog parents itself to the window that asked for it instead of guessing
// at whatever the host happens to have key.
//
// A token rather than a native pointer on purpose: a dialog callback can fire
// long after the window that opened it has gone (a chained dialog, a network
// round-trip), and resolving a retired token yields nothing instead of a dangling
// view. Trivially copyable, safe to store in a deferred callback.
struct Window_token {
    uint64_t id{};

    explicit operator bool() const { return id != 0; }
    friend auto operator==(const Window_token&, const Window_token&) -> bool = default;
};

// Handed to the editor's `on_gui_create`. A struct so per-window facts can be
// added later without another signature break.
struct Gui_info {
    Window_token window{};
};

} // namespace tiny
