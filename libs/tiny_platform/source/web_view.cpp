// The browser's Platform_view: a <canvas> the page names, drawn from requestAnimationFrame. Input arrives from the
// page's own listeners through the exports below (wrappers/web/js/editor.js), as window messages do on Windows.
#include <tiny_platform/platform_view.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <tiny_platform/platform_text.hpp>
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
        b->delegate->set_dark(dark);
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

// MARK: - text fields

// An <input> over the canvas, in the editor's font (a FontFace from its bytes, once per font). Enter commits,
// Escape cancels, blur does what the field says; its keys stop at the input, so the page's shortcuts don't fire.
EM_JS(void, web_text_open, (std::uint32_t id, const char* selector, double x, double y, double w, double h, const char* initial, const char* label,
                            const std::byte* font, std::size_t font_len, float size, float weight, std::uint32_t ink, std::uint32_t background,
                            float inset, int align, int select_all, int blur_commits), {
    const canvas = document.querySelector(UTF8ToString(selector));
    if (!canvas) { _tiny_web_text_done(id, 0, 0); return; }
    const css = (c) => `rgba(${(c >>> 16) & 255},${(c >>> 8) & 255},${c & 255},${((c >>> 24) & 255) / 255})`;
    let family = "system-ui, sans-serif";
    if (font_len > 0) {
        if (!globalThis.tinyFonts) globalThis.tinyFonts = new Map();
        if (!globalThis.tinyFonts.has(font)) {
            const name = `tinyplug-font-${font}`;
            const bytes = new Uint8Array(font_len);
            bytes.set(HEAPU8.subarray(font, font + font_len)); // A copy: FontFace can't take shared memory.
            const face = new FontFace(name, bytes.buffer, { weight: "1 1000" });
            document.fonts.add(face);
            face.load();
            globalThis.tinyFonts.set(font, name);
        }
        family = `"${globalThis.tinyFonts.get(font)}"`;
    }
    const input = document.createElement("input");
    Object.assign(input, { type: "text", value: UTF8ToString(initial), spellcheck: false, autocomplete: "off" });
    const aria = UTF8ToString(label);
    if (aria) input.setAttribute("aria-label", aria);
    const rect = canvas.getBoundingClientRect();
    Object.assign(input.style, {
        position: "fixed", left: `${rect.left + x}px`, top: `${rect.top + y}px`, width: `${w}px`, height: `${h}px`,
        boxSizing: "border-box", margin: "0", padding: `0 ${inset}px`, border: "none", outline: "none", borderRadius: "0",
        fontFamily: family, fontSize: `${size}px`, fontWeight: String(weight), fontVariationSettings: `"wght" ${weight}, "opsz" ${size}`,
        color: css(ink), background: css(background), caretColor: css(ink), textAlign: ["left", "center", "right"][align], zIndex: "2147483647",
    });
    let ended = false;
    const end = (kind) => {
        if (ended) return;
        ended = true;
        globalThis.tinyFields.delete(id);
        const text = stringToNewUTF8(input.value);
        input.remove();
        _tiny_web_text_done(id, kind, text);
        _free(text);
    };
    if (!globalThis.tinyFields) globalThis.tinyFields = new Map();
    globalThis.tinyFields.set(id, end);
    input.addEventListener("keydown", (e) => {
        e.stopPropagation();
        if (e.isComposing) return; // An IME's Enter confirms its composition, not the field.
        if (e.key === "Enter") { e.preventDefault(); end(1); }
        else if (e.key === "Escape") { e.preventDefault(); end(0); }
    });
    input.addEventListener("keyup", (e) => e.stopPropagation());
    input.addEventListener("blur", () => end(blur_commits ? 1 : 0));
    document.body.appendChild(input);
    input.focus();
    if (select_all) input.select();
    else input.setSelectionRange(input.value.length, input.value.length);
});

// Ends field `id` as `kind` (1 committed, 0 cancelled), if it's still open.
EM_JS(void, web_text_end, (std::uint32_t id, int kind), {
    const end = globalThis.tinyFields && globalThis.tinyFields.get(id);
    if (end) end(kind);
});

struct Web_field {
    std::function<void(Text_result)> done{};
    Window_token window{};
    bool blur_commits{};
};

std::map<std::uint32_t, Web_field> fields{};
std::uint32_t next_field{1};

// Ends the field open in `window` as `kind`, or as its blur says.
auto end_field(Window_token window, std::optional<int> kind) -> void
{
    for (const auto& [id, field] : fields) {
        if (field.window != window) continue;
        web_text_end(id, kind.value_or(field.blur_commits ? 1 : 0));
        return; // web_text_end reported back synchronously, and `fields` changed.
    }
}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE auto tiny_web_text_done(std::uint32_t id, int kind, const char* text) -> void
{
    const auto found = fields.find(id);
    if (found == fields.end()) return;
    auto done = std::move(found->second.done);
    fields.erase(found);
    if (done) done({.kind = kind == 1 ? Text_result::Kind::Committed : Text_result::Kind::Cancelled, .text = text ? text : ""});
}

auto Platform_text::edit(const Text_field& field, std::function<void(Text_result)> on_done, Dialog_context ctx) -> void
{
    auto deliver = [ctx, on_done = std::move(on_done)](Text_result result) {
        if (on_done) ctx.tasks.on_main([on_done, result = std::move(result)] { on_done(result); });
    };
    const auto* binder = static_cast<Web_binder*>(Window_registry::resolve(ctx.window));
    if (!binder || binder->selector.empty()) {
        deliver({.kind = Text_result::Kind::Cancelled});
        return;
    }
    end_field(ctx.window, std::nullopt); // One per view.
    const auto id = next_field++;
    fields[id] = {std::move(deliver), ctx.window, field.blur == Text_field::Blur::Commit};
    web_text_open(id, binder->selector.c_str(), field.frame.x, field.frame.y, field.frame.w, field.frame.h, field.initial.c_str(), field.label.c_str(),
                  field.font.data.data(), field.font.data.size(), field.font.size, field.font.weight, field.ink, field.background,
                  field.inset, static_cast<int>(field.align), field.select_all ? 1 : 0, field.blur == Text_field::Blur::Commit ? 1 : 0);
}

auto Platform_text::cancel(Window_token window) -> void
{
    end_field(window, 0);
}

Platform_view::Platform_view(std::shared_ptr<View_delegate> delegate, bool owns_view, std::function<void()>)
    : _owns_view{owns_view}, _delegate{delegate}
{
    auto* binder = new Web_binder{.delegate = _delegate.get()};
    binder->dark = web_dark_mode() != 0;
    _delegate->set_dark(binder->dark);
    current = binder;
    _view = binder;
    _token = Window_registry::add(_view);
}

Platform_view::~Platform_view()
{
    Platform_text::cancel(_token);
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
