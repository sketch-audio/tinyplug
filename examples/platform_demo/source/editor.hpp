#pragma once

#include <array>
#include <deque>
#include <memory>
#include <string>

#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// A quick check of the platform layer. Left, a pad watched by every gesture recognizer, each
// with a lamp that flashes on its callbacks and a count; beneath it the raw pointer events,
// modifiers and scroll. Right, one button per dialog, the last result, and what the window
// reports about itself.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {680, 480}; }

    Editor(const Edit_context& edit) : _edit{edit} { _make_probes(); }
    ~Editor() = default;

    auto on_gui_create(Gui_info info) -> void { _window = info.window; } // Dialogs hang off this window.
    auto on_gui_show() -> void {}
    auto notify(const Host_event&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void { _controls.cancel(_edit.actions); }
    auto on_gui_destroy() -> void {} // Keeps `_window`: a retired token already resolves to nothing.

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using Address = models::Params::Address;

    // One recognizer, and what it has said lately.
    struct Probe {
        const char* name{};
        std::unique_ptr<Gesture_recognizer> recognizer{};
        uint32_t count{};
        double flash{};        // Seconds of lamp left.
        const char* phase{""}; // The last callback.
    };

    Edit_context _edit{};
    Window_token _window{};
    bool _dark{true};
    ui::Controls _controls{};

    std::array<Probe, 7> _probes{};
    Frame _pad{};
    std::optional<Drag_info> _drag{};
    std::deque<std::string> _raw{};
    Coords _scroll{};
    std::string _result{"NO DIALOG YET"};

    Time_point _last{};
    double _fps{};

    auto _make_probes() -> void;
    auto _dialog(int which) -> void;

};

} // namespace tiny::edit
