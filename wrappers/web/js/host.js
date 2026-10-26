// @tinyplug/web: the page's host for a tinyplug plug-in. It is the DAW: it owns the AudioContext, the plug-in's
// node and its transport, and gives the page a host API.
//   load_plugin: the full format. The controller module on this thread (param authority, editor, undo, state,
//                worker), the processor module in the worklet, the pump between them on its own worker.
//   load:        processor-only. The page edits plain values and reads meters straight from the processor's rings.
// A build of the nonisolated variant (manifest.json's "variant") runs on any page: the pump becomes link.js's two
// halves over the worklet's port. The isolated variant needs a cross-origin isolated page (COOP / COEP).
import { Ring, each_entry } from "./wire.js";
import { create_memory, encode, decode } from "./processor.js";
import { load_controller } from "./controller.js";
import { attach_editor } from "./editor.js";
import { controller_end, pack } from "./link.js";

const here = new URL(".", import.meta.url);

const page_isolated = () => globalThis.crossOriginIsolated ?? typeof SharedArrayBuffer !== "undefined";
const read_manifest = (url) => fetch(url).then((r) => (r.ok ? r.json() : null)).catch(() => null);

// Linked unless the build is the isolated variant; which needs an isolated page, said plainly rather than as a
// missing SharedArrayBuffer.
const linked_for = (manifest) => {
    if (manifest?.variant === "nonisolated") return true;
    if (!page_isolated()) {
        throw new Error(`${manifest?.name ?? "This plug-in"} is a build of the isolated web variant, which needs a cross-origin isolated page: `
            + "serve it with Cross-Origin-Opener-Policy: same-origin and Cross-Origin-Embedder-Policy: require-corp, "
            + "or use a build of the nonisolated variant (TINY_WEB_VARIANT=nonisolated).");
    }
    return false;
};

// The worklet node. Isolated, its memory is created here and shared with the worklet; linked, the worklet makes its
// own, and `inbox.handler` receives what its end of the link posts (any that arrive first wait in `inbox.early`).
const start_processor = async (context, processor_url, params, linked) => {
    const module = await WebAssembly.compileStreaming(fetch(processor_url));
    const memory = linked ? null : create_memory();
    await context.audioWorklet.addModule(new URL("worklet.js", here));
    let ready;
    const ready_promise = new Promise((resolve) => { ready = resolve; });
    const node = new AudioWorkletNode(context, "tinyplug", {
        numberOfInputs: 2, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { module, memory, params, linked },
    });
    const inbox = { handler: null, early: [] };
    node.port.onmessage = ({ data }) => {
        if (data.log) console.log(`[tinyplug] ${data.log}`);
        if (data.ready) ready(data);
        if (data.link) inbox.handler ? inbox.handler(data.link) : inbox.early.push(data.link);
    };
    console.debug("[tinyplug] load: waiting for the worklet's processor");
    const { manifest, table } = await ready_promise; // The rings' offsets come from the processor's own table.
    const receive = (handler) => { inbox.handler = handler; inbox.early.splice(0).forEach(handler); };
    return { node, memory, manifest, table, receive };
};

export const load = async ({ processor_url, context = new AudioContext(), params = undefined }) => {
    const linked = linked_for(await read_manifest(new URL("manifest.json", new URL(processor_url, location.href))));
    const { node, memory, manifest, table, receive } = await start_processor(context, processor_url, params, linked);
    const wire = linked
        ? {
            send: (entry) => node.port.postMessage({ link: { ring: pack([entry]), stores: [] } }),
            receive: (fn) => receive((msg) => { if (msg.ring) each_entry(msg.ring, fn); }),
            poll: () => {},
        }
        : (() => {
            const inbound = new Ring(memory.buffer, table.inbound, table.inbound_capacity);
            const outbound = new Ring(memory.buffer, table.outbound, table.outbound_capacity);
            let fn = () => {};
            return { send: ([kind, payload]) => inbound.push(kind, payload), receive: (f) => { fn = f; }, poll: () => outbound.drain(fn) };
        })();
    return make_host({ context, node, manifest, wire });
};

const make_host = ({ context, node, manifest, wire }) => {
    const by_id = new Map(manifest.params.map((p) => [p.identifier, p]));
    const address = (id) => (typeof id === "number" ? id : by_id.get(id)?.address);
    const listeners = { meters: [], latency: [] };
    const meters = new Float32Array(manifest.num_meters);
    let latency = 0;
    let changed = false;

    wire.receive((kind, bytes) => {
        const m = decode(kind, bytes);
        if (m.kind === "meter" && m.address < meters.length) { meters[m.address] = m.value; changed = true; }
        if (m.kind === "latency") { latency = m.samples; listeners.latency.forEach((f) => f(latency)); }
    });

    // Once a frame: meters coalesce to their latest value, as the native editors' mailboxes do.
    const poll = () => {
        wire.poll();
        if (changed) listeners.meters.forEach((f) => f(meters));
        changed = false;
        requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);

    return {
        context, node, manifest,
        get latency() { return latency; },
        setParam: (id, plain) => wire.send(encode.set(address(id), plain)),
        rampParam: (id, plain, samples) => wire.send(encode.ramp(address(id), plain, samples)),
        setBypass: (on) => wire.send(encode.bypass(on)),
        reset: () => wire.send(encode.host_reset()),
        transport: (t) => wire.send(encode.transport(t)),
        onMeters: (f) => listeners.meters.push(f),
        onLatency: (f) => listeners.latency.push(f),
    };
};

// MARK: - the full format

// Isolated: the pump on its own worker, holding both memories.
const start_pump = async (memory, table, ctl) => {
    const stop = new Int32Array(new SharedArrayBuffer(4));
    const pump = new Worker(new URL("pump_worker.js", here), { type: "module" });
    await new Promise((resolve) => {
        pump.onmessage = ({ data }) => { if (data.started) resolve(); };
        pump.postMessage({ spec: { processor: { buffer: memory.buffer, table }, controller: { buffer: ctl.buffer(), table: ctl.table } }, stop });
    });
    return { send: () => {}, stop: () => Atomics.store(stop, 0, 1) };
};

// Linked: the controller's end of link.js. It sends at the page's pace and whenever the worklet's end posts, so a
// hidden tab (no frames, throttled timers) still answers the processor.
const start_link = (node, ctl, receive) => {
    const end = controller_end({ buffer: ctl.buffer, table: ctl.table });
    const send = () => {
        const m = end.take();
        if (m) node.port.postMessage({ link: m.msg }, m.transfer);
    };
    receive((msg) => { end.land(msg); send(); });
    const timer = setInterval(send, 10);
    return { send, stop: () => { clearInterval(timer); receive(() => {}); } };
};

// `base_url`: the bundle directory (".../GainDemo.web/"). `name`: its base name. `canvas`: where the editor draws.
// `storage`: false keeps the editor's files (prefs, user presets) in memory for this page only (see load_controller).
// The node's input 0 is the main input, input 1 the sidechain (when the manifest declares one); output 0 is stereo.
export const load_plugin = async ({ base_url, name, context = new AudioContext(), canvas = null, storage = true }) => {
    linked_for(await read_manifest(new URL("manifest.json", base_url))); // Before the controller needs threads.
    const ctl = await load_controller(new URL(`${name}_controller.mjs`, base_url).href, { storage });
    const linked = linked_for(ctl.manifest);
    ctl.start(context.sampleRate);
    console.debug(`[tinyplug] load: processor module (AudioContext ${context.state})`);
    const { node, memory, table, receive } = await start_processor(context, new URL(`${name}_processor.wasm`, base_url), ctl.plain_params(), linked);
    console.debug("[tinyplug] load: wire");
    const wire = linked ? start_link(node, ctl, receive) : await start_pump(memory, table, ctl);

    const by_id = new Map(ctl.manifest.params.map((p) => [p.identifier, p]));
    const address = (id) => (typeof id === "number" ? id : by_id.get(id)?.address);
    const listeners = { meters: [], midi: [] };
    let running = true;
    const frame = () => {
        if (!running) return;
        ctl.tick();
        wire.send(); // Linked: this frame's edits go now, not at the next timer.
        if (listeners.meters.length) { const m = ctl.meters(); listeners.meters.forEach((f) => f(m)); }
        const midi = ctl.take_midi(); // Always taken, so the controller's queue never fills.
        if (midi.length) listeners.midi.forEach((f) => midi.forEach((e) => f(e)));
        requestAnimationFrame(frame);
    };
    requestAnimationFrame(frame);

    const editor = canvas ? attach_editor(ctl, canvas) : null;
    return {
        context, node, manifest: ctl.manifest, editor, controller: ctl,
        get latency() { return ctl.latency(); },
        setParam: (id, plain) => ctl.set_param(address(id), plain),
        paramKnob: (id) => ctl.param_knob(address(id)),
        setBypass: (on) => ctl.send(encode.bypass(on)),
        reset: () => ctl.send(encode.host_reset()),
        transport: (t) => ctl.send(encode.transport(t)),
        midi: (status, data1, data2) => ctl.send(encode.midi(status, data1, data2)),
        getState: () => ctl.save_state(),
        setState: (text) => ctl.load_state(text),
        onMeters: (f) => listeners.meters.push(f),
        onMidi: (f) => listeners.midi.push(f), // { frame, bytes }: route to Web MIDI, a synth, the page.
        dispose: () => {
            running = false;
            editor?.close();
            wire.stop();
            node.disconnect();
        },
    };
};
