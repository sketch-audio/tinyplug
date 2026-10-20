// web_split_host.mjs's audio thread: the processor module rendering a quantum every ~2.7 ms, as the worklet would.
// Writes [frames, peak of each output channel] into a shared Float64Array for the main thread.
import { workerData, parentPort } from "node:worker_threads";
import { create_processor } from "../../wrappers/web/js/processor.js";

const { module, memory, params, stats, stop, level } = workerData;
const p = create_processor(module, memory, { log: (line) => parentPort.postMessage({ log: line }) });
params.forEach((v, i) => { p.params[i] = v; });
p.configure(48000);
parentPort.postMessage({ table: p.table });

const out = new Float64Array(stats);
const sleeper = new Int32Array(new SharedArrayBuffer(4));
for (let frame = 0; Atomics.load(stop, 0) === 0; frame += 128) {
    for (const ch of p.inputs) for (let i = 0; i < 128; ++i) ch[i] = level * Math.sin(2 * Math.PI * 440 * (frame + i) / 48000);
    p.process(128);
    out[0] = frame + 128;
    p.outputs.forEach((ch, c) => { let m = 0; for (let i = 0; i < 128; ++i) m = Math.max(m, Math.abs(ch[i])); out[1 + c] = m; });
    Atomics.wait(sleeper, 0, 0, 2.6);
}
