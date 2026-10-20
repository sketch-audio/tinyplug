// The web processor module against its native build (native_render.cpp), same schedule, sample by sample.
// usage: node compare.mjs <Base_processor.wasm> (<native.f32> | --native-exe <Base_web_native>) [--tolerance dB]
// Exact by default; a plug-in whose DSP calls libm (sin, exp: musl in wasm, the platform's natively) sets a tolerance.
import { readFile, mkdtemp, rm } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { create_memory, create_processor, encode } from "../../wrappers/web/js/processor.js";

const [, , wasm, second] = process.argv;
const exe = process.argv.includes("--native-exe") ? process.argv[process.argv.indexOf("--native-exe") + 1] : null;
let native_path = second, scratch = null;
if (exe) {
    scratch = await mkdtemp(join(tmpdir(), "tiny-compare-"));
    native_path = join(scratch, "native.f32");
    if (spawnSync(exe, [native_path], { stdio: "inherit" }).status !== 0) { console.log(`native render failed: ${exe}`); process.exit(1); }
}
const tolerance_db = process.argv.includes("--tolerance") ? Number(process.argv[process.argv.indexOf("--tolerance") + 1]) : null;

const p = create_processor(new WebAssembly.Module(await readFile(wasm)), create_memory(), { log: () => {} });
const manifest = p.manifest();
const param = manifest.params[0];
p.configure(48000);

const out = [];
let seed = 1;
for (let block = 0; block < 120; ++block) {
    const push = ([kind, payload]) => p.inbound.push(kind, payload);
    if (block === 2 && manifest.notes_in) push(encode.midi(0x90, 60, 100, 11));
    if (block === 10) push(encode.set(0, param.max, 37));
    if (block === 40) push(encode.ramp(0, param.min, 3000, 5));
    if (block === 50 && manifest.notes_in) push(encode.midi(0x80, 60, 0, 3));
    if (block === 60) push(encode.bypass(true));
    if (block === 80) push(encode.bypass(false));
    for (let i = 0; i < 128; ++i) {
        seed = (Math.imul(seed, 1103515245) + 12345) >>> 0;
        const x = Math.fround(((seed >>> 8) - (1 << 23)) / (1 << 24));
        for (const ch of p.inputs) ch[i] = x;
    }
    p.process(128);
    for (let i = 0; i < 128; ++i) for (const ch of p.outputs) out.push(ch[i]);
    p.outbound.drain(() => {});
}

const web = Float32Array.from(out);
const native = new Float32Array((await readFile(native_path)).buffer.slice(0));
if (scratch) await rm(scratch, { recursive: true, force: true });
let exact = 0, max_diff = 0, peak = 0;
for (let i = 0; i < web.length; ++i) {
    if (web[i] === native[i]) ++exact;
    max_diff = Math.max(max_diff, Math.abs(web[i] - native[i]));
    peak = Math.max(peak, Math.abs(native[i]));
}
const db = max_diff === 0 ? -Infinity : 20 * Math.log10(max_diff / Math.max(peak, 1e-30));
const same_length = web.length === native.length;
console.log(`${manifest.name}: ${exact}/${web.length} samples identical, max difference ${db.toFixed(1)} dB re peak`);
const ok = same_length && (tolerance_db === null ? exact === web.length : db <= tolerance_db);
process.exit(ok ? 0 : 1);
