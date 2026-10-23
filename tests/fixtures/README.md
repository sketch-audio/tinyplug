# Fixtures

State saved by older builds, checked in, so a change that stops old sessions or presets loading fails a test.
Never regenerate a fixture to make a test pass: a fixture that no longer loads is the bug.

## sessions/

One session per demo and format: `<Demo>.<format>.state` holds the format's own bytes (the CLAP stream, the VST3
processor chunk with its controller chunk in `.extra`, the AUv2 ClassInfo and AUv3 fullState as binary property
lists, the AAX chunk), and `<Demo>.<format>.expect` what loading it must give. Every fake host checks the ones for
its format (`--fixtures`, set by `tests/CMakeLists.txt`):

- the parameters read back as saved, through the format's own API;
- the document record survives a load and a save, byte for byte;
- the editor opens at the saved size (CLAP and VST3; the AU and AAX wrappers persist a size only when the
  editor asks for one, which a host can't do);
- each `text` line is in the state saved after the load. StateDemo's view model maps the old session's
  `"preset-name"` (`Load_info::legacy`) into its view, so `text Fixture preset` proves an old session's editor
  state reaches the view model and is saved with the session. `text` lines are added by hand: they state what a
  load must give, the bytes stay as the old build wrote them.

StateDemo's sessions hold parameters and bypass at non-default values, the editor size, a non-default document,
and an author key in the editor map (`"preset-name"`, as the client plug-ins write).

Written by plug-ins built at `9974496` (before the unified edit model), with two edits for the run only so the
sessions look like a client plug-in's: StateDemo's `save_state` returned
`{{"preset-name", std::string{"Fixture preset"}}}`, and its default pattern was
`7, 93, 21, 64, 0, 100, 48, 12, 77, 35, 59, 3, 88, 26, 70, 41`. Today's hosts saved them from that build's
bundles (`$OLD` below is its build tree, configured like `tsan-hosts`):

```sh
F=tests/fixtures/sessions; H=build-tsan-hosts/tests; D=$OLD/examples/state_demo
$H/clap_host $D/StateDemo.clap --fixtures $F --write-fixtures
$H/vst3_host $D/StateDemo.vst3 --fixtures $F --write-fixtures
$H/auv2_host $D/StateDemo.component --fixtures $F --write-fixtures
$H/auv3_host $D/StateDemo.auv3test $D/StateDemo.component --fixtures $F --write-fixtures
```

AAX's host compiles the wrapper in, so that commit's `aax_host_StateDemo` saved the AAX session, with today's
write scenario (the same parameter values, `fixture_value`) added to it for the run.

## presets/

- `legacy.json`: a preset in the JSON layout from before the unified edit model (`params` by keypath, an author key
  and the framework's size keys under `"editor"`, the document as base64 under `"state"`). Read by
  `state_adapter_test`, which wrote it at the commit above (its writer is gone with the layout).
- `StateDemo/Round Trip.json`: a factory preset the native exporters (`tools/presets`) turn into `.vstpreset` and
  `.tfx` under the build tree (tests `export.StateDemo.*`). The VST3 and AAX hosts then load those as a DAW loads a
  factory preset (tests `vst3_preset.StateDemo`, `aax_preset.StateDemo`) and compare with
  `Round Trip.<format>.expect`, which they wrote once with `--write-fixtures`, plus a hand-added `text Round Trip`:
  the exporter's preset key makes the load a preset's, and the view model keeps its name. The preset carries the
  same non-default document as the session fixture.
