// Page logic of the WebGPU port: rendering and the interactive run. The scripted tasks
// (?task=verify | rerun | bench | floats | gallery | speed | all; web/tasks.js) run in a worker
// (worker.js) and only report back here; with &thread=main they run on the page.
//
// The interactive run takes any parameter of the model in the address: p.<name>=<value>, for
// example ?p.lightAmount=0.7&p.lightDirX=1&p.morphogens=1&p.morphBulge=0.6, and exact=1 or
// exact=0 for the arithmetic (web/sim.js, `exact`; left out, the simulation chooses).
import { Sim } from './sim.js';
import { render as renderShader } from './shaders.js';
import { fetchState, requestDevice, runTask } from './tasks.js';

const logEl = document.getElementById('log');
const statusEl = document.getElementById('status');
const canvas = document.getElementById('view');
const query = new URLSearchParams(location.search);
const FAIL = ['', 'non-finite position', 'grid too large', 'non-finite new position', 'capacity reached'];

function log(line) {
  console.log(line);
  logEl.textContent += line + '\n';
}

// ------------------------------------------------------------------------------- rendering
function perspective(fovy, aspect, near, far) {
  const f = 1 / Math.tan(fovy / 2);
  return new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, far / (near - far), -1, 0, 0, (near * far) / (near - far), 0]);
}
function lookAt(eye, target) {
  const sub = (a, b) => [a[0] - b[0], a[1] - b[1], a[2] - b[2]];
  const norm = (a) => { const l = Math.hypot(...a) || 1; return a.map((x) => x / l); };
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const fw = norm(sub(target, eye)), s = norm(cross(fw, [0, 1, 0])), u = cross(s, fw);
  return new Float32Array([s[0], u[0], -fw[0], 0, s[1], u[1], -fw[1], 0, s[2], u[2], -fw[2], 0, -dot(s, eye), -dot(u, eye), dot(fw, eye), 1]);
}

class Renderer {
  constructor(device, context, format) {
    this.device = device;
    this.context = context;
    const module = device.createShaderModule({ code: renderShader });
    this.pipeline = device.createRenderPipeline({
      layout: 'auto',
      vertex: {
        module, entryPoint: 'vs',
        buffers: [
          { arrayStride: 16, stepMode: 'instance', attributes: [{ shaderLocation: 0, offset: 0, format: 'float32x4' }] },
          { arrayStride: 64, stepMode: 'instance', attributes: [{ shaderLocation: 1, offset: 4, format: 'uint32' }] },
        ],
      },
      fragment: { module, entryPoint: 'fs', targets: [{ format }] },
      primitive: { topology: 'triangle-strip' },
      depthStencil: { format: 'depth24plus', depthWriteEnabled: true, depthCompare: 'less' },
    });
    this.camera = device.createBuffer({ size: 208, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.group = device.createBindGroup({ layout: this.pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: this.camera } }] });
    this.depth = null;
    this.angle = 0.6;
  }
  draw(sim, colorByAge) {
    const g = sim.known;
    if (!g || !g.n) return;
    const w = canvas.width, h = canvas.height;
    if (!this.depth || this.depth.width !== w || this.depth.height !== h)
      this.depth = this.device.createTexture({ size: [w, h], format: 'depth24plus', usage: GPUTextureUsage.RENDER_ATTACHMENT });
    const lo = g.boundsMin ?? [-5, -5, -5], hi = g.boundsMax ?? [5, 5, 5];
    const centre = [0, 1, 2].map((i) => (lo[i] + hi[i]) / 2);
    const radius = Math.max(0.5 * Math.hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]), 2);
    const fovy = 0.6, dist = (radius / Math.sin(fovy / 2)) * 1.05;
    this.angle += 0.003;
    const eye = [centre[0] + dist * Math.cos(0.35) * Math.sin(this.angle), centre[1] + dist * Math.sin(0.35), centre[2] + dist * Math.cos(0.35) * Math.cos(this.angle)];
    const view = lookAt(eye, centre), proj = perspective(fovy, w / h, Math.max(dist - 2 * radius, dist * 0.01), dist + 2 * radius);
    const data = new Float32Array(52);
    data.set(view, 16);
    data.set(proj, 32);
    const p = sim.params;
    const ageScale = (0.5 * p.foodThreshold) / Math.max(0.5 * (p.foodMin + p.foodMax), 1e-9);
    data.set([0.55, Number(g.step), ageScale, colorByAge ? 1 : 0], 48);
    this.device.queue.writeBuffer(this.camera, 0, data);
    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginRenderPass({
      colorAttachments: [{ view: this.context.getCurrentTexture().createView(), clearValue: { r: 0.11, g: 0.12, b: 0.15, a: 1 }, loadOp: 'clear', storeOp: 'store' }],
      depthStencilAttachment: { view: this.depth.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'store' },
    });
    pass.setPipeline(this.pipeline);
    pass.setBindGroup(0, this.group);
    pass.setVertexBuffer(0, sim.buf.geom[sim.parity]);
    pass.setVertexBuffer(1, sim.buf.topo);
    pass.draw(4, g.n);
    pass.end();
    this.device.queue.submit([encoder.finish()]);
  }
}

// ------------------------------------------------------------------------------- interactive
async function interactive(device, renderer) {
  const start = await fetchState(query.get('start') ?? '/runs/web_verify/start.cfs');
  const capacity = Number(query.get('capacity') ?? 1200000);
  start.params.maxCells = capacity;
  for (const [k, v] of query) if (k.startsWith('p.')) start.params[k.slice(2)] = Number(v);
  const sim = await Sim.create(device, capacity, { exact: query.get('exact') === '1' ? true : query.get('exact') === '0' ? false : 'auto' });
  sim.upload(start);   // throws, with the reasons, when the parameters are not valid
  window.__sim = sim;
  let running = true, busy = false, batch = 4;
  document.getElementById('run').onclick = (e) => { running = !running; e.target.textContent = running ? 'Pause' : 'Run'; };
  document.getElementById('reset').onclick = () => { if (!busy) sim.upload({ ...start, params: sim.params }); };
  for (const name of ['springFactor', 'planarFactor', 'bulgeFactor', 'repulsionStrength', 'radiusOfInfluence', 'foodMax']) {
    const el = document.getElementById(name);
    el.value = sim.params[name];
    document.getElementById(name + 'Value').textContent = Number(el.value).toFixed(4).replace(/0+$/, '').replace(/\.$/, '');
    el.oninput = () => {
      sim.setParams({ ...sim.params, [name]: Number(el.value) });   // applies between steps, while it runs
      document.getElementById(name + 'Value').textContent = el.value;
    };
  }
  const stopAt = Number(query.get('stopcells') ?? 0);
  const frame = async () => {
    if (running && !busy && !sim.known.failed && !(stopAt && sim.known.n >= stopAt)) {
      busy = true;
      const t = performance.now();
      sim.step(batch);
      const g = await sim.readGlobals();
      const ms = (performance.now() - t) / batch;
      batch = Math.max(1, Math.min(200, Math.round(12 / Math.max(ms, 0.05))));
      statusEl.textContent = `${g.n} cells, step ${g.step}, ${ms.toFixed(2)} ms/step` + (sim.exact ? ', exact' : '') + (g.failed ? ` (stopped: ${FAIL[g.failCode]})` : '');
      busy = false;
    }
    renderer.draw(sim, document.getElementById('age').checked);
    requestAnimationFrame(frame);
  };
  requestAnimationFrame(frame);
}

async function main() {
  const task = query.get('task');
  if (task && query.get('thread') !== 'main') {
    // Scripted tasks run in a worker, so a hidden page does not slow them down.
    const worker = new Worker('worker.js', { type: 'module' });
    worker.onmessage = (e) => {
      if (e.data.log) log(e.data.log);
      if (e.data.status) statusEl.textContent = e.data.status;
      if (e.data.done) {
        document.title = e.data.error ? 'cf-webgpu: error' : `cf-webgpu: ${e.data.ok ? 'finished ok' : 'finished with failures'}`;
        log(e.data.ok ? 'ALL OK' : 'FAILURES');
      }
    };
    worker.onerror = (e) => { log('worker error: ' + e.message); document.title = 'cf-webgpu: error'; };
    worker.postMessage({ search: location.search });
    return;
  }
  let device;
  try {
    const got = await requestDevice();
    device = got.device;
    log(got.describe);
  } catch (e) {
    log(String(e.message ?? e));
    document.title = 'cf-webgpu: unavailable';
    return;
  }
  device.addEventListener('uncapturederror', (e) => log('WebGPU error: ' + e.error.message));
  const context = canvas.getContext('webgpu');
  const format = navigator.gpu.getPreferredCanvasFormat();
  context.configure({ device, format, alphaMode: 'opaque' });
  const renderer = new Renderer(device, context, format);
  try {
    if (task) {
      const ok = await runTask(device, query, log, {
        onProgress: async (sim, g, ms) => {
          statusEl.textContent = `${g.n} cells, step ${g.step}, ${ms.toFixed(2)} ms/step`;
          if (query.get('draw') !== '0') renderer.draw(sim, true);
          await new Promise((r) => setTimeout(r, 0));
        },
        onFinished: (sim) => { renderer.draw(sim, false); window.__sim = sim; },
      });
      document.title = `cf-webgpu: ${ok ? 'finished ok' : 'finished with failures'}`;
      log(ok ? 'ALL OK' : 'FAILURES');
    } else {
      await interactive(device, renderer);
    }
  } catch (e) {
    log('error: ' + (e.stack ?? e.message ?? e));
    document.title = 'cf-webgpu: error';
  }
}

main();
