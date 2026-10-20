// web_split_host.mjs's pump thread: pump.js with the two memories.
import { workerData } from "node:worker_threads";
import { run_pump } from "../../wrappers/web/js/pump.js";

run_pump(workerData.spec, workerData.stop);
