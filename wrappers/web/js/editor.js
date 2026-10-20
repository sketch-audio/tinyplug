// The editor on a page's <canvas>: opens the controller's view there and forwards the page's input to it, in CSS
// pixels relative to the canvas, which are the editor's logical pixels.
const mac = /Mac|iPhone|iPad/.test(globalThis.navigator?.platform ?? "");
const modifiers = (e) => ((mac ? e.metaKey : e.ctrlKey) ? 1 : 0) | (e.altKey ? 2 : 0) | (e.shiftKey ? 4 : 0);

let next_id = 0;

export const attach_editor = (ctl, canvas) => {
    if (!canvas.id) canvas.id = `tinyplug-editor-${next_id++}`;
    canvas.style.touchAction = "none"; // Drags are the editor's, not the page's scroll.
    const size = ctl.open_view(`#${canvas.id}`);

    const pointer = (kind) => (e) => {
        if (kind === 0) canvas.setPointerCapture(e.pointerId);
        ctl.pointer(kind, e.offsetX, e.offsetY, e.button === 2 ? 2 : 0, e.pointerId, modifiers(e));
    };
    const listeners = {
        pointerdown: pointer(0), pointerup: pointer(1), pointermove: pointer(2),
        pointerenter: pointer(3), pointerleave: pointer(4), pointercancel: pointer(5),
        contextmenu: (e) => e.preventDefault(),
        wheel: (e) => {
            e.preventDefault();
            const precise = e.deltaMode === 0; // Pixels: a trackpad, or a precise wheel.
            const scale = e.deltaMode === 2 ? 10 : 1; // Pages, roughly.
            ctl.wheel(-e.deltaX * scale, -e.deltaY * scale, precise ? 1 : 0, modifiers(e));
        },
    };
    for (const [name, f] of Object.entries(listeners)) canvas.addEventListener(name, f, { passive: name !== "wheel" });

    return {
        size,
        close: () => {
            for (const [name, f] of Object.entries(listeners)) canvas.removeEventListener(name, f);
            ctl.close_view();
        },
    };
};
