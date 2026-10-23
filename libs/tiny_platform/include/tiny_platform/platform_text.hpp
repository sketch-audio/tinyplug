#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

#include <tiny_core/tiny_input.hpp> // Frame
#include <tiny_platform/platform_dialogs.hpp> // Dialog_context

namespace tiny {

// A native text field over the editor, for typing a value or a name: NSTextField (macOS), UITextField (iOS), an EDIT
// control (Windows), <input> (web). The OS owns focus, IME, dead keys, the clipboard, selection and accessibility, and
// a host honours a focused native field; nothing reads keys from the host.
struct Text_field {
    Frame frame{};                    // logical, in the view
    std::string initial{};
    std::string label{};              // what it's for ("Gain"): read by screen readers

    // The editor's own font, from its embedded bytes (registered nowhere); empty is the system's.
    struct Font {
        std::span<const std::byte> data{};
        float size{13};
        float weight{400};            // a variable font's `wght`
    } font{};

    std::uint32_t ink{0xff000000};    // SkColors
    std::uint32_t background{0xffffffff}; // alpha 0: transparent over the editor's drawing (a popover's panel); Windows
                                          // can't show a native control through, and paints the RGB opaque
    float inset{4};                   // horizontal padding inside `frame`, logical; the text is centred vertically

    enum class Align { Left, Center, Right } align{Align::Center};
    bool select_all{true};

    // Enter commits and Escape cancels either way; this is clicking away.
    enum class Blur { Commit, Cancel } blur{Blur::Commit};
};

struct Text_result {
    enum class Kind { Committed, Cancelled } kind{};
    std::string text{};

    auto committed() const -> bool { return kind == Kind::Committed; }
};

struct Platform_text {
    // Main thread. One field per view: opening another ends the first as its blur says. `on_done` fires exactly once,
    // through `ctx.tasks` on the main thread like the dialogs, Cancelled if the view closes first or no field could be
    // shown. A Task_manager that has shut down drops it, so it may capture `this`.
    static auto edit(const Text_field& field, std::function<void(Text_result)> on_done, Dialog_context ctx) -> void;

    // Ends the field open in `window`, if any, as Cancelled: the editor is hiding or the control went away.
    static auto cancel(Window_token window) -> void;
};

} // namespace tiny
