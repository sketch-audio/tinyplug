// @tinyplug/web: the page's host for a tinyplug plug-in. It is the DAW: it owns the AudioContext, the plug-in's
// node and its transport, and gives the page a host API. Processor-only for now: the page edits plain values and
// reads meters straight from the processor module's rings, on the main thread.
import { Ring } from "./wire.js";
import { create_memory, encode, decode } from "./processor.js";

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
