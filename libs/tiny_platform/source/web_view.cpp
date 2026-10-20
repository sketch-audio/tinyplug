// The browser's Platform_view: a <canvas> the page names, drawn from requestAnimationFrame. Input arrives from the
// page's own listeners through the exports below (wrappers/web/js/editor.js), as window messages do on Windows.
#include <tiny_platform/platform_view.hpp>

#include <chrono>
#include <cmath>
#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <tiny_platform/window_context.hpp>

#include "window_registry.hpp"

namespace tiny {

namespace {

// What the page's input and the frame loop share. Outlives the view while a frame is still scheduled.
struct Web_binder {
    View_delegate* delegate{};
    std::string selector{};
    User_interaction interaction{};
    Event_stream events{};
    std::optional<Coords> left_down{};
    Coords last_pos{};
    Steady_time last_click{};
    Coords last_click_pos{};
    bool mouse_in{};
    bool dark{};
    bool shown{};
    bool looping{};
    bool dead{};
};

Web_binder* current{}; // One editor per controller module.

constexpr auto click_slop = 4.0; // Logical pixels between press and release, as the platform drag threshold.
constexpr auto double_click_time = std::chrono::milliseconds{500};

EM_JS(int, web_dark_mode, (), {
    return (globalThis.matchMedia && matchMedia("(prefers-color-scheme: dark)").matches) ? 1 : 0;
});

auto frame(double, void* user) -> EM_BOOL
{
    auto* b = static_cast<Web_binder*>(user);
    if (b->dead) {
        delete b;
        return EM_FALSE;
    }
    if (!b->shown || !b->delegate) {
        b->looping = false;
        return EM_FALSE;
    }
    if (const auto dark = web_dark_mode() != 0; dark != b->dark) {
        b->dark = dark;
        b->delegate->notify(Dark_mode_changed{dark});
    }
    b->interaction.events = b->events.consume(Steady_clock::now());
    b->delegate->draw(b->interaction, System_clock::now());
    b->interaction.scroll_deltas = {};
    b->interaction.precise_scroll = false;
    return EM_TRUE;
}

auto modifiers(int bits) -> Modifier_keys
{
    return {.primary = (bits & 1) != 0, .alt = (bits & 2) != 0, .shift = (bits & 4) != 0};
}

} // namespace

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()>)
    : _owns_view{owns_view}, _delegate{delegate}
{
    auto* binder = new Web_binder{.delegate = _delegate.get()};
    binder->dark = web_dark_mode() != 0;
    _delegate->notify(Dark_mode_changed{binder->dark});
    current = binder;
    _view = binder;
    _token = Window_registry::add(_view);
}

Platform_view::~Platform_view()
{
    Window_registry::remove(_token);
    auto* binder = static_cast<Web_binder*>(_view);
    if (current == binder) current = nullptr;
    binder->delegate = nullptr;
    if (binder->looping) binder->dead = true; // The scheduled frame frees it.
    else delete binder;
}

auto Platform_view::on_create() -> void {}
auto Platform_view::on_destroy() -> void {}

// `parent` is the canvas's CSS selector, e.g. "#editor".
auto Platform_view::receive_parent(void* parent) -> void
{
    auto* binder = static_cast<Web_binder*>(_view);
    binder->selector = static_cast<const char*>(parent);
    auto context = std::make_unique<Window_context>();
    context->setup({.native_handle = binder->selector.data()});
    _delegate->assign_context(std::move(context));
}

auto Platform_view::on_show() -> void
{
    auto* binder = static_cast<Web_binder*>(_view);
    binder->shown = true;
    if (!binder->looping) {
        binder->looping = true;
        emscripten_request_animation_frame_loop(&frame, binder);
    }
}

auto Platform_view::on_hide() -> void
{
    static_cast<Web_binder*>(_view)->shown = false; // The next frame stops the loop.
}

auto Platform_view::resize(int32_t w, int32_t h) -> void
{
    const auto* binder = static_cast<Web_binder*>(_view);
    emscripten_set_element_css_size(binder->selector.c_str(), w, h);
    _delegate->on_resize({w, h});
}

} // namespace tiny

// MARK: - input from the page, in logical (CSS) pixels relative to the canvas

extern "C" {

// kind: 0 down, 1 up, 2 move, 3 enter, 4 exit, 5 cancel. button: 0 left, 2 right (DOM numbering).
EMSCRIPTEN_KEEPALIVE auto tiny_web_pointer(int kind, double x, double y, int button, int tag, int mods) -> void
{
    using namespace tiny;
    auto* b = current;
    if (!b) return;
    const auto pos = Coords{x, y};
    const auto which = button == 2 ? Pointer_button::right : Pointer_button::left;
    const auto pointer_tag = static_cast<uintptr_t>(tag);
    b->interaction.modifier_keys = modifiers(mods);
    b->interaction.pointer_abs = pos;
    switch (kind) {
        case 0:
            b->events.push(Event{.event = Pointer_down{which, pos}, .pointer_tag = pointer_tag});
            if (which == Pointer_button::left) b->left_down = pos;
            break;
        case 1: {
            b->events.push(Event{.event = Pointer_up{which, pos}, .pointer_tag = pointer_tag});
            const auto start = which == Pointer_button::left ? b->left_down : std::optional<Coords>{pos};
            if (start && std::abs(pos.x - start->x) < click_slop && std::abs(pos.y - start->y) < click_slop) {
                const auto now = Steady_clock::now();
                const auto near = std::abs(pos.x - b->last_click_pos.x) < click_slop && std::abs(pos.y - b->last_click_pos.y) < click_slop;
                const auto count = (which == Pointer_button::left && near && now - b->last_click < double_click_time) ? 2u : 1u;
                b->events.push(Event{.event = Pointer_click{which, count, pos}, .pointer_tag = pointer_tag});
                b->last_click = count == 2 ? Steady_time{} : now;
                b->last_click_pos = pos;
            }
            if (which == Pointer_button::left) b->left_down.reset();
            break;
        }
        case 2:
            if (!b->mouse_in) {
                b->events.push(Event{.event = Pointer_enter{pos}, .pointer_tag = pointer_tag});
                b->mouse_in = true;
            }
            b->events.push(Event{.event = Pointer_move{pos}, .pointer_tag = pointer_tag});
            break;
        case 3:
            b->events.push(Event{.event = Pointer_enter{pos}, .pointer_tag = pointer_tag});
            b->mouse_in = true;
            break;
        case 4:
            b->events.push(Event{.event = Pointer_exit{pos}, .pointer_tag = pointer_tag});
            b->mouse_in = false;
            break;
        case 5:
            b->events.push(Event{.event = Pointer_cancel{pos}, .pointer_tag = pointer_tag});
            b->left_down.reset();
            break;
        default: break;
    }
    b->last_pos = pos;
}

// Deltas in lines (precise = 0) or pixels (precise = 1), as the DOM's deltaMode reports them.
EMSCRIPTEN_KEEPALIVE auto tiny_web_wheel(double dx, double dy, int precise, int mods) -> void
{
    auto* b = tiny::current;
    if (!b) return;
    b->interaction.scroll_deltas.x += dx;
    b->interaction.scroll_deltas.y += dy;
    if (precise) b->interaction.precise_scroll = true;
    b->interaction.modifier_keys = tiny::modifiers(mods);
}

} // extern "C"
