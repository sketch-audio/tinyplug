#pragma once

#include <array>
#include <vector>

#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// Six parameter strips over a two-octave keyboard. Each finger (or the mouse) plays its own
// note: where it lands on a key sets the velocity, sliding up presses harder (per-note
// pressure, which deepens that note's vibrato) and sliding sideways bends it (per-note tuning).
// Lit keys come from the processor, so notes from the host light up too.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {720, 360}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto notify(const Host_event&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void;
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

private:

    using Address = models::Params::Address;
    static constexpr auto num_params = User_params::num_params;
    static constexpr auto max_touches = size_t{10};

    struct Layout {
        std::array<Frame, num_params> strips{};
        Frame keys{};
    };

    // One finger on one key. The id is ours; the framework maps it, so two fingers on the same
    // key stay two notes.
    struct Touch {
        uintptr_t tag{};
        uint32_t id{};
        uint8_t key{};
        Coords down{};
        double pressure{};
        double tuning{};
        bool moved{};
        bool used{};
    };

    Edit_context _edit{};
    bool _dark{true};

    std::array<Touch, max_touches> _touches{};
    uint32_t _next_id{1};
    std::vector<midi::Note::Off> _unsent{}; // Offs the pipe refused; retried every frame.

    int _dragging{-1}; // Parameter strip under a drag, or -1.

    auto _layout(Rect_size size) const -> Layout;
    auto _press(uintptr_t tag, const Frame& keys, Coords pos) -> void;
    auto _slide(uintptr_t tag, const Frame& keys, Coords pos) -> void;
    auto _lift(uintptr_t tag) -> void;
    auto _set(int strip, const Frame& frame, Coords pos) -> void;
    auto _end_drag() -> void;

};

} // namespace tiny::edit
