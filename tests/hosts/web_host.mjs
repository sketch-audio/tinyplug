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
