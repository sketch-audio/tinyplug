// The JS end of tiny_core's wire (<tiny_core/wire.hpp>): the same Byte_ring and Block_store layouts, reached by
// byte offset into a module's shared memory. The pump uses it to move frames between two modules' memories.

const align = 8;
const header_bytes = 8;
const align_up = (n) => (n + align - 1) & ~(align - 1);

// Byte_ring<Capacity>: [write_pos u64][read_pos u64][data, Capacity bytes]. Positions are free-running byte counts.
export class Ring {
    constructor(buffer, offset, capacity) {
        if (offset % 8 !== 0) throw new Error("Ring offset must be 8-aligned.");
        if (capacity & (capacity - 1)) throw new Error("Ring capacity must be a power of two.");
        this.capacity = capacity;
        this.mask = BigInt(capacity - 1);
        this.positions = new BigUint64Array(buffer, offset, 2);
        this.data = new Uint8Array(buffer, offset + 16, capacity);
        this.header = new DataView(new ArrayBuffer(header_bytes));
        this.header_bytes = new Uint8Array(this.header.buffer);
    }

    // Producer: false (and nothing written) when the entry doesn't fit; the producer never overwrites unread data.
    push(kind, payload) {
        const total = header_bytes + align_up(payload.length);
        const w = Atomics.load(this.positions, 0);
        const r = Atomics.load(this.positions, 1);
        if (Number(w - r) + total > this.capacity) return false;

        this.header.setUint32(0, kind, true);
        this.header.setUint32(4, payload.length, true);
        this.#write(w, this.header_bytes);
        if (payload.length > 0) this.#write(w + BigInt(header_bytes), payload);
        Atomics.store(this.positions, 0, w + BigInt(total));
        return true;
    }

    // Consumer: `fn(kind, payload)` per entry while it returns anything but false; the payload view is only valid
    // inside the call. Reclaims what was consumed.
    drain(fn) {
        const w = Atomics.load(this.positions, 0);
        let r = Atomics.load(this.positions, 1);
        while (r < w) {
            this.#read(r, this.header_bytes);
            const kind = this.header.getUint32(0, true);
            const bytes = this.header.getUint32(4, true);
            const total = header_bytes + align_up(bytes);
            if (bytes > this.capacity - header_bytes) { r = w; break; } // Malformed: can't resync, reclaim all.

            const payload = new Uint8Array(bytes);
            this.#read(r + BigInt(header_bytes), payload);
            if (fn(kind, payload) === false) break;
            r += BigInt(total);
        }
        Atomics.store(this.positions, 1, r);
    }

    #write(pos, src) {
        const start = Number(pos & this.mask);
        const first = Math.min(src.length, this.capacity - start);
        this.data.set(src.subarray(0, first), start);
        if (first < src.length) this.data.set(src.subarray(first), 0);
    }

    #read(pos, dst) {
        const start = Number(pos & this.mask);
        const first = Math.min(dst.length, this.capacity - start);
        dst.set(this.data.subarray(start, start + first), 0);
        if (first < dst.length) dst.set(this.data.subarray(0, dst.length - first), first);
    }
}

// Moves whole entries from one module's ring to another's until the source is empty or the destination is full.
// A full destination leaves the rest in the source: backpressure, never a drop the producer didn't decide.
export const pump_ring = (from, to) => {
    let moved = 0;
    from.drain((kind, payload) => {
        if (!to.push(kind, payload)) return false;
        ++moved;
    });
    return moved;
};

// Block_store<Frame>: [seq u64][two frame slots, each frame_bytes, from slot_offset]. seq 0 means never published.
// The slot copy is per-word Atomics, as the C++'s: a plain copy is unordered against seq, so a torn frame could pass.
export class Store {
    constructor(buffer, offset, frame_bytes, slot_offset = 8) {
        if (offset % 8 !== 0) throw new Error("Store offset must be 8-aligned.");
        this.frame_bytes = frame_bytes;
        this.seq = new BigUint64Array(buffer, offset, 1);
        const W = frame_bytes % 4 === 0 ? Uint32Array : Uint8Array; // As Block_store::Word.
        this.words = new W(buffer, offset + slot_offset, (2 * frame_bytes) / W.BYTES_PER_ELEMENT);
        this.per_slot = frame_bytes / W.BYTES_PER_ELEMENT;
        this.scratch = new W(this.per_slot);
    }

    // Reader: the latest frame into `into` (bytes), and its seq; 0 when nothing was published or the copy tore.
    read(into) {
        const s = Atomics.load(this.seq, 0);
        if (s === 0n) return 0n;
        const at = Number((s - 1n) & 1n) * this.per_slot;
        for (let i = 0; i < this.per_slot; ++i) this.scratch[i] = Atomics.load(this.words, at + i);
        if (Atomics.load(this.seq, 0) !== s) return 0n;
        into.set(new Uint8Array(this.scratch.buffer, 0, this.frame_bytes));
        return s;
    }

    // Producer: fill the back slot, then bump seq.
    publish(frame) {
        const s = Atomics.load(this.seq, 0);
        const at = Number(s & 1n) * this.per_slot;
        new Uint8Array(this.scratch.buffer, 0, this.frame_bytes).set(frame.subarray(0, this.frame_bytes));
        for (let i = 0; i < this.per_slot; ++i) Atomics.store(this.words, at + i, this.scratch[i]);
        Atomics.store(this.seq, 0, s + 1n);
    }
}

// Copies a newer frame from one module's store to another's. `state` remembers what was last copied.
export const pump_store = (from, to, state) => {
    state.scratch ??= new Uint8Array(from.frame_bytes);
    state.seen ??= 0n;
    const s = from.read(state.scratch);
    if (s === 0n || s === state.seen) return false;
    to.publish(state.scratch);
    state.seen = s;
    return true;
};
