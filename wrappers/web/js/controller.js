// The controller module's JS end: the Emscripten factory, the exports as a small API, and its wire table.
import { read_controller_table } from "./pump.js";

// platform::Paths::shared_writable lives under /tinyplug/user: IndexedDB in a browser, so an editor's prefs and user
// presets survive a reload. Loaded before the controller exists, since an editor may read them as it starts.
const persist = "/tinyplug/user";
const mount = async (m) => {
    if (typeof indexedDB === "undefined" || !m.IDBFS) return null; // Node: plain memory.
    m.FS.mkdirTree(persist);
    m.FS.mount(m.IDBFS, { autoPersist: false }, persist);
    await new Promise((resolve) => m.FS.syncfs(true, resolve));
    let syncing = false;
    const flush = () => {
        if (syncing) return;
        syncing = true;
        m.FS.syncfs(false, () => { syncing = false; });
    };
    const timer = setInterval(flush, 2000);
    addEventListener("pagehide", flush);
    document.addEventListener("visibilitychange", () => { if (document.visibilityState === "hidden") flush(); });
    return { flush, stop: () => clearInterval(timer) };
};

export const load_controller = async (url) => {
    console.debug("[tinyplug] load: controller module");
    const m = await (await import(url)).default();
    console.debug("[tinyplug] load: storage (IndexedDB)");
    const storage = await mount(m);
    const table_at = m._tiny_ctl_create();
    const string = (ptr) => m.UTF8ToString(ptr);
    const with_string = (text, f) => { const p = m.stringToNewUTF8(text); try { return f(p); } finally { m._free(p); } };
    const manifest = JSON.parse(string(m._tiny_ctl_manifest()));
    return {
        module: m,
        storage,
        manifest,
        table: read_controller_table(m.HEAPU8.buffer, table_at),
        buffer: () => m.HEAPU8.buffer,
        start: (sample_rate) => m._tiny_ctl_start(sample_rate),
        tick: () => m._tiny_ctl_tick(),
        plain_params: () => {
            const n = manifest.params.length, p = m._malloc(8 * Math.max(n, 1));
            m._tiny_ctl_plain_params(p);
            const out = Array.from(new Float64Array(m.HEAPU8.buffer, p, n));
            m._free(p);
            return out;
        },
        set_param: (address, plain) => m._tiny_ctl_set_param(address, plain),
        param_knob: (address) => m._tiny_ctl_param_knob(address),
        send: ([kind, payload]) => {
            const p = m._malloc(Math.max(payload.length, 1));
            m.HEAPU8.set(payload, p);
            const ok = m._tiny_ctl_send(kind, p, payload.length);
            m._free(p);
            return ok;
        },
        action: (kind, address, knob = 0) => m._tiny_ctl_action({ start: 0, set: 1, end: 2 }[kind], address, knob),
        undo: () => m._tiny_ctl_undo(),
        redo: () => m._tiny_ctl_redo(),
        meters: () => Array.from(new Float32Array(m.HEAPU8.buffer, m._tiny_ctl_meters(), manifest.num_meters)),
        latency: () => m._tiny_ctl_latency(),
        // MIDI the processor sent since the last take: [{ frame, bytes }], frame within its quantum.
        take_midi: () => {
            const max = 256, p = m._malloc(16 * max);
            const n = m._tiny_ctl_take_midi(p, max);
            const v = new DataView(m.HEAPU8.buffer, p, 16 * n), out = [];
            for (let i = 0; i < n; ++i) out.push({ frame: v.getInt32(16 * i, true), bytes: Array.from(new Uint8Array(m.HEAPU8.buffer, p + 16 * i + 8, v.getUint32(16 * i + 4, true))) });
            m._free(p);
            return out;
        },
        open_view: (selector) => with_string(selector, (p) => { const v = m._tiny_ctl_open_view(p) >>> 0; return { w: v >>> 16, h: v & 0xffff }; }),
        close_view: () => m._tiny_ctl_close_view(),
        pointer: (kind, x, y, button, tag, mods) => m._tiny_web_pointer(kind, x, y, button, tag, mods),
        wheel: (dx, dy, precise, mods) => m._tiny_web_wheel(dx, dy, precise, mods),
        save_state: () => string(m._tiny_ctl_save_state()),
        load_state: (text) => with_string(text, (p) => m._tiny_ctl_load_state(p)),
    };
};
