// The pump: moves frames between the processor module's memory and the controller's, on its own thread, so the wire
// keeps moving in a hidden tab (rAF stops; a dedicated worker's wait doesn't). It holds the two memories and nothing
// else, as VST3's message shuttle holds nothing but the two components' message queues.
import { Ring, Store, pump_ring, pump_store } from "./wire.js";
import { Kind } from "./processor.js";

// The controller's table (source/controller/controller.cpp, Controller_table).
export const read_controller_table = (buffer, at) => {
    const t = new Uint32Array(buffer, at, 17);
    const entry = (i) => ({ offset: t[i], frame_bytes: t[i + 1], slot_offset: t[i + 2] });
    return {
        to_processor: t[1], from_worker: t[2], from_processor: t[3], to_worker: t[4], ring_capacity: t[5],
        blocks: t[6], num_blocks: t[7], state_edit: entry(8), state_snapshot: entry(11), state_load: entry(14),
    };
};

const stores = (buffer, entries) => entries.map((e) => (e.offset ? new Store(buffer, e.offset, e.frame_bytes, e.slot_offset) : null));
const block_entries = (buffer, at, count) => {
    const t = new Uint32Array(buffer, at, 3 * count);
    return Array.from({ length: count }, (_, i) => ({ offset: t[3 * i], frame_bytes: t[3 * i + 1], slot_offset: t[3 * i + 2] }));
};

// `processor` and `controller`: { buffer, table } for each module's memory. Returns one pass of the pump.
export const make_pump = ({ processor, controller }) => {
    const p = processor.table, c = controller.table;
    const ring = (side, at, capacity) => new Ring(side.buffer, at, capacity);

    const proc_in = ring(processor, p.inbound, p.inbound_capacity);
    const proc_out = ring(processor, p.outbound, p.outbound_capacity);
    const to_processor = [ring(controller, c.to_processor, c.ring_capacity), ring(controller, c.from_worker, c.ring_capacity)];
    const to_main = ring(controller, c.from_processor, c.ring_capacity);
    const to_worker = ring(controller, c.to_worker, c.ring_capacity);

    // Processor -> controller, routed by kind so each controller ring keeps one reader.
    const route = (kind) => (kind === Kind.Worker_message ? to_worker : to_main);
    const split = () => {
        let moved = 0;
        proc_out.drain((kind, payload) => {
            if (!route(kind).push(kind, payload)) return false;
            ++moved;
        });
        return moved;
    };

    const pairs = [];
    const proc_blocks = stores(processor.buffer, block_entries(processor.buffer, p.blocks, p.num_blocks));
    const ctl_blocks = stores(controller.buffer, block_entries(controller.buffer, c.blocks, c.num_blocks));
    proc_blocks.forEach((from, i) => pairs.push([from, ctl_blocks[i], {}]));
    const [proc_snapshot, ctl_snapshot] = [stores(processor.buffer, [p.state_snapshot])[0], stores(controller.buffer, [c.state_snapshot])[0]];
    const [ctl_edit, proc_edit] = [stores(controller.buffer, [c.state_edit])[0], stores(processor.buffer, [p.state_edit])[0]];
    if (proc_snapshot && ctl_snapshot) pairs.push([proc_snapshot, ctl_snapshot, {}]);
    const [ctl_load, proc_load] = [stores(controller.buffer, [c.state_load])[0], stores(processor.buffer, [p.state_load])[0]];
    if (ctl_load && proc_load) pairs.push([ctl_load, proc_load, {}]); // Before the edit: an edit after a load applies to it.
    if (ctl_edit && proc_edit) pairs.push([ctl_edit, proc_edit, {}]);

    return () => {
        let moved = split();
        for (const r of to_processor) moved += pump_ring(r, proc_in);
        for (const [from, to, state] of pairs) if (pump_store(from, to, state)) ++moved;
        return moved;
    };
};

// Runs the pump until `stop[0]` is set, sleeping briefly when there was nothing to move.
export const run_pump = (spec, stop, idle_ms = 1) => {
    const step = make_pump(spec);
    const sleeper = new Int32Array(new SharedArrayBuffer(4));
    while (Atomics.load(stop, 0) === 0) {
        if (step() === 0) Atomics.wait(sleeper, 0, 0, idle_ms);
    }
};
