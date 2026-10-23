# Plan: Port all_plugins to main (0.5)

> Status: **next.** The products in `../all_plugins` build against `release/0.4`. Bringing them up on main is what
> makes it 0.5. Collects what the landed plans left for the clients: step 7 of the unified edit model (its
> framework steps 0–6 are in the tree, its rules in CLAUDE.md) and step 6 of
> [bounce-param-authority.md](bounce-param-authority.md). Products: Doctor Vibe, Fuzz Droid, Galaxy Brain, Hyper
> Boost, Scissor Hands; the nuggets after. Port one product end to end (Doctor Vibe) before the rest.

## Before starting

- **Confirm the client defects in a real host** (below), by hand in Logic or Live with Doctor Vibe on 0.4, so the
  port can be checked against them.

## Processor side

- **`Reset`**: 129 `Reset::` sites in 63 files, most behind the shared `Effect_adapter`. `Hard` and `Soft` become
  `Restart{params}`; see MIGRATION.md. `Musical_context` users (`Host_timeline` and others) move to `Timeline`.
- **`make_contract_checks`** for every product. Galaxy Brain's carrier and Doctor Vibe's unidentified state should
  show there.

## Editor side: features rebuilt on the edit tools

The five products share one editor; their `notify`, `save_state` and `load_state` are identical, and go.

| Feature | 0.4 | On main |
|---|---|---|
| Preset name | `glb_preset_index` counter + `Preset_history`, `add_param` in `notify` | `preset_name` and `preset_values` in `View::Undoable`, set by `on_host_load` and inside the browser's transaction. Delete `Preset_history` (and its observer), `Value_restorer`, the counter logic and `add_param` |
| Modified `*` | action-queue observer | derived each frame from `preset_values` against the parameters |
| Restore to preset | `Value_restorer`'s 256 copies | an ordinary gesture to `preset_values[addr]` |
| A/B | `glb_a_settings` + `Ab_settings` members | slots and selection in `View::Undoable`, swaps in a transaction. Delete `Ab_settings::start/end_transaction` |
| Linked controls | copied start/end + `Action_queue::sort()` | append only the follower's `Set_param` (the copied form still groups correctly) |
| Layout tabs and panels | `Layout_model`, editor lifetime | `View::Restored` |
| Window size | wrapper `_last_size`, guessed user size, 200×150 floor | `sizing()`; the layout engine keeps its own landscape/portrait choice |

- **Legacy sessions**: `on_host_load` maps `Load_info::legacy`'s `"preset-name"`, and seeds the other A/B slot from
  the loaded parameters for sessions without a view record.
- **Leave `glb_preset_index` and `glb_a_settings` as `Interface`, unused.** Don't retire them with `Hidden`:
  `Hidden` persists, so they would start saving meaningless values.
- Factory JSON gains `"name"` (or relies on the file name); `editor.preset-name` stays readable.
- Stays where it is: `Local_prefs`, `Morph_data`, `glb_input` (`Interface`, deliberately unsaved), the `mph_*`
  morph targets (`Hidden`).

**Defects the port should fix** (found by reading 0.4, not reproduced in a host):

1. The preset name is lost on project reload: `notify` runs before the GUI environment exists and returns early.
2. A save after undo writes the last captured name, not the name at the current index.
3. `*` is unreliable: after a reload it's captured from already-modified values; window-closed and automation
   edits are never counted.
4. A/B doesn't persist, and copy to the other slot isn't undoable.
5. Layout tabs don't persist across project reloads.
6. Deep undo can show the wrong name: the counter wraps after 256 loads.

## Sketches

Client code, not framework code.

```cpp
// Preset browser: params and document (framework), name and values (client), one step.
edit.undo_redo.transaction([&] {
    edit.apply_preset(json);
    edit.view.edit([name, values = values_of(json)](View& v) {
        v.undoable.preset_name = name;
        v.undoable.preset_values = values;
    });
});

// The modified marker: derived each frame, nothing observed.
const auto& u = edit.view.get();
const auto modified = std::ranges::any_of(persistent_addrs, [&](auto a) {
    return std::abs(params[a] - u.preset_values[a]) > 1e-6;
});

// Host load of a preset or session, window open or closed.
static auto on_host_load(View& v, std::span<const double> params, const Load_info& info) -> void
{
    if (info.kind == Load_info::Kind::Preset) {
        v.undoable.preset_name = info.name;
        v.undoable.preset_values = to_array(params);
    }
    else if (info.legacy) {
        v.undoable.preset_name = legacy_name(*info.legacy);
        v.undoable.preset_values = to_array(params);
        v.undoable.other_slot = to_array(params);
    }
}

// A/B select: params plus view, one step.
edit.undo_redo.transaction([&] {
    const auto incoming = edit.view.get().other_slot;
    for (auto a : ab_addrs) edit.actions.push(Set_param{a, incoming[a]});
    edit.view.edit([b, outgoing = to_array(params)](View& v) {
        v.undoable.b_selected = b;
        v.undoable.other_slot = outgoing;
    });
});

// Auto gain: the client's rule; appending is enough.
for (const auto& action : edit.actions.get_actions()) {
    if (const auto* set = std::get_if<Set_param>(&action); set && is_lead(set->address)) {
        edit.actions.push(Set_param{follower(set->address), compensated(*set, state)});
    }
}
```

## Then

What the port shows belongs upstream decides the editor-side work (text, assets, keyboard) for after 0.5.
