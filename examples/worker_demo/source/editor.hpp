#pragma once

#include <array>

#include <tiny_ui/controls.hpp>
#include <tinyplug/tinyplug.hpp>

namespace tiny::edit {

// The three parties and the four channels between them, each with its count, a lamp that
// flashes as messages cross, and round-trip times; beneath, the curve the processor shapes with
// and the Drive that asks for it.
class Editor {
public:

    static auto preferred_size() -> Rect_size { return {720, 420}; }

    Editor(const Edit_context& edit) : _edit{edit} {}
    ~Editor() = default;

    auto on_gui_create(Gui_info) -> void {}
    auto on_gui_show() -> void {}
    auto notify(const Host_event&) -> void;
    auto on_gui_draw(Plugin_state&) -> void;
    auto on_gui_hide() -> void { _controls.cancel(_edit.actions); }
    auto on_gui_destroy() -> void {}

    auto save_state() -> State_map { return {}; }
    auto load_state(const State_map&) -> void {}

    auto bind_worker(Worker_editor_actor worker) -> void { _worker = worker; }
    auto on_worker_reply(const User_work::To_editor& reply) -> void;

private:

    using Address = models::Params::Address;
    using Block = models::Blocks::Address;

    // One direction of one channel: messages seen, and the lamp.
    struct Leg {
        uint32_t count{};
        double flash{};
    };

    Edit_context _edit{};
    bool _dark{true};
    ui::Controls _controls{};
    Worker_editor_actor _worker{};

    std::array<Leg, 4> _legs{}; // Editor -> worker, worker -> editor, processor -> worker, worker -> processor.
    uint32_t _ping_seq{};
    int64_t _next_ping_ns{};
    double _pong_ms{};
    uint32_t _designs{};
    int64_t _now_ns{};
    Time_point _last{};

};

} // namespace tiny::edit
