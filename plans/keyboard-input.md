# Plan: Keyboard input — native text fields, then best-effort shortcuts, never stolen keys

> Status: **part 1 in progress on `keyboard-text-fields` (2026-10-23).** `Platform_text`
> ([platform_text.hpp](../libs/tiny_platform/include/tiny_platform/platform_text.hpp)) is in on macOS (an
> `NSTextField` over the view, in `mac_view.mm` with per-plug-in class names) and web (an `<input>` over the canvas,
> `web_view.cpp`), both in the editor's embedded font, registered nowhere (Windows: `AddFontMemResourceEx`, private to
> the process, matched by the family name read from the font's `name` table).
> Windows has a subclassed `EDIT` in a container child (win_view.cpp), built and checked on Windows; iOS a
> `UITextField` (ios_view.mm): a tap elsewhere in the editor ends it, a hardware keyboard's Escape cancels, and it
> floats above the on-screen keyboard while that covers it (checked on a device in an AUv3 host, 2026-10-23). text_demo types
> slider values (right-click) and its specimen; platform_demo has INLINE > GAIN and INLINE NOTE. Checked in headless
> Chrome (commit, Escape, clamping, the field drawn in Inter); macOS built and installed, not yet tried in hosts.
> Remaining: a Windows build and check, the host matrix, part 2.
>
> Before: no platform view delivered a key: `mac_view.mm` has no `keyDown:`, `win_view.cpp` no `WM_KEYDOWN` /
> `WM_CHAR`, `web_view.cpp` and `editor.js` no key listeners, and `tiny_input.hpp` no key event. Modifiers work
> everywhere because they're polled each frame (`[NSEvent modifierFlags]`, `GetKeyState`), not delivered. Text entry
> was `Platform_dialogs::text_input`, a modal dialog.

## Principles

- **Work within the format and the OS.** No keyboard hooks (`SetWindowsHookEx`), no event monitors
  (`addLocalMonitorForEvents`), no subclassing a window we don't own. JUCE and others do; we don't.
- **The host keeps every key we don't claim.** Space still plays.
- **Focus follows user intent.** The view takes it on a click that needs it and gives it back when done.
- **Where the host won't deliver, degrade to what's reliable** (the modal dialog) rather than steal.

## The landscape

| | Key API | What decides delivery |
|---|---|---|
| VST3 | `IPlugView::onKeyDown / onKeyUp` | The host. Many never call them; on macOS most let the view be first responder instead |
| AUv2, AUv3, CLAP | none | The OS: first responder (macOS), focus (Windows) |
| AAX | none | Pro Tools keeps many keys for itself on both platforms. The riskiest host |
| macOS | responder chain | Our `NSView` gets keys while it's first responder in the key window; unhandled keys go up the chain to the host |
| Windows | focus + the host's message loop | A child `HWND` gets keys only with focus, and only if the host's loop doesn't eat them first (`TranslateAccelerator`, `IsDialogMessage`). A modal dialog runs its own loop, which is why `text_input` is reliable there |
| iOS | `UIResponder` | Text fields bring up the software keyboard; hardware keyboards deliver to the first responder |
| Web | DOM | Trivial: a focusable canvas, or an `<input>` |

## Part 1 — Inline text fields

A real native text field placed over a control while it's edited: `NSTextField`, Win32 `EDIT`, `UITextField`,
`<input>`. The OS handles focus, IME, dead keys, clipboard, selection, accessibility and the software keyboard;
hosts honour a focused native field because it's the standard mechanism. It covers typed values, preset names
and search, without a general key model.

```cpp
struct Text_field {
    Frame frame{};               // logical, in the view
    std::string initial{};
    text::Style style{};         // the editor's font, see fonts-and-text.md
    SkColor ink{}, background{};
    Align align{Align::Center};
    bool select_all{true};
    enum class Commit { Enter_or_blur, Enter_only } commit{};
};

struct Text_result {
    enum class Kind { Committed, Cancelled } kind{};
    std::string text{};
};

// Main thread. One field per view at a time; a second call commits the first. `on_done` fires exactly once,
// like the dialogs, cancelled if the view closes or the field couldn't be shown.
static auto Platform_text::edit(const Text_field&, std::function<void(Text_result)> on_done, Dialog_context) -> void;
```

The window comes from `Dialog_context::window` as for dialogs. A typed value goes back through
`Host_formatter::to_value`, so the field parses what the host displays.

**The field uses the editor's font, still without OS registration**: `CTFontManagerCreateFontDescriptorFromData`
(macOS, iOS), `AddFontMemResourceEx` (Windows: private to the process, not to us: acceptable for one field),
`FontFace` from an `ArrayBuffer` (web).

Per platform:
- **macOS**: an `NSTextField` subview of the tiny view; remember the window's first responder, `makeFirstResponder:`
  the field, end on `controlTextDidEndEditing:` and `doCommandBySelector:` (`insertNewline:`,
  `cancelOperation:`), then restore the responder and remove the field.
- **Windows**: a child `EDIT` on our `HWND`, `SetFocus`; subclass our own edit (not the host's window) for
  `VK_RETURN`, `VK_ESCAPE` and `WM_KILLFOCUS`, and answer `WM_GETDLGCODE` with `DLGC_WANTALLKEYS` so a host's
  dialog handling doesn't take Enter and Tab. Focus goes back to the previous `HWND` after.
- **iOS**: a `UITextField`, `becomeFirstResponder`; keep the field visible above the software keyboard
  (`UIKeyboardWillShowNotification` → offset the editor or float the field).
- **Web**: an absolutely positioned `<input>` over the canvas from `editor.js`, removed on commit.

**Fallback.** Where a host eats keys before a focused child field (to be found by the matrix below; Windows hosts
that translate accelerators are the suspects), `Platform_text::edit` uses `text_input`'s modal dialog for that
host. Chosen per host by name (VST3 `IHostApplication`, CLAP `clap_host::name`, the AU host bundle id), on
evidence only. A list of known hosts is the one host-specific concession; it only ever picks between two
legitimate paths.

## Part 2 — Key events, best-effort

For shortcuts while the plug-in window is focused: arrows to nudge a value, Delete on a selected item, Enter to
confirm, the plug-in's own undo where the host leaves `⌘Z` alone.

**The synchronous problem.** The platform must answer "handled?" inside `keyDown:` / `WM_KEYDOWN` / `onKeyDown`
so an unhandled key can go back to the host, but an immediate-mode editor only sees events next frame. So the
editor **declares claims ahead of time**, and the platform answers from the last frame's claims:

```cpp
struct Key_claims {
    enum class Mode { None, Shortcuts, All } mode{Mode::None};  // All: a custom-drawn field has focus
    std::span<const Shortcut> shortcuts{};                      // key + required modifiers
};
// The editor sets it each frame through its context; the view session copies it to the platform view.
```

Claims change on user actions (a click selects something), which arrive before the keys that follow, so a
frame's lag doesn't show.

**Events.** `Key_down{key, text, modifiers, repeat}` and `Key_up{key, modifiers}` in a `keys` list on
`User_interaction`, beside pointer events. `key` is a framework enum (letters, digits, arrows, Enter, Escape,
Tab, Delete, Backspace, Home/End, PageUp/Down, F1-F12), mapped from virtual key codes per platform; `text` is the
UTF-8 the key produced, for display only. Custom-drawn text editing (composition, IME) stays out of scope: part 1
covers text.

Delivery:
- **macOS**: `acceptsFirstResponder` is YES only while claims aren't `None`; a click while claimed makes the view
  first responder. A claimed key is queued and consumed; anything else is `[super keyDown:]`, up the chain to the
  host. `flagsChanged:` stays unused (modifiers are polled).
- **Windows**: `SetFocus` on a click only while claimed. Unclaimed keys go to the host as `DefWindowProc` would
  leave them, forwarded to the parent `HWND` (`SendMessage`), not swallowed.
- **VST3**: `onKeyDown / onKeyUp` feed the same path and return `kResultTrue` only for a claimed key; a host that
  calls them gets the same behaviour as one that delivers through the OS.
- **iOS**: `pressesBegan:` for hardware keyboards, under the same claims.
- **Web**: `keydown` / `keyup` on the canvas (`tabindex`), `preventDefault` only for claimed keys.

## Verification

Fake hosts can't test focus, so this is a by-hand matrix, kept in this plan as it fills in: inline field and
shortcuts in Logic, Live, Reaper, Cubase, Bitwig, Pro Tools, FL Studio and Studio One, macOS and Windows,
VST3 / AU / CLAP / AAX; Enter, Escape, Tab, a dead key (`´` `e`), IME (Japanese) and paste. Unit tests cover the
key mapping and the claims answer.

## Order of work

1. ~~Part 1 on macOS and web~~, in the demos. A value-entry control in `tiny_ui` is left out: tiny_ui is header-only
   over tinyplug and doesn't link `tiny_platform`; the demos show the pattern (a right-click opens a field, the
   result goes through `Host_formatter::to_value` as one gesture).
2. The matrix for part 1 on macOS; the per-host fallback only for hosts that fail.
3. ~~Part 1 on Windows (`EDIT`) and iOS (`UITextField`)~~. Both work on their devices. A transparent background
   can't show our pixels through a native Win32 control: Windows paints the RGB opaque.
4. Part 2, macOS first, then VST3 `onKeyDown`, Windows, web, iOS.

## Learned building part 1

- **The web processor module includes the editor's header** (through `<tiny_plugin.hpp>`) without linking
  `tiny_platform`: an editor includes `platform_text.hpp` in its `.cpp`, as platform_demo always did for dialogs.
- **`??=` and friends are trigraphs** to the C preprocessor inside `EM_JS`; write the JS without them.
- **`Host_formatter::to_value` returns plain**, while `to_string` takes host. Easy to get wrong (text_demo did,
  first): convert with `plain_to_knob`.
- **Web's `text_input` never answers a cancel** ([web_dialogs.cpp](../libs/tiny_platform/source/web_dialogs.cpp)),
  against the header's contract (an empty string on cancel). Not fixed here.
- **A double-click to type moves the slider first** (the first press sets the value), so Escape couldn't restore it;
  the demos use right-click / Ctrl-click, which tiny_ui's controls ignore.
- **iOS logs `-[RTIInputSystemClient remoteTextInputSessionWithID:performInputOperation:] … requires a valid
  sessionID … UIEmojiSearchOperations`** when the field takes the keyboard in an extension. The system's own text
  input (the keyboard's emoji search), harmless, and nothing of ours to silence.
- **A long press on iOS is a right click** (an earlier decision), but its touch-down arrives first, so a
  click-to-set slider like tiny_ui's jumps before the field opens. Incremental sliders (set only by dragging) don't.
- **GDI doesn't know a variable font by its name table's family.** It derives a family per named instance from STAT,
  keeping axis values that aren't elidable ("Inter Variable Text Medium"), with Bold folded into the base family at
  700; asking for "Inter Variable Medium" got Arial. DirectWrite's in-memory loader reports each instance's Win32
  family and weight (`gdi_faces`), and the field takes the instance nearest the weight, checked with `GetTextFaceW`.
  So on Windows a field's weight snaps to a named instance, and a font with optical-size instances at one weight
  would need a size tie-break (Inter has only opsz 14 instances).
- **On web, `focus()` from the frame loop** works on desktop; iOS Safari only raises its keyboard from a user gesture,
  so a field opened a frame later may not show it there. Unverified.

## Open questions

- **Commit on focus loss, or cancel?** macOS convention commits; a lean toward commit, with `Enter_only` for
  fields where a stray click shouldn't apply a change.
- **Should claims include a keyboard focus ring** the framework draws, or is that the editor's? Lean: the
  editor's; the framework only reports focus.
- **Plug-in undo shortcuts.** Most hosts take `⌘Z` / `Ctrl+Z` for their own undo. Claiming them is the client's
  choice, and may surprise users; no framework default.

## Rejected

- **Hooks and event monitors** (JUCE's Windows message hook, local NSEvent monitors): they take keys the host
  and the user meant for something else, and break across host updates.
- **Custom-drawn text fields first**: IME, dead keys and accessibility mean reimplementing `NSTextInputClient`
  and TSF; a native field gets them free.
