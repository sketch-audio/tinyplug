# Migration guide

How to port a downstream plug-in across breaking framework changes. Each section covers one
change, newest last, and says which release it breaks from. Work through them in order.

Every change here breaks the build, so nothing changes behaviour silently. If it builds, it
is almost certainly right.

---

## Model layer

Breaks from `release/0.4`. Splits the framework into a model-free core and a header-only,
model-aware interface, and generates model discovery from CMake. The *why* is in
[plans/model-layer.md](plans/model-layer.md).

### 0. What changed, in one screen

```
BEFORE                                         AFTER
libs/tinyplug (STATIC)                         libs/tiny_core (STATIC, model-free)
                                               libs/tinyplug  (INTERFACE, model-aware)
${TINY_SHARED_LIB}                             ${TINY_PLUG_LIB}  (links ${TINY_CORE_LIB})
<tinyplug/value_helper.hpp> etc.               <tiny_core/value_helper.hpp> etc.
__has_include("worker.hpp")                    configure_models() + configure_plugin()
namespace tiny::plugin { class Editor; }       namespace tiny::edit { class Editor; }
namespace tiny::plugin { class Worker; }       namespace tiny::work { class Worker; }
Worker::Model { From_processor ... }           models::Work in source/models/work.hpp
Worker_reply_actor<Worker>                     Worker_replies
```

Seven changes, in the order to make them:

1. CMake: link `${TINY_PLUG_LIB}` and call the two generators.
2. Include paths: model-free headers moved to `<tiny_core/...>`.
3. Model headers include core only.
4. `tiny::plugin::Editor` → `tiny::edit::Editor`.
5. The worker's channel shape moves out of the class into `models/work.hpp`, and the class
   moves to `tiny::work`.
6. Drop the local `User_params` / `User_meters` aliases (optional, but they are now provided).
7. Meters are optional. Delete an empty `models/meters.hpp`.

### 1. CMake

```cmake
# BEFORE
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_SHARED_LIB})

# AFTER
configure_plug_info(${PLUGIN_TARGET} ${CMAKE_CURRENT_BINARY_DIR}/plug_info.hpp)
configure_models(${PLUGIN_TARGET}) # <tiny_models.hpp> from source/models/
configure_plugin(${PLUGIN_TARGET}) # <tiny_plugin.hpp> from source/
target_link_libraries(${PLUGIN_TARGET} PUBLIC ${TINY_PLUG_LIB})
```

`${TINY_SHARED_LIB}` / `tiny_shared_lib` no longer exist. Use `${TINY_PLUG_LIB}` (`tinyplug`)
for a plug-in, or `${TINY_CORE_LIB}` (`tiny_core`) for something that needs no models.

`configure_models` and `configure_plugin` find your headers by **file presence** under
`source/`. Pass `SOURCE_DIR <dir>` if your sources live somewhere else. Each one puts its
generated header on the target's PUBLIC include path.

| File | Required | Absent ⇒ |
|---|---|---|
| `source/models/params.hpp` | yes (for now) | configure error |
| `source/models/meters.hpp` | no | `meters::None`, `TINY_HAS_METERS 0` |
| `source/models/work.hpp` | with `worker.hpp` | `work::None`, `TINY_HAS_WORK 0` |
| `source/processor.hpp` | yes | configure error |
| `source/editor.hpp` | yes (for now) | configure error |
| `source/worker.hpp` | with `models/work.hpp` | `work::None`, `TINY_HAS_WORKER 0` |

Adding or removing one of these files re-runs configure on the next build (the discovery
globs are `CONFIGURE_DEPENDS`), so you don't need to re-run cmake by hand. **Xcode is the
exception:** building a single target with `cmake --build --target X` doesn't run
`ZERO_CHECK`. Build `ZERO_CHECK` (or re-run the preset) after adding or removing a model.

### 2. Include paths

Everything that doesn't depend on your models moved to `libs/tiny_core`. Replace the prefix:

```
tinyplug/{change_list, denormal_guard, gesture_recognizers, host_formatter,
          lifecycle_probe, lock_free_queue, meter_mailbox, meter_publisher,
          notification_queue, platform_defs, relay, serial_queue, state_adapter,
          state_rules, task_launcher, task_manager, tiny_log, tiny_meters,
          tiny_params, tiny_utils, value_helper, window_token}.hpp
   →  tiny_core/<same>.hpp
```

`<tinyplug/tinyplug.hpp>` is still the umbrella for processor, editor and worker code, and
still reaches everything. Two headers were split, and the core halves are new:

- `<tiny_core/tiny_input.hpp>`: pointer, gesture and time vocabulary, plus `View_context`,
  `Draw_context`, `Scroll_data`, `Draw_callback` and `Notify_callback`, all from
  `tiny_view.hpp`.
- `<tiny_core/tiny_notifications.hpp>`: `Set_param`, `Dark_mode_changed`,
  `Host_preset_loaded` and `Host_event`, from `tiny_events.hpp`.

If you included `tiny_view.hpp` or `tiny_events.hpp` only for those types, include the core
header instead.

### 3. Model headers include core only

`source/models/*.hpp` are included by the generated `<tiny_models.hpp>`, which the framework
includes **before** its own model-aware headers. A model that includes
`<tinyplug/tinyplug.hpp>` creates an include cycle.

```cpp
// BEFORE
#include "tinyplug/tinyplug.hpp"
// AFTER
#include <tiny_core/tiny_core.hpp>
```

Your processor, editor and worker headers keep including `<tinyplug/tinyplug.hpp>`, which now
brings in your models for you. Explicit `#include "models/params.hpp"` lines there are
harmless, but you no longer need them.

### 4. The editor moves to `tiny::edit`

```cpp
// BEFORE                               // AFTER
namespace tiny::plugin {                namespace tiny::edit {
class Editor { ... };                   class Editor { ... };
}                                       }
```

Framework types are still in `tiny`, so they resolve unqualified inside `tiny::edit` just as
they did inside `tiny::plugin`. Rename the namespace in `editor.hpp` and `editor.cpp` and
you're done.

### 5. The worker's channel shape moves out of the class

The four channel variants and the tuning constants were nested in `Worker::Model`. That put
them in a header that needs the framework, so the framework could only read them through the
"must come last" include rule. They now live in a core-only model:

```cpp
// source/models/work.hpp  (NEW)
#pragma once
#include <tiny_core/tiny_core.hpp>

namespace tiny::models {

struct Tick { int64_t sample_pos{}; };
// ... your other message types ...

struct Work {
    using From_processor = std::variant<Tick>;
    using From_editor    = std::variant<Set_session>;
    using To_processor   = std::variant<Set_counter>;
    using To_editor      = std::variant<Session_path>;

    static constexpr auto inbound_capacity  = size_t{64};
    static constexpr auto outbound_capacity = size_t{16};
    static constexpr auto update_period = std::chrono::milliseconds{16};
};

} // namespace tiny::models
```

```cpp
// source/worker.hpp
// BEFORE                                          // AFTER
namespace tiny::plugin {                           namespace tiny::work {
class Worker {                                     class Worker {
    struct Model { ... };                              // (gone — see models/work.hpp)
    using From_processor = Model::From_processor;      using From_processor = User_work::From_processor;
    explicit Worker(Worker_reply_actor<Worker>,        explicit Worker(Worker_replies,
                    Task_manager::Actor);                              Task_manager::Actor);
```

Everywhere else:

| Before | After |
|---|---|
| `plugin::Worker::Model::To_processor` | `User_work::To_processor` |
| `Worker::Model::To_editor` | `User_work::To_editor` |
| `plugin::Tick` (message types) | `models::Tick` |
| `Worker_reply_actor<Worker>` | `Worker_replies` |
| `No_worker` | `work::None` |

`Worker_processor_actor`, `Worker_editor_actor`, `bind_worker`, `handle_worker_reply` and
`on_worker_reply` are unchanged.

The two files are paired. `worker.hpp` without `models/work.hpp`, or the reverse, is a
configure error.

### 6. Aliases you get for free

The generated headers declare these in `tiny`, so you can delete your local copies:

```cpp
using User_params = params::Infos<models::Resolved::Params>;   // <tiny_models.hpp>
using User_meters = meters::Infos<models::Resolved::Meters>;
using User_work   = models::Resolved::Work;
inline constexpr bool has_params, has_meters, has_state, has_work;

using User_processor = plugin::Resolved::Processor;             // <tiny_plugin.hpp>
using User_editor    = plugin::Resolved::Editor;
using User_worker    = plugin::Resolved::Worker;
inline constexpr bool has_editor, has_worker;
```

A redeclaration as a class member (`using User_params = params::Infos<models::Params>;`) is
still legal, because it names the same type. Delete it anyway.

### 7. Meters are optional

A plug-in without `models/meters.hpp` carries no meter machinery at all: no publisher,
mailbox, output parameters (VST3) or Direct Data traffic (AAX). If your meter model declares
only `Num_meters`, delete the file and remove it from your `target_sources`.

Keeping an empty model still works, but the machinery stays compiled in.

Without a meter model, two fields don't exist:

| Field | Absent when `TINY_HAS_METERS == 0` |
|---|---|
| `process::Dsp_context::meters` | the processor has nowhere to write, and nothing to write |
| `Ui_receiver::read_meters` | wrapper-internal; only matters if you build your own `Ui_receiver` |

`Plugin_state::processor_state.meters` is still there, as an empty span, because
`Processor_state` lives in core and can't depend on your models. Code shared between
plug-ins with and without meters can guard itself with `#if TINY_HAS_METERS` or, inside a
template, `if constexpr (has_meters)`.

### Traps

- **A model header that includes `<tinyplug/tinyplug.hpp>`.** While the model only uses
  core types, this is a harmless cycle. Once it reaches for an interface type you get
  `no type named '...' in namespace 'tiny'`, because the interface isn't declared yet at that
  point. Include `<tiny_core/tiny_core.hpp>`.
- **`<tiny_plugin.hpp>` is for wrappers.** Your own code never needs it. It includes your
  `processor.hpp`, `editor.hpp` and `worker.hpp`, so including it from one of those creates a
  cycle.
- **`process::Some_plug_processor` is now asserted.** The generated `<tiny_plugin.hpp>`
  checks it. A processor that was never asserted and doesn't quite match the concept now
  fails to compile, with the diagnostic in the generated header.

### Verifying

Build every format for the plug-in. There is no runtime behaviour change. The only
generated-code difference is that `action_queue` and `undo_history` are now inlined into
each plug-in instead of linked from the static library.
