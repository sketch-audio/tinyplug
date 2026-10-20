// The AudioWorkletProcessor: the processor module on the audio thread. The page compiles the module and creates
// the shared memory, so the main thread (and later the pump) can reach the same rings.
import { create_processor } from "./processor.js";

class Tinyplug_processor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        const { module, memory, params } = options.processorOptions;
        this.p = create_processor(module, memory, { log: (line) => this.port.postMessage({ log: line }) });
        params?.forEach((value, i) => { this.p.params[i] = value; });
        this.p.configure(sampleRate); // Allocates: before the first render, never inside one.
        this.port.postMessage({ ready: true, manifest: this.p.manifest(), table: this.p.table });
    }

    process(inputs, outputs) {
        const main = inputs[0] ?? [], side = inputs[1] ?? [], out = outputs[0] ?? [];
        const frames = out[0]?.length ?? 128;
        const copy_in = (from, to) => to.forEach((ch, i) => { const src = from[Math.min(i, from.length - 1)]; src ? ch.set(src) : ch.fill(0, 0, frames); });
        copy_in(main, this.p.inputs);
        copy_in(side, this.p.sidechain);
        this.p.process(frames);
        out.forEach((ch, i) => ch.set(this.p.outputs[Math.min(i, this.p.outputs.length - 1)].subarray(0, frames)));
        return true;
    }
}

registerProcessor("tinyplug", Tinyplug_processor);
