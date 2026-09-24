# Platform Demo

A quick check of the platform layer: input, gestures, dialogs and what the window reports.

- **The pad** is watched by every gesture recognizer (over, down, dwell, left click, double
  click, right click, drag). Each has a lamp that flashes on its callbacks and a count; a drag
  draws across the pad. Click the lamps to zero the counts.
- **Raw input**: the last pointer events with their pointer tags, the modifier keys, and the
  accumulated scroll (lit while inertial, P for a precise device).
- **Dialogs**: one button per `Platform_dialogs` call. Text sets Gain through a gesture;
  Chained asks for a second dialog from the first one's callback, which must queue rather than
  vanish. The last result shows at the top of the info panel.
- **Info**: format, graphics backend, scale, logical and pixel size, frame rate, held pointers.
