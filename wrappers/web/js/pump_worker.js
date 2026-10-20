// The browser's pump thread: a dedicated module worker running pump.js.
import { run_pump } from "./pump.js";

self.onmessage = ({ data }) => {
    self.postMessage({ started: true });
    run_pump(data.spec, data.stop);
};
