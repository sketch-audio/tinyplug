// The AudioWorkletProcessor: the processor module on the audio thread. Isolated, the page compiles the module and
// creates the shared memory, so the main thread (and the pump) can reach the same rings. Linked (the nonisolated
// variant), the memory is the worklet's own and link.js's processor end posts what crosses.
import { create_memory, create_processor } from "./processor.js";
import { processor_end } from "./link.js";

const post_every = 4; // Quanta between posts out, linked: ~11 ms at 48 kHz, faster than any editor frame.

class Tinyplug_processor extends AudioWorkletProcessor {
    constructor(options) {
        super();
        const { module, memory, params, linked } = options.processorOptions;
        const mem = linked ? create_memory(false) : memory;
        this.p = create_processor(module, mem, { log: (line) => this.port.postMessage({ log: line }) });
        params?.forEach((value, i) => { this.p.params[i] = value; });
        this.p.configure(sampleRate); // Allocates: before the first render, never inside one.
        if (linked) {
            this.link = processor_end({ buffer: mem.buffer, table: this.p.table });
            this.quanta = 0;
            this.port.onmessage = ({ data }) => { if (data.link) this.link.land(data.link); };
        }
        this.port.postMessage({ ready: true, manifest: this.p.manifest(), table: this.p.table });
    }

    process(inputs, outputs) {
        const main = inputs[0] ?? [], side = inputs[1] ?? [], out = outputs[0] ?? [];
        const frames = out[0]?.length ?? 128;
        const copy_in = (from, to) => to.forEach((ch, i) => { const src = from[Math.min(i, from.length - 1)]; src ? ch.set(src) : ch.fill(0, 0, frames); });
        copy_in(main, this.p.inputs);
        copy_in(side, this.p.sidechain);
        this.link?.flush();
        this.p.process(frames);
        out.forEach((ch, i) => ch.set(this.p.outputs[Math.min(i, this.p.outputs.length - 1)].subarray(0, frames)));
        if (this.link && ++this.quanta % post_every === 0) {
            const m = this.link.take();
            if (m) this.port.postMessage({ link: m.msg }, m.transfer);
        }
        return true;
    }
}

registerProcessor("tinyplug", Tinyplug_processor);
