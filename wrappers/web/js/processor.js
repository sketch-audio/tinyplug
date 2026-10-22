// The processor module's JS end, shared by the AudioWorklet and the Node host: instantiation with our own imports
// (no Emscripten JS), the module's buffers and rings, and the channel encodings of source/wire_layout.hpp.
import { Ring, Store } from "./wire.js";

export const memory_pages = 512; // -sINITIAL_MEMORY=32MB = -sMAXIMUM_MEMORY in make_web_plugin.cmake.

// Fixed-size: growing would leave the controller's views at the old length. Shared for the isolated variant, which
// the pump maps; the nonisolated module imports a plain memory only the worklet sees.
export const create_memory = (shared = true) => new WebAssembly.Memory({ initial: memory_pages, maximum: memory_pages, shared });

export const Kind = Object.freeze({
    Param_set: 0, Param_ramp: 1, Bypass: 2, Host_reset: 3, Transport: 4, Midi: 5, Editor_note: 6, Worker_reply: 7,
    Meter: 64, Latency: 65, Midi_out: 66, Worker_message: 67,
});

// AudioWorkletGlobalScope has no TextDecoder (Chrome) and may have no `performance`.
const utf8 = (bytes) => {
    if (typeof TextDecoder !== "undefined") return new TextDecoder().decode(bytes);
    let out = "";
    for (const b of bytes) out += String.fromCharCode(b);
    return decodeURIComponent(escape(out));
};
const now_ns = () => BigInt(Math.round((globalThis.performance?.now() ?? Date.now()) * 1e6));

const imports = (memory, log) => {
    const view = () => new DataView(memory.buffer);
    let line = "";
    return {
        env: { memory, __do_set_thread_state: () => {} },
        wasi_snapshot_preview1: {
            clock_time_get: (_id, _precision, out) => {
                view().setBigUint64(out, now_ns(), true);
                return 0;
            },
            fd_write: (_fd, iovs, count, written) => { // stdout / stderr to `log`, a line at a time.
                const v = view();
                let total = 0;
                for (let i = 0; i < count; ++i) {
                    const ptr = v.getUint32(iovs + 8 * i, true), len = v.getUint32(iovs + 8 * i + 4, true);
                    line += utf8(new Uint8Array(memory.buffer, ptr, len).slice());
                    total += len;
                }
                for (let nl; (nl = line.indexOf("\n")) >= 0; line = line.slice(nl + 1)) log(line.slice(0, nl));
                v.setUint32(written, total, true);
                return 0;
            },
            fd_close: () => 0,
            fd_seek: () => 0,
        },
    };
};

// `module` is a compiled WebAssembly.Module (posted to a worklet, or compiled from bytes in Node).
export const create_processor = (module, memory, { log = console.log } = {}) => {
    const { exports } = new WebAssembly.Instance(module, imports(memory, log));
    exports.emscripten_stack_init?.();
    exports._initialize();

    const table_at = exports.tiny_web_create();
    const t = new Uint32Array(memory.buffer, table_at, 23);
    const table = {
        version: t[0], inbound: t[1], inbound_capacity: t[2], outbound: t[3], outbound_capacity: t[4],
        inputs: t[5], sidechain: t[6], outputs: t[7], max_channels: t[8], max_frames: t[9], params: t[10], num_params: t[11],
        blocks: t[12], num_blocks: t[13],
        state_edit: { offset: t[14], frame_bytes: t[15], slot_offset: t[16] },
        state_snapshot: { offset: t[17], frame_bytes: t[18], slot_offset: t[19] },
        state_load: { offset: t[20], frame_bytes: t[21], slot_offset: t[22] },
    };
    const entries = new Uint32Array(memory.buffer, table.blocks, 3 * table.num_blocks);
    const channels = (at) => Array.from({ length: table.max_channels },
        (_, ch) => new Float32Array(memory.buffer, at + ch * table.max_frames * 4, table.max_frames));

    return {
        table,
        inputs: channels(table.inputs),
        sidechain: channels(table.sidechain),
        outputs: channels(table.outputs),
        params: new Float64Array(memory.buffer, table.params, table.num_params), // Plain: what `configure` comes up holding.
        inbound: new Ring(memory.buffer, table.inbound, table.inbound_capacity),
        outbound: new Ring(memory.buffer, table.outbound, table.outbound_capacity),
        blocks: Array.from({ length: table.num_blocks }, (_, i) => new Store(memory.buffer, entries[3 * i], entries[3 * i + 1], entries[3 * i + 2])),
        configure: (sample_rate) => exports.tiny_web_configure(sample_rate),
        process: (frames) => exports.tiny_web_process(frames),
        manifest: () => {
            const at = exports.tiny_web_manifest();
            const bytes = new Uint8Array(memory.buffer, at, memory.buffer.byteLength - at);
            return JSON.parse(utf8(bytes.slice(0, bytes.indexOf(0))));
        },
    };
};

// For tools and tests holding a module of either variant: the memory is whichever it imports. (Shared needs a
// SharedArrayBuffer, which Node always has.)
export const create_processor_any = (module, options) => {
    try {
        return create_processor(module, create_memory(true), options);
    }
    catch (e) {
        if (!(e instanceof WebAssembly.LinkError)) throw e;
        return create_processor(module, create_memory(false), options);
    }
};

// MARK: - messages, as source/wire_layout.hpp lays them out

const payload = (bytes, fill) => {
    const buffer = new ArrayBuffer(bytes);
    fill(new DataView(buffer));
    return new Uint8Array(buffer);
};

export const encode = {
    set: (address, value, offset = 0) => [Kind.Param_set, payload(16, (v) => {
        v.setFloat64(0, value, true); v.setUint32(8, address, true); v.setInt32(12, offset, true);
    })],
    ramp: (address, value, samples, offset = 0) => [Kind.Param_ramp, payload(24, (v) => {
        v.setFloat64(0, value, true); v.setUint32(8, address, true); v.setInt32(12, samples, true); v.setInt32(16, offset, true);
    })],
    bypass: (on) => [Kind.Bypass, payload(8, (v) => v.setUint32(0, on ? 1 : 0, true))],
    host_reset: () => [Kind.Host_reset, new Uint8Array(0)],
    midi: (status, data1 = 0, data2 = 0, offset = 0) => [Kind.Midi, payload(8, (v) => {
        v.setUint8(0, status); v.setUint8(1, data1); v.setUint8(2, data2); v.setInt32(4, offset, true);
    })],
    worker_reply: (bytes) => [Kind.Worker_reply, bytes], // User_work::To_processor, laid out by the controller.
    transport: ({ tempo = 0, position = 0, anchor = 0, numer = 0, denom = 0, playing = false }) => [Kind.Transport, payload(40, (v) => {
        v.setFloat64(0, tempo, true); v.setBigInt64(8, BigInt(position), true); v.setBigInt64(16, BigInt(anchor), true);
        v.setInt32(24, numer, true); v.setInt32(28, denom, true); v.setUint32(32, playing ? 1 : 0, true);
    })],
};

export const decode = (kind, bytes) => {
    const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    switch (kind) {
        case Kind.Meter: return { kind: "meter", address: v.getUint32(0, true), value: v.getFloat32(4, true) };
        case Kind.Latency: return { kind: "latency", samples: v.getUint32(0, true) };
        case Kind.Midi_out: return { kind: "midi", frame: v.getInt32(0, true), bytes: Array.from(bytes.subarray(8, 8 + v.getUint32(4, true))) };
        case Kind.Worker_message: return { kind: "worker", bytes: bytes.slice() }; // User_work::From_processor.
        default: return { kind: "unknown", id: kind };
    }
};
