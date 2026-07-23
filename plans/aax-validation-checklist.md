# AAX two-component: validation & Pro Tools test checklist

> Operationalizes §14 phase 6 and §15 of [aax-two-component.md](aax-two-component.md).
> Goal: prove the rewritten two-component AAX wrapper (commits `96c26c8`,
> `4e409f5`) is at parity with the old monolith on **core framework features**,
> using the six demo plug-ins.
>
> **Constraint: this Pro Tools is a developer/dev-signed setup — it will run
> non-PACE-signed plug-ins but WILL NOT SAVE.** Every test that depends on
> persistence (session recall, preset write, compare, undo-of-load) is therefore
> **deferred** and marked ⏸ below. Do those on a PACE-signed build later. Focus
> this pass on the ▶ items.

---

## 0. Setup (once)

- [ ] Confirm the six bundles exist and are fresh:
      `ls -la build-debug/examples/*/*.aaxplugin`
- [ ] Install them so Pro Tools can scan them. Either build with
      `-DTINY_INSTALL_PLUGINS=ON` or copy by hand:
      ```
      sudo cp -R build-debug/examples/<demo>/<Name>.aaxplugin \
        "/Library/Application Support/Avid/Audio/Plug-Ins/"
      ```
      (Debug bundles are unsigned; the dev Pro Tools must be configured to load
      unsigned/developer plug-ins — DigiSignServer / developer entitlement.)
- [ ] Strip quarantine if the bundles get flagged:
      `xattr -dr com.apple.quarantine "<bundle>"`
- [ ] Note the AAX SDK version PT reports vs. what we built against, in case of
      descriptor-era mismatches.

**Deferred-feature ledger (⏸ — cannot test without saving):**
- Session save → reopen → parameter recall (`GetChunk`/`SetChunk` round-trip).
- Editor `State_map` recall (window size, editor keys).
- Preset (`.tfx`) write; user-preset recall.
- Pro Tools **Compare** light (`CompareActiveChunk`) — needs a saved settings baseline.
- **Undo of a host preset/state load** (`push_host_load`) and the
  `Host_preset_loaded` `notify` — needs a preset to load.
- Restoring `host_bypass` state across a reload.

Everything else below is testable live in one session.

---

## 1. AAX Validator (automated — do this FIRST, before Pro Tools)

> **Status: RUN 2026-07-23 — PASS on all six demos.** Full `runtests` suite via
> `dsh`/`aaxval` (validator v2024.6.0). Every applicable test passed for all six
> bundles. The **only** failure is `test.page_table.load` ("No page tables are
> registered for this effect") on every demo — **expected**, the demos ship no AAX
> page table (opt-in). The two-component-critical tests all pass:
> `test.describe_validation`, `test.data_model` (instantiate/de-instantiate across
> all host contexts — incl. both stereo *and* mono effects for GainDemo), `test.parameters`
> (Gain 2048 steps + the packed **Bypass** pseudo-param 2 steps), all three
> `test.parameter_traversal.*` (100% coverage), `test.load_unload` (1000×), and
> `test.page_table.automation_list`. No crashes, exceptions, or asserts.
> One-time gotcha: the validator tree ships quarantined — strip it fully
> (`find … -xattrname com.apple.quarantine | xargs xattr -d …`, incl. symlinks with
> `-s`) or `load_dish aaxval` fails with OSErr -23. Logs in the session scratchpad.
> *Follow-up:* ship a page table for at least one demo to exercise the page-table
> tests, or accept that failure as N/A for demos.
>


The validator (`../aax-validator`) catches structural/Describe bugs the
two-component rewrite is most likely to have introduced (unregistered context
slots, port counts, data-model reliability) far faster than clicking in a host.
PACE must be installed/running for the toolkit.

```
cd /Users/ryan/Developer/aax-validator
xattr -dr com.apple.quarantine .        # if needed
./CommandLineTools/dsh
dsh> load_dish aaxval
dsh> runtests "/absolute/path/to/build-debug/examples"      # all demos, all tests
# or per-plugin, per-collection:
dsh> runtests {coll: col_tests_config, path ".../GainDemo.aaxplugin"}
dsh> runtest  {test: test.data_model, path ".../WorkerDemo.aaxplugin", stringformat: yaml}
```

Watch for, specifically tied to this rewrite:
- [ ] **Describe/component tests pass** — every pointer slot in `Alg_context` is
      registered (unregistered slots → "context corruption"; filler
      `AddPrivateData` must cover the gaps). §15 Q6.
- [ ] **Buffered data-port count** flagged? Native has no documented cap; confirm
      no warning at our demo counts (all ≤1 segment). §3.
- [ ] **Data-model tests pass** (chunk get/compare structural checks — these run
      without a host *saving*, they exercise the API directly).
- [ ] **Meter / latency descriptor tests pass.**
- [ ] Run the whole suite per demo and diff results against expectations; save the
      logs alongside this file.

If the validator is clean, the risk going into Pro Tools is mostly runtime/timing,
not structural.

---

## 2. Pro Tools — core matrix (▶ testable now)

Do these on a stereo audio track unless noted. Have a known input (loop a file, or
a tone) so audio changes are obvious. Insert on an aux/audio track, not master, so
PDC is observable.

### 2.1 Instantiation & teardown — every demo
- [ ] Each of the six inserts without error and shows its editor.
- [ ] Remove the insert; re-insert; repeat ~5×. No crash, no leak-driven slowdown.
      (§15 Q2: `RemovingInstance` must run so `Alg_state`/user `Processor` dtor
      fires. Watch memory across repeated insert/remove — a steady climb means the
      teardown callback isn't firing.)
- [ ] Open/close the editor window repeatedly — data model & editor live at wrapper
      lifetime, so this must be independent of the algorithm.

### 2.2 Audio passthrough & the one core param — **GainDemo**
- [ ] Audio passes. Move **Gain**; level tracks. Default value correct on insert
      (§15 Q1: `config` packet must be populated when `InstanceInit` runs — if the
      first block is silent or wrong until you touch a control, that's the Q1
      fallback path being needed).
- [ ] **Master bypass** (Pro Tools plug-in bypass button): audio goes dry. This is
      the pseudo-parameter at `bypass_address == num_params` packed into a coef
      segment — it's the two-component design's most novel bypass path, test it on
      **every** demo.
- [ ] **Mono instance:** insert GainDemo on a **mono** track. It's the only demo
      with `can_process_mono`, so this exercises the second algorithm component
      (`plugin_id + 1`) from `describe.cpp`. Audio passes, gain works, bypass works.
- [ ] **Multi-mono:** if available, instantiate across a multi-mono chain.

### 2.3 Parameter application & automation timing — **AutomationTester**
This demo outputs **DC equal to the Gain value**, so the output sample value *is*
the parameter — ideal for verifying coefficient-segment delivery and timing.
- [ ] Set Gain to a few values; confirm output DC matches (meter or a scope).
- [ ] **Write automation** on Gain across a pass; play back. Output DC should follow
      the automation curve. The two-component path gives **32-sample** automation
      accuracy (host splits buffers to land coef packets at breakpoints) — steps
      should land tight to the breakpoints, not quantized to full buffers.
- [ ] Automation **read** mode: the on-screen control follows the lane.
- [ ] Touch/latch: grab the knob (Action_start) → move → release (Action_end);
      confirm write arms/disarms correctly (gesture → begin/edit/end).
- [ ] Rapid automation + varying playback buffer size (Playback Engine H/W buffer):
      no zipper/glitch beyond the ramp, no stuck values.

### 2.4 Latency & PDC — **LatencyDemo**
Exercises the full latency handshake over the new Direct Data return channel.
- [ ] Toggle **Latency** param Low↔High (it's `Policy::Control`, no automation).
- [ ] Pro Tools' **delay compensation** display for the track updates to the new
      sample count. Confirm the reported latency matches Low vs High.
- [ ] The **Latency_actual** meter in the editor (Stream) flips 0↔1 in lockstep,
      confirming the algorithm actually switched paths *after* the host accepted
      (i.e. `Accepted_latency` arrived via `Runtime_packet`, not before).
- [ ] Play material through a summing bus with a dry reference track; toggling
      latency should keep phase-aligned once PDC settles (the handshake worked).
- [ ] Watch for the "host accepted zero" trap (§ CLAUDE.md): a fresh insert must
      not spuriously apply latency 0 from a zero-inited packet — sequence gates it.

### 2.5 Offline / realtime render — **RenderModeDemo**
- [ ] Realtime playback: sine + faint noise (noise present).
- [ ] **Offline bounce** (Bounce to Disk / "offline"): the rendered file should have
      the noise **dropped** (offline path), sine only. This is `context.render_mode
      == Offline` reaching the algorithm.
      > Confirm the dev Pro Tools *permits bounce-to-disk* (it writes an audio file,
      > not a session/preset — expected to be allowed, but verify; if blocked, this
      > moves to ⏸).
- [ ] The **Offline** meter (Stream) reads 1 during offline render, 0 otherwise —
      confirms render-mode reporting reaches the editor.
- [ ] **AudioSuite** (RenderModeDemo as an AudioSuite process, if the demo is
      exposed there): offline path taken, output correct.

### 2.6 Worker channel — **WorkerDemo**
Exercises both worker legs; the processor→worker leg is the one that now crosses
the Direct Data ring in the two-component design.
- [ ] With audio playing, the processor pushes `Tick`s → worker replies
      `Set_counter` → processor. Confirm the round-trip runs without audio-thread
      stalls (the ring is drained on the ~30 ms Direct Data timer).
- [ ] Editor→worker leg: whatever UI action sends `Set_session` gets a
      `Session_path` reply reflected in the editor. (This leg is direct, unchanged.)
- [ ] Leave it running several minutes under playback: no dropped or stuck worker
      state, no assert (worker overflow is an assert, not a drop, per CLAUDE.md).
- [ ] Move **Gain**; audio still correct alongside worker traffic.

### 2.7 Editor / GUI / platform — **PlatformDemo**
- [ ] Editor renders (Skia/Metal), 800×600, redraws smoothly at frame rate.
- [ ] Gesture recognizer / click behavior works.
- [ ] **Resize** request from the editor is honored by the host (Request_resize).
- [ ] **Dark mode:** toggle macOS appearance; the `Dark_mode_changed` `Host_event`
      reaches `Editor::notify` and the UI reflects it (this path is independent of
      saving).
- [ ] Editor open while transport runs — no contention with the algorithm.

### 2.8 Meter smoothness (cross-cutting, §15 Q3)
- [ ] On LatencyDemo / RenderModeDemo Stream meters and any Peak meters: updates
      should look smooth (~33 Hz from the Direct Data timer). Under a **heavy
      session** (many tracks/plug-ins, high CPU), watch for the timer being held
      off — meters going choppy or freezing is the Q3 risk. Note the observed
      behavior; it's expected-acceptable but should be recorded.

---

## 3. Deferred — persistence pass (⏸ — needs a PACE-signed build that can save)

Run these later on a signed build. Listed here so nothing is forgotten:
- [ ] Session save → close → reopen: all param values recall; editor state (window
      size, editor keys) recall.
- [ ] Preset: save `.tfx`, reload it, values + editor markers restore together
      (the `amend_host_load` coalesced undo step).
- [ ] **Undo** immediately after a preset/state load reverts as one step, with the
      editor window both open and closed (undo history lives at wrapper lifetime).
- [ ] `Host_preset_loaded` `notify` fires once per load, editor open or closed.
- [ ] **Compare** light: enable, tweak a param, confirm the light indicates a diff
      vs saved settings (`CompareActiveChunk` — params-only today).
- [ ] Host bypass state survives save/reload.
- [ ] Chunk sentinel mismatch handling (load a preset from a different plug-in code
      → should reject/assert, not corrupt).

---

## 4. Sign-off

- [ ] Validator: all demos, all tests — clean (logs attached).
- [ ] Pro Tools ▶ matrix: all six demos pass §2.
- [ ] §15 empirical questions answered and recorded: Q1 (config@InstanceInit),
      Q2 (RemovingInstance/leaks), Q3 (Direct Data wakeup rate under load),
      Q6 (validator port-count warnings).
- [ ] Update [aax-two-component.md](aax-two-component.md) status table: phase 6
      from **outstanding** → done-for-core, with the ⏸ persistence pass noted as
      the remaining gate.
