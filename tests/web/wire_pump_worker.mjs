// wire_test.mjs's pump thread: the two memories and nothing else, as the web format's pump worker.
import { parentPort, workerData } from "node:worker_threads";
import { Ring, Store, pump_ring, pump_store } from "../../wrappers/web/js/wire.js";

const { spec, stop } = workerData;
const from = new Ring(spec.from[0], spec.from[1], spec.capacity);
const to = new Ring(spec.to[0], spec.to[1], spec.capacity);
const [store_from, store_to] = spec.stores.map(([buffer, offset]) => new Store(buffer, offset, spec.frame_bytes, spec.slot_offset));
const state = {};
const idle = new Int32Array(new SharedArrayBuffer(4));

parentPort.postMessage("started");
while (Atomics.load(stop, 0) === 0) {
    const moved = pump_ring(from, to);
    pump_store(store_from, store_to, state);
    if (moved === 0) Atomics.wait(idle, 0, 0, 0.05); // A short sleep, as the real pump's timeout.
}
