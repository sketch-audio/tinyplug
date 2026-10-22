// The web format in its real shape, under Node: the controller module on the main thread, the processor module on
// its own thread rendering as the worklet would, and the pump on a third. With --link (the nonisolated variant), no
// pump: link.js's two ends over a MessagePort, each on its own memory. usage: node web_split_host.mjs <Base.web> [--link]
import { MessageChannel, Worker } from "node:worker_threads";
import { readFile } from "node:fs/promises";
import { basename, join, resolve } from "node:path";
import { create_memory } from "../../wrappers/web/js/processor.js";
import { load_controller } from "../../wrappers/web/js/controller.js";
import { Store } from "../../wrappers/web/js/wire.js";
import { controller_end } from "../../wrappers/web/js/link.js";

const dir = resolve(process.argv[2]);
const base = basename(dir, ".web");
const linked = process.argv.includes("--link");

let failures = 0;
const expect = (ok, what) => {
    if (!ok) ++failures;
    console.log(`  [${ok ? "pass" : "FAIL"}] ${what}`);
};
const near = (a, b, tol) => Math.abs(a - b) <= tol;

const ctl = await load_controller(join(dir, `${base}_controller.mjs`));
ctl.start(48000);

const memory = linked ? null : create_memory();
const stats = new Float64Array(new SharedArrayBuffer(8 * 8));
const stop = new Int32Array(new SharedArrayBuffer(4));
const module = new WebAssembly.Module(await readFile(join(dir, `${base}_processor.wasm`)));
const level = 0.5;
const channel = linked ? new MessageChannel() : null;
const audio = new Worker(new URL("../web/processor_thread.mjs", import.meta.url), {
    workerData: { module, memory, params: ctl.plain_params(), stats: stats.buffer, stop, level, linked, port: channel?.port2 },
    transferList: channel ? [channel.port2] : [],
});
audio.on("message", (m) => { if (m.log) console.log(`    | ${m.log}`); });
const processor_table = await new Promise((r) => audio.on("message", (m) => { if (m.table) r(m.table); }));
const pump = linked ? null : new Worker(new URL("../web/pump_thread.mjs", import.meta.url), {
    workerData: { spec: { processor: { buffer: memory.buffer, table: processor_table }, controller: { buffer: ctl.buffer(), table: ctl.table } }, stop },
});

// Linked: the controller's end, sending each frame and whenever the processor's end posts, as host.js does.
const end = linked ? controller_end({ buffer: ctl.buffer, table: ctl.table }) : null;
const send = () => {
    const m = end?.take();
    if (m) channel.port1.postMessage(m.msg, m.transfer);
};
channel?.port1.on("message", (msg) => { end.land(msg); send(); });

// The page's frame loop: tick the controller every ~16 ms for `ms`.
const run = async (ms) => {
    for (const until = Date.now() + ms; Date.now() < until;) {
        ctl.tick();
        send();
        await new Promise((r) => setTimeout(r, 16));
    }
};
const peak = () => stats[1];
const param = (id) => ctl.manifest.params.find((p) => p.identifier === id);

const scenarios = {
    GainDemo: async () => {
        const gain = param("gain").address;
        await run(200);
        expect(stats[0] > 0, `the processor renders on its own thread (${stats[0]} frames)`);
        expect(near(peak(), level, 1e-3), "unity at the default");

        ctl.set_param(gain, 2);
        await run(300);
        expect(near(peak(), 2 * level, 1e-3), "a page edit crosses the pump");
        expect(near(ctl.meters()[0], level, 1e-2) && near(ctl.meters()[2], 2 * level, 1e-2), "meters come back to the controller");

        ctl.action("start", gain); ctl.action("set", gain, 0); ctl.action("end", gain);
        await run(300);
        expect(peak() < 1e-3, "an editor gesture lands (knob 0 mutes)");
        ctl.undo();
        await run(300);
        expect(near(peak(), 2 * level, 1e-3), "undo restores the page's value");
        ctl.redo();
        await run(300);
        expect(peak() < 1e-3, "redo mutes again");

        ctl.set_param(gain, 2);
        await run(100);
        const saved = ctl.save_state();
        expect(JSON.parse(saved).params !== undefined, "save_state is the preset document");
        ctl.set_param(gain, 0.5);
        await run(300);
        expect(ctl.load_state(saved), "load_state accepts it");
        await run(300);
        expect(near(peak(), 2 * level, 1e-3), "a load lands its values in the processor");
        ctl.undo();
        await run(300);
        expect(near(peak(), 0.5 * level, 1e-3), "and undoes as one step");
    },
    WorkerDemo: async () => {
        await run(200);
        ctl.set_param(param("drive").address, 0.9);
        await run(800);
        const entry = ctl.table.num_blocks > 0 && new Uint32Array(ctl.buffer(), ctl.table.blocks, 3);
        const store = new Store(ctl.buffer(), entry[0], entry[1], entry[2]);
        const frame = new Uint8Array(store.frame_bytes);
        expect(store.read(frame) > 0n, "the channel block reaches the controller");
        const v = new DataView(frame.buffer);
        const echoes = v.getUint32(4, true), curves = v.getUint32(20, true), drive = v.getFloat64(24, true);
        expect(echoes > 0, `heartbeats come back from the worker thread (${echoes})`);
        expect(curves > 0 && near(drive, 0.9, 1e-9), `a drive change returns a designed curve (${curves}, drive ${drive})`);
    },
    StateDemo: async () => {
        await run(200);
        const doc = JSON.parse(ctl.save_state());
        const record = Buffer.from(doc.state, "base64");
        expect(record[20] === 100, "the default pattern starts at 100 %");
        record[20] = 50; // Step 0's level: a 16-byte record header, the payload's version word, then the levels.
        doc.state = record.toString("base64");
        expect(ctl.load_state(JSON.stringify(doc)), "a changed pattern loads");
        expect(Buffer.from(JSON.parse(ctl.save_state()).state, "base64")[20] === 50, "the editor's half holds it");
        await run(300);
        expect(stats[7] === 50, "the load reaches the processor as a session load, with its pattern");
    },
};

console.log(`${base} (split${linked ? ", linked" : ""})`);
await (scenarios[base] ?? (async () => { await run(300); expect(stats[0] > 0, "renders"); }))();
Atomics.store(stop, 0, 1);
channel?.port1.close();
await Promise.all([audio, pump].filter(Boolean).map((w) => new Promise((r) => w.once("exit", r))));
console.log(failures ? `FAILED (${failures})` : "ok");
process.exit(failures ? 1 : 0);
