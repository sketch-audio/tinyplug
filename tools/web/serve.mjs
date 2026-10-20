// A static server for the web format, with the cross-origin isolation SharedArrayBuffer needs.
//   /js/*                 wrappers/web/js
//   /plugins/<Base>.web/* the build's bundles (build-web/examples/*/<Base>.web)
//   /*                    tools/web (host pages)
// usage: node tools/web/serve.mjs [--port 8040] [--build build-web]
import { createServer } from "node:http";
import { readFile, readdir } from "node:fs/promises";
import { extname, join, normalize, resolve } from "node:path";

const root = resolve(new URL("../..", import.meta.url).pathname);
const arg = (name, fallback) => { const i = process.argv.indexOf(name); return i > 0 ? process.argv[i + 1] : fallback; };
const port = Number(arg("--port", 8040));
const build = resolve(root, arg("--build", "build-web"));

const types = { ".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".wasm": "application/wasm", ".json": "application/json", ".css": "text/css" };

const bundles = async () => {
    const out = new Map();
    for (const demo of await readdir(join(build, "examples")).catch(() => [])) {
        for (const entry of await readdir(join(build, "examples", demo)).catch(() => [])) {
            if (entry.endsWith(".web")) out.set(entry, join(build, "examples", demo, entry));
        }
    }
    return out;
};

const locate = async (path) => {
    const parts = normalize(path).split("/").filter(Boolean);
    if (parts[0] === "js") return join(root, "wrappers/web/js", ...parts.slice(1));
    if (parts[0] === "plugins") { const dir = (await bundles()).get(parts[1]); return dir && join(dir, ...parts.slice(2)); }
    return join(root, "tools/web", ...(parts.length ? parts : ["index.html"]));
};

createServer(async (req, res) => {
    const headers = {
        "Cross-Origin-Opener-Policy": "same-origin",
        "Cross-Origin-Embedder-Policy": "require-corp",
        "Cache-Control": "no-store",
    };
    try {
        const file = await locate(new URL(req.url, "http://x").pathname);
        if (!file) throw new Error("not found");
        const body = await readFile(file);
        res.writeHead(200, { ...headers, "Content-Type": types[extname(file)] ?? "application/octet-stream" });
        res.end(body);
    } catch {
        res.writeHead(404, headers);
        res.end("not found");
    }
}).listen(port, () => console.log(`serving on http://localhost:${port}`));
