// wire.js against <tiny_core/wire.hpp>: two instances of wire_harness (two memories, as the web format's two
// modules), with C++ producing and checking and JS pumping between them.
import { Worker } from "node:worker_threads";
import { Ring, Store, pump_ring, pump_store } from "../../wrappers/web/js/wire.js";

const [, , harness] = process.argv;
const load = async () => (await import(harness)).default();

let failures = 0;
const expect = (ok, what) => {
    if (!ok) ++failures;
    console.log(`  [${ok ? "pass" : "FAIL"}] ${what}`);
};

const a = await load();
const b = await load();
expect(a.HEAPU8.buffer !== b.HEAPU8.buffer, "two modules, two memories");
expect(a.HEAPU8.buffer instanceof SharedArrayBuffer, "memory is shared");

const ring = (m, i) => new Ring(m.HEAPU8.buffer, m._wire_ring(i), m._wire_ring_capacity());
expect(a._wire_ring_data_offset() === 16, "ring layout matches: data at 16");

// A -> B: C++ pushes in A, JS pumps, C++ checks in B. Random rates so both rings fill and wrap.
{
    const from = ring(a, 0), to = ring(b, 0);
    let rng = 12345;
    const next = (n) => (rng = (Math.imul(rng, 1103515245) + 12345) >>> 0) % n;
    let pushed = 0, consumed = 0, ok = true;
    for (let i = 0; i < 20000; ++i) {
        pushed += a._wire_push(0, next(8));
        if (next(3) > 0) pump_ring(from, to);
        if (next(2) > 0) {
            const c = b._wire_consume(0);
            if (c < 0) ok = false; else consumed += c;
        }
    }
    while (pushed !== consumed && ok) {
        pump_ring(from, to);
        const c = b._wire_consume(0);
        if (c < 0) ok = false; else consumed += c;
    }
    expect(ok, "every entry arrives in order with its bytes, across wraparound");
    expect(pushed > 20000 && pushed === consumed, `nothing lost under backpressure (${pushed} entries)`);
}

// JS as producer and consumer of the same layout: push from JS into A, C++ can't tell.
{
    const r = ring(a, 1);
    let seq = 0, ok = true, total = 0;
    for (let i = 0; i < 2000; ++i) {
        for (;;) {
            const n = seq % 61;
            const payload = Uint8Array.from({ length: n }, (_, k) => (seq + k) & 0xff);
            if (!r.push(seq, payload)) break;
            ++seq;
        }
        const c = a._wire_consume(1);
        if (c < 0) ok = false; else total += c;
    }
    expect(ok && total === seq, "C++ reads what JS pushed");
    expect(!r.push(0, new Uint8Array(r.capacity)), "a push that can't fit is refused");
}

// Store: C++ publishes in A, JS pumps the latest frame to B, C++ reads a consistent frame there.
{
    const store = (m) => new Store(m.HEAPU8.buffer, m._wire_store(), m._wire_store_frame_bytes(), m._wire_store_slot_offset());
    const from = store(a), to = store(b), state = {};
    expect(b._wire_store_value() === 0n, "nothing published reads as nothing");
    expect(!pump_store(from, to, state), "nothing to pump before a publish");
    let ok = true;
    for (let v = 1; v <= 5000; ++v) {
        a._wire_publish(v);
        if (v % 3 === 0) a._wire_publish(v); // Twice: the pump only moves the latest.
        pump_store(from, to, state);
        if (b._wire_store_value() !== BigInt(v)) ok = false;
    }
    expect(ok, "the latest frame arrives whole every time");
    expect(!pump_store(from, to, state), "an unchanged store isn't pumped again");
}

// The real shape: the pump on its own thread, holding only the two memories, while C++ produces and consumes.
{
    const c = await load(), d = await load();
    const spec = { from: [c.HEAPU8.buffer, c._wire_ring(0)], to: [d.HEAPU8.buffer, d._wire_ring(0)], capacity: c._wire_ring_capacity(),
                   stores: [[c.HEAPU8.buffer, c._wire_store()], [d.HEAPU8.buffer, d._wire_store()]],
                   frame_bytes: c._wire_store_frame_bytes(), slot_offset: c._wire_store_slot_offset() };
    const stop = new Int32Array(new SharedArrayBuffer(4));
    const pump = new Worker(new URL("./wire_pump_worker.mjs", import.meta.url), { workerData: { spec, stop } });
    const started = new Promise((resolve) => pump.once("message", resolve));
    await started;

    let pushed = 0, consumed = 0, ok = true, torn = 0, retried = 0;
    const deadline = Date.now() + 2000;
    for (let v = 1; Date.now() < deadline; ++v) {
        pushed += c._wire_push(0, 4);
        const n = d._wire_consume(0);
        if (n < 0) ok = false; else consumed += n;
        c._wire_publish(v);
        const value = d._wire_store_value();
        if (value === -2n) ++torn;
        if (value === -1n) ++retried;
        if (v % 64 === 0) await new Promise((r) => setImmediate(r)); // Let the worker's messages through.
    }
    for (const until = Date.now() + 1000; consumed < pushed && Date.now() < until;) {
        const n = d._wire_consume(0);
        if (n < 0) { ok = false; break; }
        consumed += n;
        await new Promise((r) => setImmediate(r));
    }
    Atomics.store(stop, 0, 1);
    await new Promise((r) => pump.once("exit", r));
    expect(ok && pushed > 1000 && pushed === consumed, `a concurrent pump keeps order and loses nothing (${pushed} entries)`);
    expect(torn === 0, `a concurrently pumped store never passes a torn copy (${retried} overlaps caught and retried)`);
}

console.log(failures ? `FAILED (${failures})` : "ok");
process.exit(failures ? 1 : 0);
