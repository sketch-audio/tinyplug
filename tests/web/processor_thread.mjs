// web_split_host.mjs's audio thread: the processor module rendering a quantum every ~2.7 ms, as the worklet would.
// Writes [frames, peak of each output channel] into a shared Float64Array for the main thread, and at [7] the first
// byte of the latest session load the processor holds. Linked (the nonisolated variant): its own memory, and
// link.js's processor end over `port`, posting every 4 quanta as worklet.js does.
import { workerData, parentPort, receiveMessageOnPort } from "node:worker_threads";
import { create_memory, create_processor } from "../../wrappers/web/js/processor.js";
import { Store } from "../../wrappers/web/js/wire.js";
import { processor_end } from "../../wrappers/web/js/link.js";

const { module, memory, params, stats, stop, level, linked, port } = workerData;
const mem = linked ? create_memory(false) : memory;
const p = create_processor(module, mem, { log: (line) => parentPort.postMessage({ log: line }) });
params.forEach((v, i) => { p.params[i] = v; });
p.configure(48000);
parentPort.postMessage({ table: p.table });

const link = linked ? processor_end({ buffer: mem.buffer, table: p.table }) : null;
const t = p.table.state_load;
const load = t.offset ? new Store(mem.buffer, t.offset, t.frame_bytes, t.slot_offset) : null;
const load_frame = load ? new Uint8Array(load.frame_bytes) : null;

const out = new Float64Array(stats);
const sleeper = new Int32Array(new SharedArrayBuffer(4));
for (let frame = 0, quanta = 0; Atomics.load(stop, 0) === 0; frame += 128, ++quanta) {
    for (let m; link && (m = receiveMessageOnPort(port));) link.land(m.message);
    link?.flush();
    for (const ch of p.inputs) for (let i = 0; i < 128; ++i) ch[i] = level * Math.sin(2 * Math.PI * 440 * (frame + i) / 48000);
    p.process(128);
    if (link && quanta % 4 === 3) {
        const m = link.take();
        if (m) port.postMessage(m.msg, m.transfer);
    }
    out[0] = frame + 128;
    p.outputs.forEach((ch, c) => { let m = 0; for (let i = 0; i < 128; ++i) m = Math.max(m, Math.abs(ch[i])); out[1 + c] = m; });
    if (load && load.read(load_frame) > 0n) out[7] = load_frame[0];
    Atomics.wait(sleeper, 0, 0, 2.6);
}
port?.close();
