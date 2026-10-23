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
- each `text` line is in the state saved after the load. GainDemo's view model maps the old session's
  `"preset-name"` (`Load_info::legacy`) into its view, so `text Fixture preset` proves an old session's editor
  state reaches the view model and is saved with the session. `text` lines are added by hand: they state what a
  load must give, the bytes stay as the old build wrote them.

| Demo | Covers |
|---|---|
| GainDemo | parameters and bypass at non-default values, the editor size, and an author key in the editor map (`"preset-name"`, as the client plug-ins write) |
| StateDemo | a non-default document |

Generated at `9974496` (before the unified edit model) with two edits applied for the run only, so the sessions
look like a client plug-in's: GainDemo's `save_state` returned `{{"preset-name", std::string{"Fixture preset"}}}`,
and StateDemo's default pattern was `7, 93, 21, 64, 0, 100, 48, 12, 77, 35, 59, 3, 88, 26, 70, 41`. From the
repository root, with the `tsan-hosts` build:

```sh
F=tests/fixtures/sessions; H=build-tsan-hosts/tests
for d in gain_demo:GainDemo state_demo:StateDemo; do
  dir=build-tsan-hosts/examples/${d%%:*}; name=${d##*:}
  $H/clap_host $dir/$name.clap --fixtures $F --write-fixtures
  $H/vst3_host $dir/$name.vst3 --fixtures $F --write-fixtures
  $H/auv2_host $dir/$name.component --fixtures $F --write-fixtures
  $H/auv3_host $dir/$name.auv3test $dir/$name.component --fixtures $F --write-fixtures
  $H/aax_host_$name --fixtures $F --write-fixtures
done
```

## presets/

- `legacy.json`: a preset in the JSON layout from before the unified edit model (`params` by keypath, an author key
  and the framework's size keys under `"editor"`, the document as base64 under `"state"`). Read by
  `state_adapter_test`, which wrote it at the commit above (its writer is gone with the layout).
- `<Demo>/Round Trip.json`: a factory preset the native exporters (`tools/presets`) turn into `.vstpreset` and
  `.tfx` under the build tree (tests `export.<Demo>.*`). The VST3 and AAX hosts then load those as a DAW loads a
  factory preset (tests `vst3_preset.<Demo>`, `aax_preset.<Demo>`) and compare with `Round Trip.<format>.expect`,
  which they wrote once with `--write-fixtures`, plus a hand-added `text Round Trip` for GainDemo: the exporter's
  preset key makes the load a preset's, and the view model keeps its name. StateDemo's preset carries the same
  non-default document as its session fixture.
