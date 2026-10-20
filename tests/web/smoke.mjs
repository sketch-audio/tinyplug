// Headless Chrome over CDP: a demo's smoke page, offline-rendered and muted. No dependencies (Node's WebSocket).
// usage: node tests/web/smoke.mjs <Base> [--build build-web] [--page smoke.html] [--result tiny_smoke] [--shot out.png]
import { spawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";

const base = process.argv[2] ?? "GainDemo";
const arg = (name, fallback) => (process.argv.includes(name) ? process.argv[process.argv.indexOf(name) + 1] : fallback);
const build = arg("--build", "build-web");
const page = arg("--page", "smoke.html");
const result_name = arg("--result", "tiny_smoke");
const shot = arg("--shot", null);
const drag = arg("--drag", null)?.split(",").map(Number); // x0,y0,x1,y1 in page CSS pixels.
const probe = arg("--eval", null); // An expression to report after the drag.
const root = resolve(new URL("../..", import.meta.url).pathname);
const chrome = process.env.CHROME ?? "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome";
const http_port = 8041, cdp_port = 9241;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const serve_root = arg("--root", null);
const server = spawn("node", [join(root, "tools/web/serve.mjs"), "--port", String(http_port), "--build", build, ...(serve_root ? ["--root", serve_root] : [])], { stdio: "ignore" });
const profile = await mkdtemp(join(tmpdir(), "tiny-chrome-"));
const browser = spawn(chrome, ["--headless=new", "--mute-audio", "--autoplay-policy=no-user-gesture-required", "--no-first-run", "--no-default-browser-check",
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
    await send("Page.enable");
    await send("Emulation.setDeviceMetricsOverride", { width: 900, height: 700, deviceScaleFactor: 2, mobile: false });
    await send("Page.navigate", { url: `http://localhost:${http_port}/${page}?plugin=${base}&auto${arg("--query", "")}` });
    let result;
    for (let i = 0; i < 100 && !result; ++i) {
        await sleep(100);
        const r = await send("Runtime.evaluate", { expression: `window.${result_name} && JSON.stringify(window.${result_name})`, returnByValue: true });
        if (r.result?.result?.value) result = JSON.parse(r.result.result.value);
    }
    console.log(JSON.stringify(result ?? { ok: false, error: "timed out" }));
    if (drag && result?.ok) {
        const [x0, y0, x1, y1] = drag;
        const mouse = (type, x, y, buttons) => send("Input.dispatchMouseEvent", { type, x, y, button: "left", buttons, clickCount: 1 });
        await mouse("mouseMoved", x0, y0, 0);
        await mouse("mousePressed", x0, y0, 1);
        const steps = Number(arg("--steps", 10)), hold = Number(arg("--hold", 0));
        for (let i = 1; i <= steps; ++i) { await mouse("mouseMoved", x0 + ((x1 - x0) * i) / steps, y0 + ((y1 - y0) * i) / steps, 1); await sleep(30); }
        await sleep(hold);
        await mouse("mouseReleased", x1, y1, 0);
        await sleep(500);
    }
    await sleep(Number(arg("--wait", 0)));
    if (arg("--perf", null) && result?.ok) { // Main-thread busy time over a window, from Chrome's own task metrics.
        await send("Performance.enable");
        const metric = async () => Object.fromEntries((await send("Performance.getMetrics")).result.metrics.map((x) => [x.name, x.value]));
        const ms = Number(arg("--perf", 3000));
        const a = await metric();
        await sleep(ms);
        const b = await metric();
        const busy = (k) => (((b[k] - a[k]) * 1000) / ms * 100).toFixed(1);
        console.log(`perf: task ${busy("TaskDuration")}%  script ${busy("ScriptDuration")}%  over ${ms} ms`);
    }
    if (probe && result?.ok) {
        const r = await send("Runtime.evaluate", { expression: `JSON.stringify(${probe})`, returnByValue: true });
        console.log(`eval: ${r.result?.result?.value ?? JSON.stringify(r.result)}`);
    }
    if (shot && result?.ok) {
        await sleep(1000); // A few editor frames.
        const { result: { data } } = await send("Page.captureScreenshot", { format: "png" });
        const { writeFile } = await import("node:fs/promises");
        await writeFile(shot, Buffer.from(data, "base64"));
        console.log(`screenshot: ${shot}`);
    }
    let expected = true;
    if (arg("--expect", null) && result?.ok) {
        const r = await send("Runtime.evaluate", { expression: `Boolean(${arg("--expect", "true")})`, returnByValue: true });
        expected = r.result?.result?.value === true;
        console.log(`expect ${arg("--expect", "")}: ${expected}`);
    }
    ws.close();
    await cleanup();
    process.exit(result?.ok && expected ? 0 : 1);
}
catch (e) {
    console.log(String(e));
    await cleanup();
    process.exit(1);
}
