// The web format's conformance host: a demo's processor module driven as the AudioWorklet drives it, under Node.
// usage: node web_host.mjs <Base.web directory>
import { readFile } from "node:fs/promises";
import { basename, join } from "node:path";
import { create_memory, create_processor, encode, decode } from "../../wrappers/web/js/processor.js";

const [, , dir] = process.argv;
const base = basename(dir, ".web");

let failures = 0;
const expect = (ok, what) => {
    if (!ok) ++failures;
    console.log(`  [${ok ? "pass" : "FAIL"}] ${what}`);
};
const near = (a, b, tol = 1e-6) => Math.abs(a - b) <= tol;

const module = new WebAssembly.Module(await readFile(join(dir, `${base}_processor.wasm`)));
const load = () => create_processor(module, create_memory(), { log: (line) => console.log(`    | ${line}`) });

const quantum = 128;
const sr = 48000;

// Pushes encoded messages, then renders `blocks` quanta of `signal(frame)` into every input channel.
const render = (p, blocks, signal = () => 0, messages = []) => {
    for (const [kind, payload] of messages) if (!p.inbound.push(kind, payload)) throw new Error("inbound ring full");
    let frame = render.frames.get(p) ?? 0;
    for (let b = 0; b < blocks; ++b) {
        for (let i = 0; i < quantum; ++i) for (const ch of p.inputs) ch[i] = signal(frame + i);
        p.process(quantum);
        frame += quantum;
    }
    render.frames.set(p, frame);
};
render.frames = new Map();

const drain = (p) => {
    const out = [];
    p.outbound.drain((kind, bytes) => { out.push(decode(kind, bytes)); });
    return out;
};

const peak = (channel, frames = quantum) => Math.max(...channel.subarray(0, frames).map(Math.abs));
const rendered = (channel) => channel.subarray(0, quantum); // Buffers hold max_frames; only a quantum is rendered.
const passes_through = (p) => p.outputs.every((ch, i) => rendered(ch).every((x, f) => near(x, p.inputs[i][f])));

const noise = (() => { let x = 1; return () => ((x = (Math.imul(x, 1103515245) + 12345) >>> 0) / 2 ** 32 - 0.5) * 0.5; })();
const finite = (p) => p.outputs.every((ch) => rendered(ch).every(Number.isFinite));
const published = (store) => store.read(new Uint8Array(store.frame_bytes)) > 0n;
const param = (p, id) => p.manifest_cache.params.find((x) => x.identifier === id);

// What every demo owes: it configures, renders, and stays finite.
const basics = (p) => {
    p.manifest_cache = p.manifest();
    p.configure(sr);
    render(p, 50, noise);
    expect(finite(p), "renders finite audio");
    return drain(p);
};

const scenarios = {
    GainDemo: () => {
        const p = load();
        const manifest = p.manifest();
        const gain = manifest.params.find((x) => x.identifier === "gain");
        expect(manifest.name === "Gain Demo" && manifest.num_meters === 4, "manifest: name and meters");
        expect(gain && gain.kind === "real" && gain.default === 1 && gain.max === 4, "manifest: the gain param, plain range");

        p.configure(sr);
        expect(drain(p).some((m) => m.kind === "latency" && m.samples === 0), "configure reports its latency");

        const sine = (f) => 0.5 * Math.sin(2 * Math.PI * 440 * f / sr);
        render(p, 4, sine);
        expect(passes_through(p), "unity gain passes the input through");

        render(p, 100, sine, [encode.set(gain.address, 2)]);
        expect(near(peak(p.outputs[0]), 2 * peak(p.inputs[0]), 1e-4), "a page edit doubles the output once the glide lands");

        const meters = drain(p).filter((m) => m.kind === "meter");
        const last = (address) => meters.findLast((m) => m.address === address)?.value;
        expect(meters.length > 0, "meters arrive on the outbound ring");
        expect(near(last(0), 0.5, 1e-3) && near(last(2), 1.0, 1e-3), "meters: input and output peaks");

        render(p, 100, sine, [encode.bypass(true)]);
        expect(passes_through(p), "bypass fades to the dry input");
        render(p, 100, sine, [encode.bypass(false)]);
        expect(near(peak(p.outputs[0]), 2 * peak(p.inputs[0]), 1e-4), "un-bypass returns to the gain");

        // `configure` comes up holding the values it's given: no glide from the default on block 1.
        const q = load();
        q.params[gain.address] = 0.25;
        q.configure(sr);
        render(q, 1, () => 0.8);
        expect(rendered(q.outputs[0]).every((x) => near(x, 0.2)), "configure with a param value lands it on the first sample");
    },
    MeterDemo: () => {
        const p = load();
        const out = basics(p);
        expect(out.some((m) => m.kind === "meter"), "meters arrive");
    },
    PlatformDemo: () => { basics(load()); },
    StateDemo: () => {
        const p = load();
        basics(p);
        render(p, 50, noise, [encode.set(param(p, "depth").address, 1)]);
        expect(finite(p), "a depth change renders through the default pattern");
    },
    BlockDemo: () => {
        const p = load();
        basics(p);
        expect(p.blocks.length > 0 && p.blocks.every(published), `every block address publishes (${p.blocks.length})`);
    },
    AutomationDemo: () => {
        const p = load();
        basics(p);
        const value = param(p, "value");
        render(p, 20, noise, [encode.ramp(value.address, value.max, 256, 32)]);
        expect(finite(p) && p.blocks.every(published), "a ramp at an offset renders, and the log publishes");
    },
    LatencyDemo: () => {
        const p = load();
        const configured = basics(p).findLast((m) => m.kind === "latency")?.samples;
        const mode = param(p, "mode");
        render(p, 4, noise, [encode.set(mode.address, mode.max)]);
        const accepted = drain(p).findLast((m) => m.kind === "latency")?.samples;
        expect(configured !== undefined && accepted !== undefined && accepted !== configured,
            `a proposal is accepted at the next block (${configured} -> ${accepted})`);
        expect(p.blocks.every(published), "the handshake block publishes");
    },
    SineSynth: () => {
        const p = load();
        basics(p);
        render(p, 20, () => 0, [encode.midi(0x90, 60, 100)]);
        expect(peak(p.outputs[0]) > 0.01, "a MIDI note on sounds");
        render(p, 400, () => 0, [encode.midi(0x80, 60, 0)]);
        expect(peak(p.outputs[0]) < 1e-3, "and its note off releases it");
    },
    StepSequencer: () => {
        const p = load();
        basics(p);
        expect(!drain(p).some((m) => m.kind === "midi"), "stopped: no notes");
        render(p, 400, () => 0, [encode.transport({ tempo: 120, playing: true, numer: 4, denom: 4 })]);
        const notes = drain(p).filter((m) => m.kind === "midi" && (m.bytes[0] & 0xf0) === 0x90 && m.bytes[2] > 0);
        expect(notes.length > 0, `playing at 120 bpm sends notes out (${notes.length} note ons in ~1 s)`);
    },
    WorkerDemo: () => {
        const p = load();
        basics(p);
        render(p, 4, noise, [encode.set(param(p, "drive").address, 0.9)]);
        expect(drain(p).some((m) => m.kind === "worker"), "a drive change asks the worker for a design");
    },
};

const scenario = scenarios[base];
if (!scenario) {
    console.log(`no scenario for ${base}`);
    process.exit(1);
}
console.log(base);
scenario();
console.log(failures ? `FAILED (${failures})` : "ok");
process.exit(failures ? 1 : 0);
