// The nonisolated variant's wire: the pump split in two halves joined by the worklet's MessagePort, for a page
// without COOP / COEP. Each half runs on the thread that owns its module's memory, takes what its module wrote,
// posts it, and lands what the other half posted. The rings and stores are the isolated variant's, unchanged.
//
// A message is { ring, stores }: `ring` a ring's unread bytes as Ring.take returns them (or null), `stores` a list of
// [key, frame] for each store with a newer frame. Keys: "block:<i>" and "snapshot" outwards, "load" and "edit" inwards.
import { Ring, each_entry } from "./wire.js";
import { Kind } from "./processor.js";
import { stores, block_entries } from "./pump.js";

const align_up = (n) => (n + 7) & ~7;

// [kind, payload] pairs as ring bytes, for a page with no ring of its own to send from.
export const pack = (entries) => {
    const out = new Uint8Array(entries.reduce((n, [, p]) => n + 8 + align_up(p.length), 0));
    const v = new DataView(out.buffer);
    let at = 0;
    for (const [kind, payload] of entries) {
        v.setUint32(at, kind, true);
        v.setUint32(at + 4, payload.length, true);
        out.set(payload, at + 8);
        at += 8 + align_up(payload.length);
    }
    return out;
};

// `[key, store, seen]` per store: the frames newer than the last taken.
const take_frames = (named) => {
    const out = [];
    for (const [key, store, seen] of named) {
        seen.scratch ??= new Uint8Array(store.frame_bytes);
        seen.seq ??= 0n;
        const s = store.read(seen.scratch);
        if (s === 0n || s === seen.seq) continue;
        seen.seq = s;
        out.push([key, seen.scratch.slice()]);
    }
    return out;
};

// A message and its transfer list, or null when there is nothing to send.
const message = (ring, frames) => {
    if (!ring && !frames.length) return null;
    return { msg: { ring, stores: frames }, transfer: [...(ring ? [ring.buffer] : []), ...frames.map(([, f]) => f.buffer)] };
};

const concat = (parts) => {
    const some = parts.filter(Boolean);
    if (some.length < 2) return some[0] ?? null;
    const out = new Uint8Array(some.reduce((n, p) => n + p.length, 0));
    some.reduce((at, p) => (out.set(p, at), at + p.length), 0);
    return out;
};

// Posted ring bytes waiting for room in a ring, oldest first. Past `limit` bytes a post is dropped whole, as a full
// ring refuses its producer: the receiving module isn't keeping up (a hidden tab's controller, say).
class Landing {
    constructor(limit) {
        this.limit = limit;
        this.pending = [];
        this.bytes = 0;
    }

    add(bytes) {
        if (this.bytes + bytes.length > this.limit) return;
        this.pending.push(bytes);
        this.bytes += bytes.length;
    }

    // `push(kind, payload) -> bool` per entry until one doesn't fit.
    flush(push) {
        while (this.pending.length) {
            const head = this.pending[0];
            const rest = each_entry(head, push);
            this.bytes -= head.length - rest.length;
            if (rest.length) { this.pending[0] = rest; return; }
            this.pending.shift();
        }
    }
}

// The worklet's half, on the processor module's memory: `{ buffer, table }` as the processor's create returns them.
export const processor_end = ({ buffer, table: p }) => {
    const inbound = new Ring(buffer, p.inbound, p.inbound_capacity);
    const outbound = new Ring(buffer, p.outbound, p.outbound_capacity);
    const out_stores = [
        ...stores(buffer, block_entries(buffer, p.blocks, p.num_blocks)).map((s, i) => [`block:${i}`, s, {}]),
        ["snapshot", stores(buffer, [p.state_snapshot])[0], {}],
    ].filter(([, s]) => s);
    const [load, edit] = stores(buffer, [p.state_load, p.state_edit]);
    const in_stores = { load, edit };
    const landing = new Landing(4 * p.inbound_capacity);

    return {
        land: (msg) => {
            if (msg.ring) landing.add(msg.ring);
            for (const [key, frame] of msg.stores) in_stores[key]?.publish(frame); // In order: a load before an edit.
        },
        // Before each quantum: what the page posted, into the inbound ring.
        flush: () => landing.flush((kind, payload) => inbound.push(kind, payload)),
        take: () => message(outbound.take(), take_frames(out_stores)),
    };
};

// The page's half, on the controller module's memory. `buffer` is a getter: growing a plain memory replaces its
// buffer, so the views are rebuilt whenever it changes.
export const controller_end = ({ buffer, table: c }) => {
    const seen = { load: {}, edit: {} }; // Survives a rebuild: the last frames sent.
    let views = null, mapped = null;
    const build = (b) => {
        const ring = (at) => new Ring(b, at, c.ring_capacity);
        const blocks = stores(b, block_entries(b, c.blocks, c.num_blocks));
        const [snapshot, load, edit] = stores(b, [c.state_snapshot, c.state_load, c.state_edit]);
        return {
            to_processor: [ring(c.to_processor), ring(c.from_worker)],
            to_main: ring(c.from_processor),
            to_worker: ring(c.to_worker),
            in_stores: Object.fromEntries([...blocks.map((s, i) => [`block:${i}`, s]), ["snapshot", snapshot]]),
            out_stores: [["load", load, seen.load], ["edit", edit, seen.edit]].filter(([, s]) => s),
        };
    };
    const at = () => {
        const b = buffer();
        if (b !== mapped) { mapped = b; views = build(b); }
        return views;
    };
    const landing = new Landing(4 * c.ring_capacity);
    const flush = () => {
        const v = at();
        landing.flush((kind, payload) => (kind === Kind.Worker_message ? v.to_worker : v.to_main).push(kind, payload));
    };

    return {
        land: (msg) => {
            if (msg.ring) landing.add(msg.ring);
            const v = at();
            for (const [key, frame] of msg.stores) v.in_stores[key]?.publish(frame);
            flush();
        },
        take: () => {
            flush(); // Room the controller made since the last post.
            const v = at();
            return message(concat(v.to_processor.map((r) => r.take())), take_frames(v.out_stores));
        },
    };
};
