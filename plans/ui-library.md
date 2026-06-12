# Plan: tinyplug UI library (`tiny::ui`)

## Context

The framework at `/Users/ryan/Developer/tinyplug/` ships everything needed
to wrap a `Plug_processor` + `Plug_editor` into AAX/AUv2/AUv3/CLAP/VST3,
**except** anything in the editor itself: the framework hands the editor
a Skia canvas, an `Edit_context`, and a `Plugin_state` per frame, then
walks away. Every plug-in author has to invent the layout system, the
control library, and the presentation/modal mechanics from scratch.

Downstream, the `sketch-audio/all_plugins` repo
(`/Users/ryan/Developer/all_plugins/shared/sui/`) has evolved a
SwiftUI-style declarative UI system over the past year — `View` variant
tree, `Layout_resolver`, `Ui_element` interface, key-value `Layout_state`,
descriptor-based settings table. It works well and the call sites read
nicely. But it has three rough edges:

1. **`Environment` is a concrete 15-field struct** baked with plug-in-
   family-specific actors (`Ab_settings`, `Auto_gain`, `Morph_actions`,
   `License_checker`). Nothing about that belongs in a framework type.
2. **Modal / popover / tooltip are mutually exclusive.** A popover hides
   the tooltip; a modal hides the popover; you can't stack a confirmation
   modal on top of a settings popover. Hit-testing is gated by a chain
   of `if (!showing_modal && !showing_popover)` short-circuits.
3. **Control boilerplate.** Every draggable repeats the same
   over/down/click/dwell recognizer setup, every value-bearing control
   reaches for the global `Value_conv`/`Host_formatter` functions, and
   controls cache a `Plugin_state*` between hooks that's only valid for
   one frame.

This plan lifts the working parts into the framework as `tiny::ui`,
genericizes the environment, generalizes the presentation surface, and
ships a bundled basic control library while preserving the terse
declarative call sites. Downstream `all_plugins` keeps running against
the old `sui::` until follow-up migration.

User-stated goals:
- Type-erased, extensible `Env` (drop the concrete struct).
- Layout engine that gracefully handles **stacking** modals, popovers,
  tooltips.
- Preserve the SwiftUI-style declarative layout with state/action
  lambdas injected into controls.
- Preserve the four-phase control lifecycle.
- Ship a basic control library.
- Make it easy for plug-in authors to implement their own controls.

## Approach

### Library shape

New CMake target `tiny_ui` (static library) as a new optional lib under
`libs/tiny_ui/` — public headers `include/tiny_ui/`, implementations `source/`,
its own `CMakeLists.txt` — mirroring the existing `tiny_platform` / `tiny_dsp`
shape (see the optional-lib dependency rules in
[plans/refactor-ideas.md](refactor-ideas.md)). The `libs/` layout already exists,
so `tiny_ui` drops in as another peer.

```
libs/ui/
├── include/tinyplug/ui/
│   ├── ui.h                # umbrella public header
│   ├── view.h              # View, Column, Row, Box (+later Z_stack/If/For)
│   ├── layout.h            # Layout, Layout_context, Layout_gen, Layout_state
│   ├── element.h           # Element interface + Some_element concept
│   ├── env.h               # Env, Env_key concept, Scoped_override
│   ├── presentation.h      # Presentation_stack, Entry, Dismiss_policy
│   ├── resolver.h          # Layout_resolver (presentation-aware)
│   ├── composer.h          # Composer (editor runtime)
│   ├── gesture_pack.h      # bundled gesture recognizers
│   ├── resolvable.h        # Resolvable_color/_string/_icon
│   ├── theme.h             # Colors, Font_set, default palette
│   ├── controls/           # bundled controls (one header each)
│   └── settings.h          # descriptor-based settings table
└── src/                    # implementations
```

Dependency: `tiny_ui` depends PUBLIC on `tiny_core` (params, events,
view, gestures, edit, tasks) and on `tiny_platform` (Skia canvas, font
lookup). It must **not** depend on any format wrapper or `Plug_editor`.

### Namespacing

Per user direction — sub-namespaces for the **big ideas only**, not for
every file:

- `tiny::ui::` — `View`, `Column`, `Row`, `Box`, `Sizing`, `Insets`,
  `Spacing`, `Alignment`, `Element`, `Layout`, `Layout_state`,
  `Layout_context`, `Composer`, `Resolvable_color`, `Theme`, controls
  (`Knob`, `Button`, etc.), settings types. The default landing zone.
- `tiny::ui::env::` — the bundled environment keys (`Colors`, `Edit`,
  `Modal`, `Popover`, `Tooltip`, etc.). Keeping them in their own
  namespace prevents the global pollution of registering ~15 key types,
  and reads cleanly: `env.set<env::Colors>(palette)`.
- `tiny::ui::detail::` — `Inline_visitor`, axis math, internal
  resolution helpers.

No `tiny::ui::layout::`, `tiny::ui::controls::`, or
`tiny::ui::settings::` sub-namespaces. Flat is fine; the type names are
already distinctive (`Column`, `Knob`, `Cell`).

### Type-erased `Env`

Replace the concrete `Environment` struct with a key-value map keyed by
`std::type_index`, stored as a small flat vector of `unique_ptr<Storage_base>`.
Linear scan beats hashing for the ~15-key realistic working set.

```cpp
namespace tiny::ui {

template<class K>
concept Env_key = requires { typename K::Value; }
              && std::is_default_constructible_v<K>;

class Env {
public:
    template<Env_key K> auto get() const -> const typename K::Value&;
    template<Env_key K> auto try_get() const -> const typename K::Value*;
    template<Env_key K> auto set(typename K::Value v) -> void;

    // RAII scope: pops on destruction. For sub-tree env overrides.
    template<Env_key K>
    [[nodiscard]] auto with(typename K::Value v) -> Scoped_override;
};

}
```

**Defaults.** If `K::default_value()` is detectable, `get<K>()` falls
back to it; otherwise unbound `get` asserts in debug. Most framework
keys provide defaults (`env::Time` → `steady_clock::now()`, `env::Dark_mode`
→ `false`); a couple require explicit registration (`env::Edit` — there
is no sensible default).

**Sub-tree scopes.** Reused from SwiftUI's `.environment(...)`
modifier. Implemented as a new `Env_modifier` view wrapper visited by
the resolver's existing `propagate_environment` pass:

```cpp
Env_modifier{
    .overrides = {
        make_override<env::Colors>(my_alt_palette),
        make_override<env::Scale>(0.85)
    },
    .content = Column{ /* ... */ }
}
```

The resolver pushes the override stack before descending and pops on
exit. Controls receiving `on_environment(env)` see the override.

**Performance.** `env.get<K>()` is called per control per frame.
Controls should grab keys they need *once* in `on_environment` and cache
pointers/copies — this is the load-bearing usage pattern. The vector
scan is the slow path; if it becomes a hot spot, an `unordered_map`
specialization is a swap-in fix.

**Framework-shipped keys** in `tiny::ui::env::`:

| Key | `Value` |
|---|---|
| `Colors` | `Colors` (default palette) |
| `Dark_mode` | `bool` |
| `Fonts` | `Font_set` |
| `Edit` | `tiny::Edit_context` (required) |
| `Scale` | `double` |
| `Time` | `tiny::Time_point` |
| `Modifiers` | `tiny::Modifier_keys` |
| `Format` | `tiny::Format` |
| `Tasks` | `tiny::Task_manager::Actor` |
| `Param_specs` | `std::span<const tiny::Param_spec>` |
| `Meter_specs` | `std::span<const tiny::Meter_spec>` |
| `Modal` | `Sender<Modal_request>` |
| `Popover` | `Sender<Popover_request>` |
| `Tooltip` | `Sender<Popover_request>` |
| `Formatters` | `Formatter_pack` |
| `Tooltip_text` | `std::function<std::string(uint32_t)>` |

Plug-in authors register their own keys (`Auto_gain`, `Preset_history`,
`License_checker`, …) by declaring a `struct Foo_key { using Value = ...; }`
and `env.set<Foo_key>(v)` at composer build.

The current `Concrete_env` struct downstream becomes — over time — a
pile of `env.set<...>(...)` calls inside `register_env`.

### Presentation stack

Replace `Layout_resolver`'s three mutually-exclusive `_modal`,
`_popover`, `_tooltip` optional slots with a single `Presentation_stack`:

```cpp
namespace tiny::ui {

enum class Presentation_kind { modal, popover, tooltip };

struct Dismiss_policy {
    bool outside_tap{};     // pointer-down outside frame dismisses
    bool any_pointer_down{};// any pointer-down dismisses
    bool hover_out{};       // pointer leaves anchor frame dismisses
    bool escape_key{};      // escape (when wired) dismisses
};

struct Presentation_entry {
    Presentation_kind kind{};
    uint64_t id{};
    Layout layout{};
    tiny::Frame frame{};
    std::optional<tiny::Frame> anchor{};
    Layout_state state{};
    Dismiss_policy dismiss{};
    bool greedy{};         // swallow events outside frame
    std::function<void()> on_dismiss{};
};

class Presentation_stack {
    std::vector<Presentation_entry> _stack;
public:
    auto push(Presentation_entry) -> uint64_t;
    auto remove(uint64_t id) -> void;
    auto top_of_kind(Presentation_kind) const -> const Presentation_entry*;
    auto entries() const -> std::span<const Presentation_entry>;
};

}
```

**Update order.** Resolver walks the stack top-down for `update`. Each
entry's gesture surfaces consume pointer events that fall inside its
frame; entries with `greedy = true` also swallow events outside (modal
backdrop behavior). The base layout updates last and only if no greedy
entry exists.

**Draw order.** Bottom-up: base layout first, then each stack entry in
push order. Within an entry the existing per-`Box` `z_index` still
applies.

**Per-kind dismissal defaults** (overridable per `push`):
- Modal: `escape_key = true`, `greedy = true`. No outside-tap. Explicit
  dismissal via `Sender<Modal_request>::send(Dismiss{id})`.
- Popover: `outside_tap = true`, `escape_key = true`. Not greedy —
  parent layout can still receive events.
- Tooltip: `hover_out = true`, `any_pointer_down = true`. Tooltips are
  never greedy and never absorb clicks.

**Stacking semantics** (the key new capability):
- Tooltips coexist with modals and popovers (always layer above).
- A popover can spawn a modal (the modal sits above the popover; both
  visible; the modal's greedy flag swallows clicks meant for the
  popover until the modal is dismissed).
- Popovers can stack on top of other popovers (a sub-menu, say). Pushing
  a popover does not auto-dismiss earlier popovers; the caller decides.
- Tooltip is kind-unique: pushing a tooltip implicitly removes the
  previous tooltip.

**Force-cancel of stale gesture state.** Today the resolver synthesizes
`Pointer_cancel` events on the underlying layout when a popover
appears, so half-pressed knobs don't keep tracking. Generalize: each
`Presentation_stack::push` carries a `cancel_layers_below` flag
(default `true`), and the resolver emits a single cancel pass through
all layers beneath the new entry.

**Anchored positioning.** Keep the existing `calc_popover` heuristic
verbatim (prefer-bottom-then-top-then-leading-then-trailing with
off-screen clamping). Tooltip positioning reuses it.

### Declarative DSL

Keep the existing variant tree mostly intact:

```cpp
namespace tiny::ui {

using View = std::variant<Column, Row, Box /* later: Z_stack, If, For */>;

struct Column {
    Sizing sizing{};
    Alignment alignment{};
    Insets insets{};
    Spacing spacing{};
    Element* overlay{};
    Element* background{};
    std::vector<View> content{};
};
// Row, Box analogous

}
```

`Sizing_rule = variant<Fill, Fix, Relative, Measure>` — unchanged.
`Layout_gen = std::function<View(Layout_context)>` — unchanged. The
`Layout_context` carries `Layout::Ui_elements`, a `Layout_state::Const_accessor`,
and a `const Env&` so generators can branch on env keys without closing
over them.

**`Layout_state` stays separate from `Env`.** Different lifetimes, different
mutation patterns — Env is read-mostly per render; Layout_state is the
mutable UI state (panel toggles, scroll offsets, active tab index).
Widen `Layout_state::Value` to `variant<bool, int32_t, double, std::string>`
to absorb the `unordered_map<string, float> scroll_positions` side-channel
that lives on `Environment` today.

**Deferred for a later PR (sketched here for design):**

```cpp
struct Z_stack {
    Sizing sizing{};
    Insets insets{};
    Element* background{};
    Element* overlay{};
    std::vector<std::pair<std::optional<int32_t>, View>> content{};
    // Each child has an optional explicit z; default is push order.
};

struct If {
    std::function<bool(Layout_context)> cond{};
    Layout_gen then_branch{};
    Layout_gen else_branch{};  // optional; empty -> Box{}
    // Resolver: if (cond(ctx)) emit then_branch(ctx) else else_branch(ctx)
};

template<class T>
struct For {
    std::vector<T> items{};
    std::function<View(const T&, Layout_context)> each{};
    // Resolver expands into a Column or Row depending on parent context;
    // first iteration: keep it explicit — wrap in Column{.content = For{...}}.
    // Variant-of-views flattening is done at expansion time.
};
```

`Z_stack` replaces deeply-nested `Box{.background=..., .overlay=...}`
chains; `If` replaces the `switch (panel_index)` pattern; `For` is
syntactic sugar over building `vector<View>` manually. None are
load-bearing — every existing layout works without them — so they ship
in PR-7 after the core engine stabilizes.

### `Element` interface

Public, virtual, four hooks (matches today exactly but cleaner
signatures):

```cpp
namespace tiny::ui {

class Element {
public:
    virtual ~Element() = default;
    virtual auto on_environment(const Env& env) -> void {}
    virtual auto on_frame(const tiny::Frame& frame) -> void {}
    virtual auto on_update(const tiny::Plugin_state& state) -> void {}
    virtual auto on_draw(const tiny::Plugin_state& state) -> void {}
};

template<class T>
concept Some_element = requires(T t, const Env& e, const tiny::Frame& f,
                                const tiny::Plugin_state& s) {
    { t.on_environment(e) } -> std::same_as<void>;
    { t.on_frame(f) } -> std::same_as<void>;
    { t.on_update(s) } -> std::same_as<void>;
    { t.on_draw(s) } -> std::same_as<void>;
};

}
```

Differences from today's `sui::Ui_element`:
- `Env` passed by `const&`, not `shared_ptr<const Environment>`. Env
  lifetime is the editor's lifetime; ownership is unnecessary.
- `on_update`/`on_draw` take `const Plugin_state&`. Controls emit
  through `Edit_context::actions.push(...)`, they don't mutate state.
- Controls **must not** stash `Plugin_state*` between hooks. Anything
  remembered across frames is the control's own member. (Plugin_state
  lifetime is one frame; the framework guarantees nothing else.)

**Narrow contexts.** Two convenience projections, constructed by the
`Composer` per frame, that controls should prefer over the raw `Env`:

```cpp
struct State_view {
    const Colors& colors;
    bool dark;
    tiny::Format format;
    Layout_state::Const_accessor layout_state;
    std::span<const double> param_values;
    std::span<const double> meter_values;
    std::span<const tiny::Param_spec> param_specs;
    std::span<const tiny::Meter_spec> meter_specs;
    tiny::Modifier_keys modifier_keys;
    const Formatter_pack& formatters;
};

struct Action_context {
    tiny::Edit_context edit;
    tiny::Task_manager::Actor tasks;
    Layout_state::Deferred_accessor layout_state;
    Sender<Modal_request> modal;
    Sender<Popover_request> popover;
    Sender<Popover_request> tooltip;
};
```

Spec lambdas continue to take these (e.g. `Knob::Spec::state =
[](const State_view&) -> double { ... }`). Plug-in authors can compose
their own extended views by aggregating these as members of a richer
type.

### `Gesture_pack` and `Formatter_pack`

`Gesture_pack` bundles the over/down/click/dwell/drag recognizers a
control typically wants — replacing the ~30-line `_setup_gestures`
paste that every interactive control carries today:

```cpp
namespace tiny::ui {

class Gesture_pack {
public:
    struct Spec {
        std::optional<Over_callbacks> over;
        std::optional<Down_callbacks> down;
        std::optional<Click_callbacks> click;
        std::optional<Dwell_callbacks> dwell;
        std::optional<Drag_callbacks> drag;
    };
    explicit Gesture_pack(Spec s);
    auto set_frame(const tiny::Frame&) -> void;
    auto process(tiny::Event_list&) -> void;
private:
    std::vector<std::unique_ptr<tiny::Gesture_recognizer>> _recognizers;
};

}
```

`Click_surface` and `Drag_surface` become thin wrappers around a single
`Gesture_pack`.

`Formatter_pack` is the env-registered bundle of formatters that
controls reach for globally today
(`Value_conv::knob_to_string`, `Host_formatter::to_string`,
`param_tooltip_for`). Controls receive it via `env::Formatters` and
cache a reference in `on_environment`. Plug-ins can swap the pack to
customize formatting; the default pack delegates to the framework's
existing `Value_conv` / `Host_formatter`.

### Basic control library

**Bundled in `tiny_ui`** (the universal vocabulary every plug-in needs):

- Primitives: `Click_surface`, `Drag_surface`, `Text`, `Label`,
  `Solid_color`, `Panel_bg`, `Scroll_view`, `Disabled_overlay`.
- Controls: `Knob`, `H_slider`, `V_slider`, `Button`, `Toggle`,
  `Peak_meter`, `LED_lamp`.

Each lives in its own header under `include/tinyplug/ui/controls/`,
each derives from `Element`, each accepts a `Spec` struct with
designated-initializer call sites. State and action are lambdas
(`std::function<bool(const State_view&)>` for booleans,
`std::function<void(const Action_context&)>` for actions).

**Deliberately out** (stays downstream as plug-in code): `Morph_button`,
`Morph_slider`, `Big_fader`, `Action_button`, `Item_cell`, `Preset_ctrl`,
`Preset_item`, `Overlay_header`. These either depend on plug-in-specific
concepts (morph mode, preset banks, A/B settings) or are presentational
chrome that's cheaper to vendor than to lift.

**Theming.** Default `Colors` palette as `env::Colors::default_value()`.
Plug-ins replace via `env.set<env::Colors>(my_palette)`. Dark-mode is a
separate `env::Dark_mode` bool; palettes can carry both variants and
resolve internally.

**Settings table** (descriptor-based, lifted from `settings_menu.hpp`):

```cpp
namespace tiny::ui {

struct Cell_spacer { double h{8}; };
struct Cell_header { std::string title; };
struct Cell_toggle { /* label, state lambda, action lambda */ };
struct Cell_cycle  { /* label, options, state, action */ };
struct Cell_action { /* label, button content, action */ };
struct Cell_text   { /* label, text resolver */ };
struct Cell_custom { std::function<View(Layout::Ui_elements)> gen; };

using Cell = std::variant<Cell_spacer, Cell_header, Cell_toggle,
                          Cell_cycle, Cell_action, Cell_text, Cell_custom>;
using Table_model = std::function<std::vector<Cell>()>;

}
```

Plug-in-specific descriptors (`License_actions_desc`, `Random_desc` from
all_plugins) stay downstream and use `Cell_custom` as the escape hatch.

### `Composer` — the editor runtime

A class the user's `Plug_editor` embeds. Owns the `Env`, the
`Layout_resolver` (which now owns the `Presentation_stack`), the
`Layout_state`, and the receiver-side of the modal/popover/tooltip
channels. Exposes hooks that mirror the framework's editor lifecycle:

```cpp
namespace tiny::ui {

class Composer {
public:
    struct Spec {
        Layout_gen root_layout;
        Layout_state::Model initial_layout_state{};
        tiny::Rect_size preferred_size{};
        std::function<void(Env&)> register_env;  // user hook
    };
    explicit Composer(Spec spec);

    auto on_show(const tiny::Edit_context& edit, tiny::Format fmt) -> void;
    auto on_hide() -> void;
    auto on_draw(tiny::Plugin_state& state) -> void;
    auto on_notify(const tiny::Ui_notification& note) -> void;

    auto env() -> Env&;
    auto resize_request() -> std::optional<tiny::Rect_size>;
};

}
```

The user's `Plug_editor` shrinks to a thin shell that holds a `Composer`
and forwards lifecycle calls. `Composer::on_notify` handles
`Dark_mode_changed` automatically (sets `env::Dark_mode`, triggers
re-layout); this stops being plug-in boilerplate.

## Critical files

**Reference, do not modify** (these inform the design):
- /Users/ryan/Developer/all_plugins/shared/sui/layout.hpp — current `View`,
  `Sizing`, `Ui_element`, `Layout`. Port mostly verbatim, rename.
- /Users/ryan/Developer/all_plugins/shared/sui/layout_resolver.hpp — current
  resolver with the three-slot model that becomes the `Presentation_stack`.
- /Users/ryan/Developer/all_plugins/shared/sui/context.hpp — the concrete
  `Environment` struct being replaced by `Env`.
- /Users/ryan/Developer/all_plugins/shared/sui/popover.hpp — `Modal_request`
  / `Popover_request` payload types and `calc_popover` heuristic to port.
- /Users/ryan/Developer/all_plugins/shared/sui/sender_receiver.hpp — the
  one-shot `Sender<T>` / `Receiver<T>` pattern to lift.
- /Users/ryan/Developer/all_plugins/shared/sui/resolvables.hpp — `Resolvable_*`
  variants to lift.
- /Users/ryan/Developer/all_plugins/shared/sui/layout_state.hpp — `Layout_state`
  key-value store to lift (widen `Value` to include `std::string`).
- /Users/ryan/Developer/all_plugins/shared/sui/ui_elements/*.{hpp,cpp} —
  control implementations to port for the bundled subset, refactor onto
  `Gesture_pack` and `Formatter_pack`.
- /Users/ryan/Developer/all_plugins/shared/sui/layouts/settings_menu.hpp —
  `Cell_desc` pattern to lift as `tiny::ui::Cell`.
- /Users/ryan/Developer/all_plugins/plugins/*/source/plug_editor.cpp —
  example integration patterns; informs the `Composer` API.

**New files to create** (under `libs/ui/include/tinyplug/ui/` and
`libs/ui/src/`):
- `view.h`, `layout.h`, `element.h`, `env.h`, `presentation.h`,
  `resolver.h`, `composer.h`, `gesture_pack.h`, `resolvable.h`,
  `theme.h`, `settings.h`, `ui.h` (umbrella).
- `controls/{click_surface,drag_surface,knob,h_slider,v_slider,button,
  toggle,peak_meter,led_lamp,text,label,panel_bg,scroll_view,
  disabled_overlay,solid_color}.h`.
- `CMakeLists.txt` declaring the `tiny_ui` static library, linking
  `tiny_core` and `tiny_platform` PUBLIC.

**Existing framework files referenced by `tiny_ui`** (no changes needed
for the framework — `tiny_ui` consumes them):
- /Users/ryan/Developer/tinyplug/shared/tinyplug/tiny_view.h — `Element`
  base must align with what `view_impl::run_frame` expects from
  `_custom_view->on_gui_draw`.
- /Users/ryan/Developer/tinyplug/shared/tinyplug/tiny_events.h — `User_action`
  emitted by controls.
- /Users/ryan/Developer/tinyplug/shared/tinyplug/gesture_recognizers.hpp —
  the underlying `Gesture_recognizer` types that `Gesture_pack` composes.
- /Users/ryan/Developer/tinyplug/shared/tinyplug/tiny_edit.h — `Edit_context`,
  source of `Action_queue::Actor`, `Undo_history::Actor`,
  `State_adapter::Actor`.

## Phasing (PR sequence)

1. **PR-1: skeleton + DSL.** Land `libs/ui/` with the `View` tree
   (`Column`/`Row`/`Box` only — defer `Z_stack`/`If`/`For`), `Sizing`,
   `Insets`, `Spacing`, `Alignment`, `Layout`, `Layout_state`,
   `Layout_context`. Keep the three-slot model in `Layout_resolver`
   temporarily. Element interface lands in `element.h`.
2. **PR-2: type-erased `Env`.** `env.h` with `Env_key`, `Env`, the
   framework-shipped key set, and `Env_modifier` view wrapper. The
   resolver's existing `propagate_environment` learns about overrides.
3. **PR-3: presentation stack.** Replace the three-slot model with
   `Presentation_stack`. Tooltips coexist with modals; modals stack
   above popovers. `Dismiss_policy` per entry. Build a small
   `presentation_demo` plug-in that exercises a popover-spawns-modal-
   spawns-tooltip scenario.
4. **PR-4: bundled controls + `Gesture_pack` + `Formatter_pack`.** Port
   `Click_surface`, `Drag_surface`, `Knob`, `H_slider`, `V_slider`,
   `Button`, `Toggle`, `Peak_meter`, `Text`, `Label`, `Panel_bg`,
   `Scroll_view`, `Disabled_overlay`, `LED_lamp`. Refactor each onto
   `Gesture_pack` and the env-injected `Formatter_pack`. Migrate
   `gain_demo` to use them for visual parity.
5. **PR-5: `Composer`.** Editor runtime that owns Env, resolver, stack,
   and the presentation receivers. Migrate `gain_demo`'s `Plug_editor`
   to a `_composer`-only shell.
6. **PR-6: settings descriptors.** Lift `Cell` variant from
   `settings_menu.hpp`. Acceptance: a settings panel in `gain_demo`
   uses three different cell kinds plus one `Cell_custom`.
7. **PR-7: DSL sugar.** `Z_stack`, `If`, `For` added to `View` variant
   per the sketches above. No semantic change to existing layouts; at
   least one demo uses each.

Downstream `all_plugins` migration is a **separate follow-up effort**
after PR-7 lands. Until then, `all_plugins` continues to use its
existing `shared/sui/` library; the new `tiny::ui` runs alongside it
inside the framework's demo plug-ins.

## Verification

**Build matrix.** Each PR keeps the framework's existing demo plug-ins
(`gain_demo`, `worker_demo`, `latency_demo`, `platform_demo`,
`automation_tester`) compiling for all five wrappers (`aax`, `auv2`,
`auv3-mac`, `auv3-ios`, `clap`, `vst3`). Build serially per the
framework's recorded no-`--parallel` rule.

**Demo-driven acceptance per PR:**
- PR-1: a stub `gain_demo` layout using only `tiny::ui::Column` builds
  and the editor opens (blank but stable) in a host.
- PR-2: `gain_demo`'s editor registers a custom env key and the layout
  reads it back through `env::*` in `Layout_context`.
- PR-3: `presentation_demo` (new) opens a settings popover anchored to
  a button; from within the popover, opens a confirmation modal sitting
  *above* the popover (both visible); hovering a control in the popover
  shows a tooltip *above* the modal. Outside-tap dismisses popover but
  not modal; escape dismisses modal; hover-out dismisses tooltip.
- PR-4: `gain_demo`'s editor renders a knob, a peak meter, a button,
  and a label using bundled `tiny::ui::controls::*`; visual diff vs.
  prior bespoke editor reviewed and approved.
- PR-5: `gain_demo`'s `Plug_editor` is under 80 LOC; bring-up still
  works in all hosts.
- PR-6: a `gain_demo` settings panel renders `Cell_header`,
  `Cell_toggle`, `Cell_cycle`, and one `Cell_custom` correctly.
- PR-7: `gain_demo` layouts use `If` to switch between panels, `For` to
  emit a knob row from a vector of param addresses, and `Z_stack` for a
  meter overlay.

**Per-host spot checks** (PR-3 onward, the presentation work is the
risky bit):
- VST3 in Bitwig + Live: presentation stack interacts with the
  controller's host-callback timing; verify no draw-thread reentrancy.
- CLAP in Bitwig + Studio One: same, plus Studio One's misbehaviour
  flag in `clap_plugin.h`.
- AUv2 in Logic: corner-drag resize handle continues to draw under the
  presentation stack but not over modals (matching current behavior).
- AUv3 in Logic (macOS) + GarageBand (iOS): tooltip dismissal on touch
  — `hover_out` is meaningless on iOS; verify `any_pointer_down`
  catches dismissal there.
- AAX in Pro Tools: same as AUv2 plus the page-table/compare-light
  interaction (out of scope but worth confirming no regression).

**Static checks.** `clang-format --dry-run --Werror` on the new files,
matching the framework's existing style (Stroustrup, snake_case types,
AAA, trailing return types, `_` prefix on private members). No PCH
expected; `tiny_ui` headers are small enough.

**Memory/lifetime.** Run one of the demos under ASan to confirm:
- No use of `Plugin_state*` cached between hooks.
- `Presentation_entry::layout` outlives any `Sender<*_request>`
  captured at push time.
- `Env_modifier` properly pops on view-tree teardown.

When verification at each PR's acceptance criteria passes and at least
two hosts per format show correct presentation-stack behavior, the
framework UI library is ready for downstream `all_plugins` migration in
a separate effort.
