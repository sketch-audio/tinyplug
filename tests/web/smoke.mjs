// Headless Chrome over CDP: a demo's smoke page, offline-rendered and muted. No dependencies (Node's WebSocket).
// usage: node tests/web/smoke.mjs <Base> [--build build-web]
import { spawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const base = process.argv[2] ?? "GainDemo";
const build = process.argv.includes("--build") ? process.argv[process.argv.indexOf("--build") + 1] : "build-web";
const root = resolve(new URL("../..", import.meta.url).pathname);
const chrome = process.env.CHROME ?? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const http_port = 8041, cdp_port = 9241;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const server = spawn("node", [join(root, "tools/web/serve.mjs"), "--port", String(http_port), "--build", build], { stdio: "ignore" });
const profile = await mkdtemp(join(tmpdir(), "tiny-chrome-"));
const browser = spawn(chrome, ["--headless=new", "--mute-audio", "--no-first-run", "--no-default-browser-check",
    `--user-data-dir=${profile}`, `--remote-debugging-port=${cdp_port}`, "about:blank"], { stdio: "ignore" });

const cleanup = async () => { browser.kill(); server.kill(); await sleep(200); await rm(profile, { recursive: true, force: true }); };

try {
    let target;
    for (let i = 0; i < 50 && !target; ++i) {
        await sleep(100);
        target = await fetch(`http://127.0.0.1:${cdp_port}/json/list`).then((r) => r.json()).then((l) => l.find((t) => t.type === "page")).catch(() => undefined);
    }
    if (!target) throw new Error("Chrome didn't start");

    const ws = new WebSocket(target.webSocketDebuggerUrl);
    await new Promise((r) => ws.addEventListener("open", r, { once: true }));
    let id = 0;
    const pending = new Map();
    ws.addEventListener("message", ({ data }) => {
        const msg = JSON.parse(data);
        if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
        if (msg.method === "Runtime.consoleAPICalled") console.log(`    | ${msg.params.args.map((a) => a.value ?? a.description).join(" ")}`);
        if (msg.method === "Runtime.exceptionThrown") console.log(`    ! ${msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text}`);
    });
    const send = (method, params = {}) => new Promise((r) => { const n = ++id; pending.set(n, r); ws.send(JSON.stringify({ id: n, method, params })); });

    await send("Runtime.enable");
    await send("Page.navigate", { url: `http://localhost:${http_port}/smoke.html?plugin=${base}` });
    let result;
    for (let i = 0; i < 100 && !result; ++i) {
        await sleep(100);
        const r = await send("Runtime.evaluate", { expression: "window.tiny_smoke && JSON.stringify(window.tiny_smoke)", returnByValue: true });
        if (r.result?.result?.value) result = JSON.parse(r.result.result.value);
    }
    console.log(JSON.stringify(result ?? { ok: false, error: "timed out" }));
    ws.close();
    await cleanup();
    process.exit(result?.ok ? 0 : 1);
}
catch (e) {
    console.log(String(e));
    await cleanup();
    process.exit(1);
}
