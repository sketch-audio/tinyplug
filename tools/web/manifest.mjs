// Writes a bundle's manifest.json from its processor module (the models are compiled in; nothing to keep in sync).
// usage: node tools/web/manifest.mjs <Base_processor.wasm> <manifest.json>
import { readFile, writeFile } from "node:fs/promises";
import { create_memory, create_processor } from "../../wrappers/web/js/processor.js";

const [, , wasm, out] = process.argv;
const p = create_processor(new WebAssembly.Module(await readFile(wasm)), create_memory(), { log: () => {} });
await writeFile(out, JSON.stringify(p.manifest(), null, 2) + "\n");
