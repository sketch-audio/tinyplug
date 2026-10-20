// @tinyplug/web: the page's host for a tinyplug plug-in. It is the DAW: it owns the AudioContext, the plug-in's
// node and its transport, and gives the page a host API.
//   load_plugin: the full format. The controller module on this thread (param authority, editor, undo, state,
//                worker), the processor module in the worklet, the pump between them on its own worker.
//   load:        processor-only. The page edits plain values and reads meters straight from the processor's rings.
import { Ring } from "./wire.js";
import { create_memory, encode, decode } from "./processor.js";
import { load_controller } from "./controller.js";
import { attach_editor } from "./editor.js";

const here = new URL(".", import.meta.url);

export const load = async ({ processor_url, context = new AudioContext(), params = undefined }) => {
    const module = await WebAssembly.compileStreaming(fetch(processor_url));
    const memory = create_memory();
    await context.audioWorklet.addModule(new URL("worklet.js", here));

    let ready;
    const ready_promise = new Promise((resolve) => { ready = resolve; });
    const node = new AudioWorkletNode(context, "tinyplug", {
        numberOfInputs: 2, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { module, memory, params },
    });
    node.port.onmessage = ({ data }) => {
        if (data.log) console.log(`[tinyplug] ${data.log}`);
        if (data.ready) ready(data);
    };
    const { manifest, table } = await ready_promise; // The rings' offsets come from the processor's own table.
    return make_host({ context, node, memory, manifest, table });
};

const make_host = ({ context, node, memory, manifest, table }) => {
    const by_id = new Map(manifest.params.map((p) => [p.identifier, p]));
    const address = (id) => (typeof id === "number" ? id : by_id.get(id)?.address);
    const inbound = new Ring(memory.buffer, table.inbound, table.inbound_capacity);
    const outbound = new Ring(memory.buffer, table.outbound, table.outbound_capacity);
    const listeners = { meters: [], latency: [] };
    const meters = new Float32Array(manifest.num_meters);
    let latency = 0;

    const send = ([kind, payload]) => inbound.push(kind, payload);

    // Drained once a frame: meters coalesce to their latest value, as the native editors' mailboxes do.
    const poll = () => {
        let changed = false;
        outbound.drain((kind, bytes) => {
            const m = decode(kind, bytes);
            if (m.kind === "meter" && m.address < meters.length) { meters[m.address] = m.value; changed = true; }
            if (m.kind === "latency") { latency = m.samples; listeners.latency.forEach((f) => f(latency)); }
        });
        if (changed) listeners.meters.forEach((f) => f(meters));
        requestAnimationFrame(poll);
    };
    requestAnimationFrame(poll);

    return {
        context, node, manifest,
        get latency() { return latency; },
        setParam: (id, plain) => send(encode.set(address(id), plain)),
        rampParam: (id, plain, samples) => send(encode.ramp(address(id), plain, samples)),
        setBypass: (on) => send(encode.bypass(on)),
        reset: () => send(encode.reset_hard()),
        transport: (t) => send(encode.transport(t)),
        onMeters: (f) => listeners.meters.push(f),
        onLatency: (f) => listeners.latency.push(f),
    };
};

// MARK: - the full format

const start_processor = async (context, processor_url, params) => {
    const module = await WebAssembly.compileStreaming(fetch(processor_url));
    const memory = create_memory();
    await context.audioWorklet.addModule(new URL("worklet.js", here));
    let ready;
    const ready_promise = new Promise((resolve) => { ready = resolve; });
    const node = new AudioWorkletNode(context, "tinyplug", {
        numberOfInputs: 2, numberOfOutputs: 1, outputChannelCount: [2],
        processorOptions: { module, memory, params },
    });
    node.port.onmessage = ({ data }) => {
        if (data.log) console.log(`[tinyplug] ${data.log}`);
        if (data.ready) ready(data);
    };
    const { table } = await ready_promise;
    return { node, memory, table };
};

// `base_url`: the bundle directory (".../GainDemo.web/"). `name`: its base name. `canvas`: where the editor draws.
export const load_plugin = async ({ base_url, name, context = new AudioContext(), canvas = null }) => {
    const ctl = await load_controller(new URL(`${name}_controller.mjs`, base_url).href);
    ctl.start(context.sampleRate);
    const { node, memory, table } = await start_processor(context, new URL(`${name}_processor.wasm`, base_url), ctl.plain_params());

    const stop = new Int32Array(new SharedArrayBuffer(4));
    const pump = new Worker(new URL("pump_worker.js", here), { type: "module" });
    await new Promise((resolve) => {
        pump.onmessage = ({ data }) => { if (data.started) resolve(); };
        pump.postMessage({ spec: { processor: { buffer: memory.buffer, table }, controller: { buffer: ctl.buffer(), table: ctl.table } }, stop });
    });

    const by_id = new Map(ctl.manifest.params.map((p) => [p.identifier, p]));
    const address = (id) => (typeof id === "number" ? id : by_id.get(id)?.address);
    const listeners = { meters: [] };
    let running = true;
    const frame = () => {
        if (!running) return;
        ctl.tick();
        if (listeners.meters.length) { const m = ctl.meters(); listeners.meters.forEach((f) => f(m)); }
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
        reset: () => ctl.send(encode.reset_hard()),
        transport: (t) => ctl.send(encode.transport(t)),
        midi: (status, data1, data2) => ctl.send(encode.midi(status, data1, data2)),
        getState: () => ctl.save_state(),
        setState: (text) => ctl.load_state(text),
        onMeters: (f) => listeners.meters.push(f),
        dispose: () => {
            running = false;
            editor?.close();
            Atomics.store(stop, 0, 1);
            node.disconnect();
        },
    };
};
