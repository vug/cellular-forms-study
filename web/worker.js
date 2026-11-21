// Runs a scripted task off the main thread and reports back through messages.
import { requestDevice, runTask } from './tasks.js';

self.onmessage = async (e) => {
  const log = (line) => self.postMessage({ log: line });
  try {
    const { device, describe } = await requestDevice();
    device.addEventListener('uncapturederror', (ev) => log('WebGPU error: ' + ev.error.message));
    log(describe);
    const query = new URLSearchParams(e.data.search);
    const ok = await runTask(device, query, log, {
      onProgress: (sim, g, ms) => self.postMessage({ status: `${g.n} cells, step ${g.step}, ${ms.toFixed(2)} ms/step` }),
    });
    self.postMessage({ done: true, ok });
  } catch (err) {
    log('error: ' + (err.stack ?? err.message ?? err));
    self.postMessage({ done: true, ok: false, error: true });
  }
};
