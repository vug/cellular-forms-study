// WebGPU simulation driver (M9): owns the buffers and pipelines and encodes steps.
// The cell count and the step counter live on the GPU; the page reads them back when it wants
// to know (readGlobals), not once per step.
import * as S from './shaders.js';

const kMaxDeg = 12;
const GLOBALS_BYTES = 320;

function tableSizeFor(cells) {
  let t = 1024;
  while (t < cells / 2 && t < (1 << 30)) t <<= 1;
  return t;
}

// The storage bindings a shader declares: [{ binding, name, readOnly }].
function bindingsOf(code) {
  const out = [];
  const re = /@group\(0\)\s*@binding\((\d+)\)\s*var<storage,\s*(read|read_write)>\s*(\w+)\s*:/g;
  for (let m; (m = re.exec(code)); ) out.push({ binding: Number(m[1]), readOnly: m[2] === 'read', name: m[3] });
  return out;
}

export class Sim {
  static async create(device, capacity) {
    const sim = new Sim(device, capacity);
    await sim.#build();
    return sim;
  }

  constructor(device, capacity) {
    this.device = device;
    this.capacity = capacity;
    this.tableCap = tableSizeFor(capacity);
    this.parity = 0;          // which geom / normal buffer is live
    this.uploadStep = 0n;
    this.uploadParity = 0;
    this.allocated = 0;
  }

  #buffer(bytes, usage) {
    const size = Math.max(16, Math.ceil(bytes / 4) * 4);
    this.allocated += size;
    return this.device.createBuffer({ size, usage });
  }

  async #build() {
    const d = this.device;
    const c = this.capacity;
    const ST = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST;
    const scanEntries = Math.max(this.tableCap + 1, c) + 16;
    this.buf = {
      G: this.#buffer(GLOBALS_BYTES, ST),
      ind: this.#buffer(9 * 12, GPUBufferUsage.STORAGE | GPUBufferUsage.INDIRECT | GPUBufferUsage.COPY_DST),
      geom: [this.#buffer(c * 16, ST | GPUBufferUsage.VERTEX), this.#buffer(c * 16, ST | GPUBufferUsage.VERTEX)],
      nrm: [this.#buffer(c * 16, ST), this.#buffer(c * 16, ST)],
      topo: this.#buffer(c * 64, ST | GPUBufferUsage.VERTEX),
      sel: this.#buffer(c * 24, ST),
      scanData: this.#buffer(scanEntries * 4, ST),
      cellFill: this.#buffer(this.tableCap * 4, ST),
      sortedId: this.#buffer(c * 4, ST),
      sortedPos: this.#buffer(c * 16, ST),
      levels: this.#buffer((Math.ceil(scanEntries / 255) + 1024) * 4, ST),
    };
    this.readback = d.createBuffer({ size: GLOBALS_BYTES, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });

    // The passes of one step, in order: [name, WGSL, indirect slot (or -1 for a single dispatch)].
    const scanPasses = (which, first) => [
      [`scan${which}up0`, S.scan(which, 'up', 0), first],
      [`scan${which}up1`, S.scan(which, 'up', 1), first + 1],
      [`scan${which}up2`, S.scan(which, 'up', 2), first + 2],
      [`scan${which}top`, S.scan(which, 'top', 3), 5],
      [`scan${which}down2`, S.scan(which, 'down', 2), first + 2],
      [`scan${which}down1`, S.scan(which, 'down', 1), first + 1],
      [`scan${which}down0`, S.scan(which, 'down', 0), first],
    ];
    const passes = [
      ['prepare', S.prepare(this.tableCap), -1],
      ['clear', S.clear, 1],
      ['pre', S.pre, 0],
      ['gridSetup', S.gridSetup, 5],
      ['gridCount', S.gridCount, 0],
      ...scanPasses(0, 2),
      ['gridFill', S.gridFill, 0],
      ['gridSort', S.gridSort, 1],
      ['move', S.move, 0],
      ['candidates', S.candidates, 0],
      ['maxRing0', S.maxRing(0), 0],
      ['maxRing1', S.maxRing(1), 0],
      ['winFlag', S.winFlag, 0],
      ...scanPasses(1, 6),
      ['split', S.split, 0],
      ['commit', S.commit, 5],
    ];

    this.passes = [];
    for (const [name, code, slot] of passes) {
      const module = d.createShaderModule({ code, label: name });
      const info = await module.getCompilationInfo();
      const errors = info.messages.filter((m) => m.type === 'error');
      if (errors.length) throw new Error(`shader ${name}: ` + errors.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('; '));
      const bindings = bindingsOf(code);
      const layout = d.createBindGroupLayout({
        entries: bindings.map((b) => ({ binding: b.binding, visibility: GPUShaderStage.COMPUTE, buffer: { type: b.readOnly ? 'read-only-storage' : 'storage' } })),
      });
      const pipeline = await d.createComputePipelineAsync({
        label: name,
        layout: d.createPipelineLayout({ bindGroupLayouts: [layout] }),
        compute: { module, entryPoint: 'main' },
      });
      // Two bind groups: "cur" names point at buffer `p`, "new" names at the other one.
      const groups = [0, 1].map((p) =>
        d.createBindGroup({
          layout,
          entries: bindings.map((b) => ({ binding: b.binding, resource: { buffer: this.#resolve(b.name, p) } })),
        }));
      this.passes.push({ name, pipeline, groups, slot });
    }
  }

  #resolve(name, p) {
    const b = this.buf;
    switch (name) {
      case 'geomCur': return b.geom[p];
      case 'geomNew': return b.geom[1 - p];
      case 'nrmCur': return b.nrm[p];
      case 'nrmNew': return b.nrm[1 - p];
      default:
        if (!(name in b)) throw new Error(`no buffer for binding '${name}'`);
        return b[name];
    }
  }

  // Loads a state (from parseCheckpoint). Parameters come from state.params.
  upload(state) {
    const n = state.n;
    if (n > this.capacity) throw new Error(`state has ${n} cells, capacity is ${this.capacity}`);
    const q = this.device.queue;
    const f = Math.fround;
    const geom = new Float32Array(n * 4), nrm = new Float32Array(n * 4), topo = new Uint32Array(n * 16);
    for (let v = 0; v < n; v++) {
      geom[4 * v] = state.pos[3 * v]; geom[4 * v + 1] = state.pos[3 * v + 1]; geom[4 * v + 2] = state.pos[3 * v + 2];
      geom[4 * v + 3] = state.food[v];
      nrm[4 * v] = state.normal[3 * v]; nrm[4 * v + 1] = state.normal[3 * v + 1]; nrm[4 * v + 2] = state.normal[3 * v + 2];
      topo[16 * v] = state.deg[v];
      topo[16 * v + 1] = state.birth[v];
      topo[16 * v + 2] = state.gen[v];
      topo[16 * v + 3] = 0xFF;
      for (let k = 0; k < kMaxDeg; k++) topo[16 * v + 4 + k] = state.nbr[kMaxDeg * v + k];
    }
    q.writeBuffer(this.buf.geom[0], 0, geom);
    q.writeBuffer(this.buf.nrm[0], 0, nrm);
    q.writeBuffer(this.buf.topo, 0, topo);

    const p = state.params;
    const l0 = f(p.linkRestLength);
    const g = new ArrayBuffer(GLOBALS_BYTES);
    const dv = new DataView(g);
    const e = f(f(f(1.0e-6) * l0) * l0);           // normal_eps2 in core/kernels.hpp, in single precision
    const floats = [l0, p.springFactor, p.planarFactor, p.bulgeFactor, p.repulsionStrength, p.radiusOfInfluence,
      f(f(p.maxDisplacement) * l0), p.foodMin, f(f(p.foodMax) - f(p.foodMin)), p.foodThreshold, p.daughterOffset,
      f(e * e), f(f(1.0e-12) * f(1.0e-12))];
    floats.forEach((x, i) => dv.setFloat32(4 * i, x, true));
    dv.setUint32(52, Number(state.seed & 0xFFFFFFFFn), true);
    dv.setUint32(56, Number(state.seed >> 32n), true);
    dv.setUint32(60, Math.min(p.maxCells ?? this.capacity, this.capacity), true);
    dv.setUint32(64, n, true);
    dv.setUint32(68, Number(state.step & 0xFFFFFFFFn), true);
    dv.setUint32(72, Number(state.step >> 32n), true);
    q.writeBuffer(this.buf.G, 0, g);

    this.params = { ...p };
    this.seed = state.seed;
    this.parity = 0;
    this.uploadParity = 0;
    this.uploadStep = state.step;
    this.known = { n, step: state.step, failed: 0 };
  }

  // Changes parameters between steps (floats only; the capacity stays).
  setParams(p) {
    const f = Math.fround;
    const l0 = f(p.linkRestLength);
    const e = f(f(f(1.0e-6) * l0) * l0);
    const floats = new Float32Array([l0, p.springFactor, p.planarFactor, p.bulgeFactor, p.repulsionStrength, p.radiusOfInfluence,
      f(f(p.maxDisplacement) * l0), p.foodMin, f(f(p.foodMax) - f(p.foodMin)), p.foodThreshold, p.daughterOffset,
      f(e * e), f(f(1.0e-12) * f(1.0e-12))]);
    this.device.queue.writeBuffer(this.buf.G, 0, floats);
    this.params = { ...p };
  }

  // Encodes and submits `count` steps in one command buffer.
  step(count = 1) {
    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    for (let i = 0; i < count; i++) {
      for (const p of this.passes) {
        pass.setPipeline(p.pipeline);
        pass.setBindGroup(0, p.groups[this.parity]);
        if (p.slot < 0) pass.dispatchWorkgroups(1);
        else pass.dispatchWorkgroupsIndirect(this.buf.ind, p.slot * 12);
      }
      this.parity ^= 1;
    }
    pass.end();
    this.device.queue.submit([encoder.finish()]);
  }

  // Reads the GPU-side counters. Resolves when everything submitted so far has run.
  async readGlobals() {
    const encoder = this.device.createCommandEncoder();
    encoder.copyBufferToBuffer(this.buf.G, 0, this.readback, 0, GLOBALS_BYTES);
    this.device.queue.submit([encoder.finish()]);
    await this.readback.mapAsync(GPUMapMode.READ);
    const dv = new DataView(this.readback.getMappedRange().slice(0));
    this.readback.unmap();
    const u = (o) => dv.getUint32(o, true);
    const ordered = (o) => {
      const i = dv.getInt32(o, true);
      const bits = i ^ ((i >> 31) & 0x7FFFFFFF);
      return new Float32Array(new Int32Array([bits]).buffer)[0];
    };
    const g = {
      n: u(64), step: (BigInt(u(72)) << 32n) | BigInt(u(68)), tableMask: u(76), failed: u(108),
      fallbacks: u(112), clamped: u(116), ready: u(120), postponed: u(124), winners: u(136),
      failStep: u(168), failCode: u(172),
      divisions: u(180) * 4294967296 + u(176), clampedTotal: u(188) * 4294967296 + u(184),
      boundsMin: [ordered(144), ordered(148), ordered(152)], boundsMax: [ordered(156), ordered(160), ordered(164)],
    };
    // After a failure the GPU stopped committing, so the live buffer is the one of the last good step.
    this.parity = (this.uploadParity + Number((g.step - this.uploadStep) & 1n)) & 1;
    this.known = g;
    return g;
  }

  // Copies the whole state back (for the checker and for comparisons).
  async download() {
    const g = await this.readGlobals();
    const n = g.n;
    const d = this.device;
    const mk = (bytes) => d.createBuffer({ size: Math.max(16, bytes), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const sg = mk(n * 16), sn = mk(n * 16), st = mk(n * 64);
    const encoder = d.createCommandEncoder();
    if (n > 0) {
      encoder.copyBufferToBuffer(this.buf.geom[this.parity], 0, sg, 0, n * 16);
      encoder.copyBufferToBuffer(this.buf.nrm[this.parity], 0, sn, 0, n * 16);
      encoder.copyBufferToBuffer(this.buf.topo, 0, st, 0, n * 64);
    }
    d.queue.submit([encoder.finish()]);
    await Promise.all([sg.mapAsync(GPUMapMode.READ), sn.mapAsync(GPUMapMode.READ), st.mapAsync(GPUMapMode.READ)]);
    const geom = new Float32Array(sg.getMappedRange().slice(0));
    const nrm = new Float32Array(sn.getMappedRange().slice(0));
    const topo = new Uint32Array(st.getMappedRange().slice(0));
    for (const b of [sg, sn, st]) { b.unmap(); b.destroy(); }
    const s = {
      n, step: g.step, seed: this.seed, params: { ...this.params },
      pos: new Float32Array(3 * n), normal: new Float32Array(3 * n), food: new Float32Array(n),
      birth: new Uint32Array(n), gen: new Uint32Array(n), deg: new Uint8Array(n), nbr: new Uint32Array(kMaxDeg * n),
    };
    for (let v = 0; v < n; v++) {
      s.pos[3 * v] = geom[4 * v]; s.pos[3 * v + 1] = geom[4 * v + 1]; s.pos[3 * v + 2] = geom[4 * v + 2];
      s.food[v] = geom[4 * v + 3];
      s.normal[3 * v] = nrm[4 * v]; s.normal[3 * v + 1] = nrm[4 * v + 1]; s.normal[3 * v + 2] = nrm[4 * v + 2];
      s.deg[v] = topo[16 * v];
      s.birth[v] = topo[16 * v + 1];
      s.gen[v] = topo[16 * v + 2];
      for (let k = 0; k < kMaxDeg; k++) s.nbr[kMaxDeg * v + k] = topo[16 * v + 4 + k];
    }
    return s;
  }

  destroy() {
    const b = this.buf;
    for (const x of [b.G, b.ind, ...b.geom, ...b.nrm, b.topo, b.sel, b.scanData, b.cellFill, b.sortedId, b.sortedPos, b.levels, this.readback]) x.destroy();
  }
}
