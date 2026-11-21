// WebGPU simulation driver (M9): owns the buffers and pipelines and encodes steps.
// The cell count and the step counter live on the GPU; the page reads them back when it wants
// to know (readGlobals), not once per step.
//
// The whole model of src/core runs here: the base model and, switched on by their parameters,
// the food sources (field, pulses, light, diffusion and decay, crowding), oriented division, the
// field-dependent bulge and the morphogens. A step runs the passes in the CPU's order
// (core/sim_cpu.cpp): normals and bounds, grid, [shadow map, food], move, [morphogens], division.
// The passes in brackets are only encoded while their mechanism is on.
import * as S from './shaders.js';

const kMaxDeg = 12;
const GLOBALS_BYTES = S.GLOBALS_BYTES;
const SHADOW_TEXELS = 1 << 24;          // kShadowMaxTexels in core/food.hpp
const f = Math.fround;
const UNIT_EPS2 = f(f(1.0e-12) * f(1.0e-12));   // kUnitEps * kUnitEps in core/vec3.hpp

// The parameters and their defaults (src/core/params.hpp). A state's parameter set may leave
// any of them out (a checkpoint only lists the ones beyond the base model that are in use).
export const DEFAULT_PARAMS = {
  linkRestLength: 1, springFactor: 0.15, planarFactor: 0.10, bulgeFactor: 0.10, repulsionStrength: 0.10, radiusOfInfluence: 2,
  foodThreshold: 1, foodMin: 0, foodMax: 0.002, maxDisplacement: 0.5, daughterOffset: 0.1, maxCells: 2000000,
  fieldAmount: 0, fieldScale: 20, fieldOctaves: 1, fieldContrast: 1,
  pulsePeriod: 0, pulseDuty: 0.5, pulseLow: 0,
  lightAmount: 0, lightDirX: 0, lightDirY: 1, lightDirZ: 0, lightAmbient: 0.05, lightBias: 0.75, lightPixel: 1,
  foodDiffusion: 0, foodDecay: 0, crowdLow: 0, crowdHigh: 0,
  orientAmount: 0, orientAxisX: 0, orientAxisY: 1, orientAxisZ: 0, fieldBulge: 0,
  morphogens: 0, morphDiffA: 0.4, morphDiffB: 0.2, morphFeed: 0.055, morphKill: 0.062, morphIterations: 4,
  morphSeedLevel: 0.75, morphSeedScale: 3, morphFood: 0, morphBulge: 0, morphPlanar: 0, morphRest: 0,
};

// The parameters as the simulation's single-precision values, defaults filled in.
function floatParams(params) {
  const p = {};
  for (const k of Object.keys(DEFAULT_PARAMS)) p[k] = k === 'maxCells' ? Number(params[k] ?? DEFAULT_PARAMS[k]) : f(Number(params[k] ?? DEFAULT_PARAMS[k]));
  return p;
}

// validate() of src/core/params.cpp: { ok, errors (the run must not start), warnings }.
export function validateParams(params) {
  const p = floatParams(params);
  const errors = [], warnings = [];
  for (const k of Object.keys(p)) if (!Number.isFinite(p[k])) errors.push(`${k} is not finite`);
  for (const k of Object.keys(params)) if (!(k in DEFAULT_PARAMS)) errors.push(`unknown parameter '${k}'`);
  if (errors.length) return { ok: false, errors, warnings };
  const err = (bad, text) => { if (bad) errors.push(text); };
  err(!(p.linkRestLength > 0), 'linkRestLength must be > 0');
  err(!(p.radiusOfInfluence > 0), 'radiusOfInfluence must be > 0');
  err(p.springFactor < 0, 'springFactor must be >= 0');
  err(p.planarFactor < 0, 'planarFactor must be >= 0');
  err(p.bulgeFactor < 0, 'bulgeFactor must be >= 0');
  err(p.repulsionStrength < 0, 'repulsionStrength must be >= 0');
  const sum = f(f(p.springFactor + p.planarFactor) + p.bulgeFactor);
  if (sum >= 2) errors.push('springFactor + planarFactor + bulgeFactor must be < 2 (the update diverges)');
  else if (sum > 1) warnings.push('springFactor + planarFactor + bulgeFactor > 1: the update overshoots');
  err(!(p.foodThreshold > 0), 'foodThreshold must be > 0');
  err(p.foodMin < 0, 'foodMin must be >= 0');
  err(p.foodMax < p.foodMin, 'foodMax must be >= foodMin');
  err(p.maxDisplacement < 0, 'maxDisplacement must be >= 0 (0 disables the cap)');
  if (p.maxDisplacement === 0) warnings.push('maxDisplacement = 0: the displacement cap is off');
  if (!(p.daughterOffset > 0)) errors.push('daughterOffset must be > 0 (a daughter may never sit on its parent)');
  else if (p.daughterOffset > 0.5) warnings.push('daughterOffset > 0.5: daughters start further apart than a rest length');
  err(!(p.maxCells >= 4), 'maxCells must be >= 4');
  if (p.repulsionStrength > 1) warnings.push('repulsionStrength > 1 is far outside the tuned range');
  err(p.fieldAmount < 0 || p.fieldAmount > 1, 'fieldAmount must be in [0, 1]');
  err(!(p.fieldScale > 0), 'fieldScale must be > 0');
  err(p.fieldOctaves < 1 || p.fieldOctaves > 6, 'fieldOctaves must be in [1, 6]');
  err(p.fieldContrast < 0, 'fieldContrast must be >= 0');
  err(p.pulsePeriod < 0 || p.pulsePeriod > 1.0e9, 'pulsePeriod must be in [0, 1e9] steps (0 = off)');
  err(p.pulseDuty < 0 || p.pulseDuty > 1, 'pulseDuty must be in [0, 1]');
  err(p.pulseLow < 0 || p.pulseLow > 1, 'pulseLow must be in [0, 1]');
  err(p.lightAmount < 0 || p.lightAmount > 1, 'lightAmount must be in [0, 1]');
  const len2 = (x, y, z) => f(f(f(x * x) + f(y * y)) + f(z * z));
  err(p.lightAmount > 0 && !(len2(p.lightDirX, p.lightDirY, p.lightDirZ) > f(1.0e-12)), 'the light direction must not be zero');
  err(p.lightAmbient < 0 || p.lightAmbient > 1, 'lightAmbient must be in [0, 1]');
  err(p.lightBias < 0, 'lightBias must be >= 0');
  err(!(p.lightPixel > 0), 'lightPixel must be > 0');
  err(p.foodDiffusion < 0 || p.foodDiffusion >= 1, 'foodDiffusion must be in [0, 1)');
  err(p.foodDecay < 0 || p.foodDecay >= 1, 'foodDecay must be in [0, 1)');
  err(p.crowdLow < 0 || p.crowdHigh < 0, 'crowdLow and crowdHigh must be >= 0');
  err(p.orientAmount < 0 || p.orientAmount > 1, 'orientAmount must be in [0, 1]');
  err(p.orientAmount > 0 && !(len2(p.orientAxisX, p.orientAxisY, p.orientAxisZ) > f(1.0e-12)), 'the orientation axis must not be zero');
  err(p.fieldBulge < -1 || p.fieldBulge > 1, 'fieldBulge must be in [-1, 1]');
  err(p.morphDiffA < 0 || p.morphDiffA > 1 || p.morphDiffB < 0 || p.morphDiffB > 1, 'morphDiffA and morphDiffB must be in [0, 1]');
  err(p.morphFeed < 0 || p.morphFeed > 1 || p.morphKill < 0 || p.morphKill > 1, 'morphFeed and morphKill must be in [0, 1]');
  err(p.morphSeedLevel < 0 || p.morphSeedLevel > 1, 'morphSeedLevel must be in [0, 1]');
  err(!(p.morphSeedScale > 0), 'morphSeedScale must be positive');
  err(p.morphFood < -1 || p.morphFood > 1 || p.morphBulge < -1 || p.morphBulge > 1 || p.morphPlanar < -1 || p.morphPlanar > 1,
    'morphFood, morphBulge and morphPlanar must be in [-1, 1]');
  err(p.morphRest < -0.5 || p.morphRest > 0.5, 'morphRest must be in [-0.5, 0.5]');
  return { ok: errors.length === 0, errors, warnings };
}

// ---- single-precision vector arithmetic as in core/vec3.hpp (every operation rounded to float)
const dotf = (a, b) => f(f(f(a[0] * b[0]) + f(a[1] * b[1])) + f(a[2] * b[2]));
const crossf = (a, b) => [f(f(a[1] * b[2]) - f(a[2] * b[1])), f(f(a[2] * b[0]) - f(a[0] * b[2])), f(f(a[0] * b[1]) - f(a[1] * b[0]))];
function unitOrZero(v) {
  const l2 = dotf(v, v);
  if (l2 < UNIT_EPS2) return [0, 0, 0];
  const inv = f(1 / f(Math.sqrt(l2)));
  return [f(v[0] * inv), f(v[1] * inv), f(v[2] * inv)];
}

// The light's direction and the two unit vectors that span its shadow map, and the texel size
// before any doubling (make_food_params in core/food.cpp).
function lightBasis(p) {
  const L = unitOrZero([p.lightDirX, p.lightDirY, p.lightDirZ]);
  const ax = Math.abs(L[0]), ay = Math.abs(L[1]), az = Math.abs(L[2]);
  const axis = (ax <= ay && ax <= az) ? [1, 0, 0] : (ay <= az ? [0, 1, 0] : [0, 0, 1]);
  const U = unitOrZero(crossf(L, axis));
  const V = crossf(L, U);
  return { L, U, V, pixel: p.lightPixel * p.linkRestLength };   // the product of two floats is exact in a double
}

// The frame of the shadow map for a bounding box, computed as the CPU does it (in double
// precision). The GPU computes the same with software doubles (shaders.js, gridSetup); the
// verification compares the two.
export function lightFrame(params, lo, hi) {
  const p = floatParams(params);
  const { U, V, pixel: pixel0 } = lightBasis(p);
  let u0 = 0, u1 = 0, v0 = 0, v1 = 0;
  for (let c = 0; c < 8; c++) {
    const corner = [(c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2]];
    const u = dotf(corner, U), v = dotf(corner, V);
    if (c === 0) { u0 = u1 = u; v0 = v1 = v; } else {
      u0 = u < u0 ? u : u0; u1 = u > u1 ? u : u1; v0 = v < v0 ? v : v0; v1 = v > v1 ? v : v1;
    }
  }
  let pixel = pixel0, nu = 1, nv = 1;
  for (;;) {
    nu = Math.floor((u1 - u0) / pixel) + 1;
    nv = Math.floor((v1 - v0) / pixel) + 1;
    if (nu * nv <= SHADOW_TEXELS) break;
    pixel *= 2;
  }
  return { u0, v0, invPixel: f(1 / pixel), nu, nv };
}

// Everything the shaders need from the parameters: the first 16 words of Globals and its
// extension block (shaders.js), prepared as the host side of the CPU reference prepares
// KernelParams, FoodParams and MorphParams.
function hostParams(params, seed, opts) {
  const p = floatParams(params);
  const l0 = p.linkRestLength;
  const e = f(f(f(1.0e-6) * l0) * l0);           // normal_eps2 in core/kernels.hpp, in single precision
  const base = new Float32Array([l0, p.springFactor, p.planarFactor, p.bulgeFactor, p.repulsionStrength, p.radiusOfInfluence,
    f(p.maxDisplacement * l0), p.foodMin, f(p.foodMax - p.foodMin), p.foodThreshold, p.daughterOffset,
    f(e * e), UNIT_EPS2]);

  const morphOn = p.morphogens !== 0;
  let flags = 0;
  if (p.pulsePeriod > 0) flags |= S.F.PULSE;
  if (p.fieldAmount > 0) flags |= S.F.FIELD;
  if (p.lightAmount > 0) flags |= S.F.LIGHT;
  if (p.foodDiffusion > 0 || p.foodDecay > 0) flags |= S.F.DIFFUSE;
  if (p.crowdHigh > p.crowdLow) flags |= S.F.CROWD;
  if (morphOn && p.morphFood !== 0) flags |= S.F.MORPH;
  let xflags = 0;
  if (p.fieldBulge !== 0) xflags |= S.X.FIELD_BULGE;
  if (morphOn) xflags |= S.X.MORPH_ON;
  if (opts.morphPresent) xflags |= S.X.MORPH_PRESENT;
  if (p.orientAmount > 0) xflags |= S.X.ORIENT;
  // anything beyond the base model (uses_model_extensions in core/params.cpp)
  const extended = flags !== 0 || p.orientAmount > 0 || p.fieldBulge !== 0 || morphOn;
  const exact = opts.exact ?? extended;
  if (exact) xflags |= S.X.EXACT;

  const ext = new ArrayBuffer(S.PARAM_BLOCK.bytes);
  const dv = new DataView(ext);
  const U = (o, x) => dv.setUint32(o, x >>> 0, true);
  const Fl = (o, x) => dv.setFloat32(o, x, true);
  U(0, flags); U(4, xflags); U(8, 0);
  // Pulses: "on" while double(step % period) / double(period) < double(pulseDuty). That is
  // true for the first pulseOn steps of a period; pulseOn is found here with the very
  // expression of the CPU, so the GPU only compares integers.
  const period = p.pulsePeriod > 0 ? Math.trunc(p.pulsePeriod < 1 ? 1 : p.pulsePeriod) : 1;
  let on = 0;
  if (p.pulsePeriod > 0) {
    let a = 0, b = period;           // the smallest r in [0, period] with !(r / period < duty)
    while (a < b) { const mid = Math.floor((a + b) / 2); if (mid / period < p.pulseDuty) a = mid + 1; else b = mid; }
    on = a;
  }
  U(12, period); U(16, on); Fl(20, p.pulseLow);
  Fl(24, p.fieldAmount); Fl(28, f(1 / p.fieldScale)); Fl(32, p.fieldContrast);
  const oct = Math.floor(f(p.fieldOctaves + 0.5));
  U(36, oct < 1 ? 1 : (oct > 6 ? 6 : oct));
  Fl(40, p.fieldBulge);
  Fl(44, p.lightAmount); Fl(48, p.lightAmbient); Fl(52, f(p.lightBias * l0));
  const light = lightBasis(p);
  Fl(56, f(1 / light.pixel));
  {
    // the texel size as a double: 53-bit mantissa and exponent, for the software doubles
    const d = new DataView(new ArrayBuffer(8));
    d.setFloat64(0, light.pixel > 0 && Number.isFinite(light.pixel) ? light.pixel : 1, true);
    const hi = d.getUint32(4, true);
    dv.setInt32(60, ((hi >>> 20) & 0x7FF) - 1075, true);
    U(64, d.getUint32(0, true)); U(68, (hi & 0xFFFFF) | 0x100000);
  }
  Fl(72, p.foodDiffusion); Fl(76, f(1 - p.foodDecay));
  light.L.forEach((x, i) => Fl(80 + 4 * i, x));
  light.U.forEach((x, i) => Fl(96 + 4 * i, x));
  light.V.forEach((x, i) => Fl(112 + 4 * i, x));
  Fl(128, p.crowdLow); Fl(132, p.crowdHigh > p.crowdLow ? f(1 / f(p.crowdHigh - p.crowdLow)) : 0); Fl(136, f(1 / l0)); Fl(140, f(1 / p.radiusOfInfluence));
  Fl(144, p.morphFood); Fl(148, p.morphBulge); Fl(152, p.morphPlanar); Fl(156, p.morphRest);
  Fl(160, p.morphDiffA); Fl(164, p.morphDiffB); Fl(168, p.morphFeed); Fl(172, f(p.morphKill + p.morphFeed));
  Fl(176, p.morphSeedLevel); Fl(180, f(1 / p.morphSeedScale));
  U(184, Math.trunc(f(f(p.orientAmount * 200) + 0.5))); U(188, opts.tableCap);
  unitOrZero([p.orientAxisX, p.orientAxisY, p.orientAxisZ]).forEach((x, i) => Fl(192 + 4 * i, x));
  for (let o = 0; o < 6; o++) {
    const s = (BigInt(seed) + 0x51ED270B0n * BigInt(o + 1)) & 0xFFFFFFFFFFFFFFFFn;   // the field's seed for octave o
    U(208 + 8 * o, Number(s & 0xFFFFFFFFn)); U(212 + 8 * o, Number(s >> 32n));
  }
  const it = Math.floor(f(p.morphIterations + 0.5));
  return {
    base, ext, flags, xflags, morphOn, extended, exact,
    iterations: it < 1 ? 1 : (it > 16 ? 16 : it),
  };
}

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

// The passes: [name, WGSL, indirect slot (or -1 for a single dispatch)]. A name ending in 0 / 1
// is the variant with the GPU's own division and square root / with exact arithmetic.
function passList() {
  const scanPasses = (which, first) => [
    [`scan${which}up0`, S.scan(which, 'up', 0), first],
    [`scan${which}up1`, S.scan(which, 'up', 1), first + 1],
    [`scan${which}up2`, S.scan(which, 'up', 2), first + 2],
    [`scan${which}top`, S.scan(which, 'top', 3), 5],
    [`scan${which}down2`, S.scan(which, 'down', 2), first + 2],
    [`scan${which}down1`, S.scan(which, 'down', 1), first + 1],
    [`scan${which}down0`, S.scan(which, 'down', 0), first],
  ];
  return [
    ['prepare', S.prepare, -1],
    ['clear', S.clear, 1],
    ['pre0', S.pre(false), 0], ['pre1', S.pre(true), 0],
    ['gridSetup', S.gridSetup, -1],   // dispatched directly: it writes the indirect buffer (the shadow map's dispatch)
    ['gridCount', S.gridCount, 0],
    ...scanPasses(0, 2),
    ['gridFill', S.gridFill, 0],
    ['gridSort', S.gridSort, 1],
    ['shadowClear', S.shadowClear, 9],
    ['shadowBuild', S.shadowBuild, 0],
    ['food', S.food, 0],
    ['move0', S.move(false), 0], ['move1', S.move(true), 0],
    ['morph', S.morph, 0],
    ['morphInit', S.morphInit, 0],
    ['candidates0', S.candidates(false), 0], ['candidates1', S.candidates(true), 0],
    ['maxRing0', S.maxRing(0), 0],
    ['maxRing1', S.maxRing(1), 0],
    ['winFlag', S.winFlag, 0],
    ...scanPasses(1, 6),
    ['split0', S.split(false), 0], ['split1', S.split(true), 0],
    ['commit', S.commit, 5],
  ];
}
const SCAN = (which) => ['up0', 'up1', 'up2', 'top', 'down2', 'down1', 'down0'].map((s) => `scan${which}${s}`);

// The shaders do not depend on the capacity, so the pipelines are built once per device.
const pipelineCache = new WeakMap();
function pipelinesFor(device) {
  let promise = pipelineCache.get(device);
  if (!promise) {
    promise = (async () => {
      const out = {};
      await Promise.all(passList().map(async ([name, code, slot]) => {
        const module = device.createShaderModule({ code, label: name });
        const info = await module.getCompilationInfo();
        const errors = info.messages.filter((m) => m.type === 'error');
        if (errors.length) throw new Error(`shader ${name}: ` + errors.map((m) => `${m.lineNum}:${m.linePos} ${m.message}`).join('; '));
        const bindings = bindingsOf(code);
        const layout = device.createBindGroupLayout({
          entries: bindings.map((b) => ({ binding: b.binding, visibility: GPUShaderStage.COMPUTE, buffer: { type: b.readOnly ? 'read-only-storage' : 'storage' } })),
        });
        const pipeline = await device.createComputePipelineAsync({
          label: name,
          layout: device.createPipelineLayout({ bindGroupLayouts: [layout] }),
          compute: { module, entryPoint: 'main' },
        });
        out[name] = { name, pipeline, layout, bindings, slot };
      }));
      return out;
    })();
    pipelineCache.set(device, promise);
  }
  return promise;
}

export class Sim {
  // options.exact: true, false, or left out for 'auto' (see `exact` below).
  static async create(device, capacity, options = {}) {
    const sim = new Sim(device, capacity, options);
    await sim.#build();
    return sim;
  }

  constructor(device, capacity, options = {}) {
    this.device = device;
    this.capacity = capacity;
    this.tableCap = tableSizeFor(capacity);
    this.parity = 0;          // which geom / normal / morphogen buffer is live
    this.uploadStep = 0n;
    this.uploadParity = 0;
    this.allocated = 0;
    this.morphPresent = false;
    this.params = { ...DEFAULT_PARAMS };
    this.seed = 0n;
    this.#exact = options.exact === 'auto' ? undefined : options.exact;
  }

  #exact = undefined; // true, false, or undefined for 'auto'
  #host = null;       // hostParams() of the current parameters
  #sequence = [];     // the passes of one step for the current parameters
  #pass = null;       // name -> { pipeline, layout, bindings, slot, groups[parity] }
  #morphGroups = null;
  #submitted = 0;     // steps submitted since the upload
  #stopped = false;   // a read found that the GPU stopped committing steps (a failure, or the capacity stop)
  #reads = Promise.resolve();   // readGlobals calls take turns (they share one staging buffer)
  #epoch = 0;         // counts the uploads, so a read that was on its way across one is recognised

  // Exact mode: whether a step's geometry (move, grid, cleavage lengths, split) is computed in
  // exact arithmetic.
  //   true:   a step is bit-identical to the CPU reference and the CUDA backend, so the same
  //           start, seed and parameters give the same form, cell for cell, after any number of
  //           steps. On the test GPU a step then takes about 13% longer at 30,000 cells and
  //           22% longer at 100,000 (?task=speed).
  //   false:  the GPU's own division and square root, as this port always did: positions agree
  //           with the CPU reference to an ulp or two per step, which over thousands of steps
  //           grows into a different form of the same kind.
  //   'auto' (the default): false for the base model, whose results stay what they always were
  //           in this port; true as soon as anything beyond the base model is on.
  // In every mode a cell's food, the morphogens, the oriented cleavage pair and the normals of
  // an extended model are exact. Can be set between steps; reading gives what is in effect.
  get exact() { return this.#host ? this.#host.exact : this.#exact === true; }
  set exact(mode) {
    this.#exact = mode === 'auto' ? undefined : mode;
    if (this.#host) this.#writeParams();
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
      G: this.#buffer(GLOBALS_BYTES, ST | GPUBufferUsage.UNIFORM),   // a renderer may bind it (renderBuffers)
      ind: this.#buffer(10 * 12, GPUBufferUsage.STORAGE | GPUBufferUsage.INDIRECT | GPUBufferUsage.COPY_DST),
      geom: [this.#buffer(c * 16, ST | GPUBufferUsage.VERTEX), this.#buffer(c * 16, ST | GPUBufferUsage.VERTEX)],
      nrm: [this.#buffer(c * 16, ST | GPUBufferUsage.VERTEX), this.#buffer(c * 16, ST | GPUBufferUsage.VERTEX)],
      topo: this.#buffer(c * 64, ST | GPUBufferUsage.VERTEX),
      sel: this.#buffer(c * 24, ST),
      scanData: this.#buffer(scanEntries * 4, ST),
      cellFill: this.#buffer(this.tableCap * 4, ST),
      sortedId: this.#buffer(c * 4, ST),
      sortedPos: this.#buffer(c * 16, ST),
      levels: this.#buffer((Math.ceil(scanEntries / 255) + 1024) * 4, ST),
      // Allocated the first time they are needed: the morphogens ([parity] and a scratch buffer
      // for the iterations inside a step; 24 bytes per cell) and the light's shadow map (64 MiB).
      morph: null, morphScratch: null, shadow: null,
      dummy: [this.#buffer(16, ST), this.#buffer(16, ST)],   // stand in for them until then
    };
    this.readback = d.createBuffer({ size: GLOBALS_BYTES, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    this.#pass = {};
    for (const [name, p] of Object.entries(await pipelinesFor(d))) this.#pass[name] = { ...p, groups: null };
    this.#bind();
  }

  // (Re)creates the bind groups: two per pass, "cur" names point at buffer `p`, "new" names at
  // the other one.
  #bind() {
    const d = this.device;
    const group = (pass, p, extra) => d.createBindGroup({
      layout: pass.layout,
      entries: pass.bindings.map((b) => ({ binding: b.binding, resource: { buffer: this.#resolve(b.name, p, extra) } })),
    });
    for (const pass of Object.values(this.#pass)) {
      if (pass.name === 'morph') continue;
      pass.groups = [0, 1].map((p) => group(pass, p));
    }
    // The reaction-diffusion iterations go between the current buffer (c), the new one (n) and
    // the scratch buffer (s).
    this.#morphGroups = null;
    const b = this.buf;
    if (b.morph) {
      const of = (p, which) => (which === 'c' ? b.morph[p] : which === 'n' ? b.morph[1 - p] : b.morphScratch);
      this.#morphGroups = {};
      for (const combo of ['cs', 'cn', 'sn', 'ns'])
        this.#morphGroups[combo] = [0, 1].map((p) => group(this.#pass.morph, p, { morphSrc: of(p, combo[0]), morphDst: of(p, combo[1]) }));
    }
  }

  #resolve(name, p, extra) {
    const b = this.buf;
    if (extra && name in extra) return extra[name];
    switch (name) {
      case 'geomCur': return b.geom[p];
      case 'geomNew': return b.geom[1 - p];
      case 'nrmCur': return b.nrm[p];
      case 'nrmNew': return b.nrm[1 - p];
      case 'morphCur': return b.morph ? b.morph[p] : b.dummy[0];
      case 'morphNew': return b.morph ? b.morph[1 - p] : b.dummy[1];
      case 'shadow': return b.shadow ?? b.dummy[0];
      default:
        if (!(name in b) || !b[name]) throw new Error(`no buffer for binding '${name}'`);
        return b[name];
    }
  }

  #ensureMorph() {
    if (this.buf.morph) return;
    const usage = GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST | GPUBufferUsage.VERTEX;
    this.buf.morph = [this.#buffer(this.capacity * 8, usage), this.#buffer(this.capacity * 8, usage)];
    this.buf.morphScratch = this.#buffer(this.capacity * 8, usage);
    this.#bind();
  }

  #ensureShadow() {
    if (this.buf.shadow) return;
    this.buf.shadow = this.#buffer(SHADOW_TEXELS * 4, GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC);
    this.#bind();
  }

  // Writes the parameters to the GPU and works out which passes a step has.
  #writeParams() {
    const h = hostParams(this.params, this.seed, { morphPresent: this.morphPresent, exact: this.#exact, tableCap: this.tableCap });
    this.#host = h;
    const q = this.device.queue;
    q.writeBuffer(this.buf.G, 0, h.base);
    q.writeBuffer(this.buf.G, 60, new Uint32Array([Math.min(this.params.maxCells ?? this.capacity, this.capacity)]));
    q.writeBuffer(this.buf.G, S.PARAM_BLOCK.offset, h.ext);

    const exact = h.exact;
    const P = this.#pass;
    const light = (h.flags & S.F.LIGHT) !== 0;
    const names = ['prepare', 'clear', (exact || h.extended) ? 'pre1' : 'pre0', 'gridSetup', 'gridCount', ...SCAN(0), 'gridFill', 'gridSort'];
    if (light) names.push('shadowClear', 'shadowBuild');
    if (h.flags !== 0) names.push('food');
    names.push(exact ? 'move1' : 'move0');
    const seq = names.map((n) => ({ pipeline: P[n].pipeline, groups: P[n].groups, slot: P[n].slot }));
    if (this.morphPresent) {
      // As core/sim_cpu.cpp: the last iteration writes the new buffer, the ones before it
      // alternate backwards from there. Switched off, one pass carries the values over.
      const k = h.morphOn ? h.iterations : 1;
      let src = 'c';
      for (let it = 0; it < k; it++) {
        const dst = (k - 1 - it) % 2 === 0 ? 'n' : 's';
        seq.push({ pipeline: P.morph.pipeline, groups: this.#morphGroups[src + dst], slot: 0 });
        src = dst;
      }
    }
    for (const n of [exact ? 'candidates1' : 'candidates0', 'maxRing0', 'maxRing1', 'winFlag', ...SCAN(1), exact ? 'split1' : 'split0', 'commit'])
      seq.push({ pipeline: P[n].pipeline, groups: P[n].groups, slot: P[n].slot });
    this.#sequence = seq;
  }

  // Gives every cell its starting morphogen concentrations, on the GPU, from the positions and
  // the step counter as they are when this runs (init_morphogens in core/morph.cpp).
  #initMorphogens() {
    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    pass.setPipeline(this.#pass.prepare.pipeline);       // sizes the per-cell dispatch from the cell count
    pass.setBindGroup(0, this.#pass.prepare.groups[this.parity]);
    pass.dispatchWorkgroups(1);
    pass.setPipeline(this.#pass.morphInit.pipeline);
    pass.setBindGroup(0, this.#pass.morphInit.groups[this.parity]);
    pass.dispatchWorkgroupsIndirect(this.buf.ind, 0);
    pass.end();
    this.device.queue.submit([encoder.finish()]);
  }

  // Loads a state (from parseCheckpoint or makeStart). Parameters come from state.params; any
  // mechanism of the model may be on. A state with morphogens switched on but without
  // concentrations (state.morphA, state.morphB) gets its starting ones here, as the CPU
  // reference does when a run starts.
  upload(state) {
    const n = state.n;
    if (n > this.capacity) throw new Error(`state has ${n} cells, capacity is ${this.capacity}`);
    const issues = validateParams(state.params);
    if (!issues.ok) throw new Error('invalid parameters: ' + issues.errors.join('; '));
    const q = this.device.queue;
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

    this.params = { ...state.params };
    this.seed = BigInt(state.seed);
    this.parity = 0;
    this.uploadParity = 0;
    this.uploadStep = state.step;
    this.#submitted = 0;
    this.#stopped = false;
    this.#epoch++;
    this.known = { n, step: state.step, failed: 0 };

    const p = floatParams(this.params);
    const hasMorph = !!(state.morphA && state.morphB);
    const startMorph = !hasMorph && p.morphogens !== 0;
    this.morphPresent = hasMorph || startMorph;
    if (this.morphPresent) this.#ensureMorph();
    if (p.lightAmount > 0) this.#ensureShadow();
    if (hasMorph) {
      const ab = new Float32Array(n * 2);
      for (let v = 0; v < n; v++) { ab[2 * v] = state.morphA[v]; ab[2 * v + 1] = state.morphB[v]; }
      q.writeBuffer(this.buf.morph[0], 0, ab);
    }

    const g = new ArrayBuffer(GLOBALS_BYTES);
    const dv = new DataView(g);
    dv.setUint32(52, Number(this.seed & 0xFFFFFFFFn), true);
    dv.setUint32(56, Number(this.seed >> 32n), true);
    dv.setUint32(64, n, true);
    dv.setUint32(68, Number(state.step & 0xFFFFFFFFn), true);
    dv.setUint32(72, Number(state.step >> 32n), true);
    q.writeBuffer(this.buf.G, 0, g);
    this.#writeParams();
    if (startMorph) this.#initMorphogens();
  }

  // Changes parameters between steps, while the run goes on: every parameter of the model, as
  // CpuSim::set_params allows (the capacity stays; maxCells counts up to it). A mechanism is
  // switched on or off by its parameters. Morphogens switched on for the first time start as
  // the CPU starts them: from the positions and the step counter at that moment.
  // Returns { ok, errors, warnings } (validate() of core/params.cpp); with errors nothing changes.
  setParams(p) {
    const issues = validateParams(p);
    if (!issues.ok) return issues;
    this.params = { ...p };
    const fp = floatParams(p);
    const startMorph = fp.morphogens !== 0 && !this.morphPresent;
    if (startMorph) { this.#ensureMorph(); this.morphPresent = true; }
    if (fp.lightAmount > 0) this.#ensureShadow();
    this.#writeParams();
    if (startMorph) this.#initMorphogens();
    return issues;
  }

  // Encodes and submits `count` steps in one command buffer. `parity` (which of the two
  // buffers is live) follows what is submitted, so renderBuffers() is right as soon as this
  // returns, whether or not a readGlobals is on its way. Once a read has shown that the GPU
  // stopped committing steps (a failure, or the capacity stop), further steps would do nothing
  // there and are not submitted: the state stays that of the last good step.
  step(count = 1) {
    if (this.#stopped) return;
    this.#submitted += count;
    const encoder = this.device.createCommandEncoder();
    const pass = encoder.beginComputePass();
    for (let i = 0; i < count; i++) {
      for (const p of this.#sequence) {
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

  // Compute passes in one step with the current parameters.
  get passesPerStep() { return this.#sequence.length; }

  // What a renderer draws from: the live per-cell buffers and the cell count.
  // geom: vec4<f32> per cell (position, food); nrm: vec4<f32> (unit normal); topo: 16 u32 per
  // cell (neighbour count, birth step, generation, flags, 12 neighbours counter-clockwise seen
  // from outside); morph: null until the cells have morphogens, then vec2<f32> per cell, 8 bytes:
  // (A, the substrate; B, the activator), both in [0, 1]. The pattern is in B: min(3 B, 1) is the
  // signal that sets the cells' parameters. (The concentrations stay, unchanged, when the
  // morphogens are switched off again.) All but topo change places every step: ask again after
  // stepping. Every per-cell buffer can be bound as a vertex buffer or as (read-only) storage.
  // count: the cell count as last read (readGlobals), which lags behind the steps submitted
  // since. The count as the GPU has it when a draw runs is in `globals`: the u32 at byte
  // `countOffset` of that buffer (GLOBALS_BYTES long; bind it as read-only storage, for example
  // as array<u32> and read element countOffset / 4, or as a uniform), so a vertex shader can
  // drop the instances past the last cell without waiting for a read. The step counter is the
  // two u32 after it (low word, high word).
  renderBuffers() {
    return {
      geom: this.buf.geom[this.parity], nrm: this.buf.nrm[this.parity], topo: this.buf.topo,
      morph: this.morphPresent ? this.buf.morph[this.parity] : null, count: this.known ? this.known.n : 0,
      globals: this.buf.G, countOffset: 64,
    };
  }

  // Reads the GPU-side counters. Resolves when everything submitted before the call has run.
  // Steps may be submitted while a read is on its way, and reads may overlap (they take turns).
  readGlobals() {
    const run = () => this.#readGlobals();
    this.#reads = this.#reads.then(run, run);
    return this.#reads;
  }

  async #readGlobals() {
    const epoch = this.#epoch;
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
      // of the last step that ran: the bounds of the positions it started from, the grid, the
      // pulse's scale and the frame of the light's shadow map
      boundsMin: [ordered(144), ordered(148), ordered(152)], boundsMax: [ordered(156), ordered(160), ordered(164)],
      grid: [u(96), u(100), u(104)], invCell: dv.getFloat32(92, true),
      pulse: dv.getFloat32(576, true),
      light: { u0: dv.getFloat32(580, true), v0: dv.getFloat32(584, true), invPixel: dv.getFloat32(588, true), nu: u(592), nv: u(596) },
    };
    // The live buffers follow what was submitted (step() flips them), which stays right when
    // steps were submitted while this read was on its way. Only when the GPU stopped committing
    // (a failure, or the capacity stop) do the steps submitted past that point not count: the
    // live buffers are then those of the last good step, for good.
    if (epoch !== this.#epoch) return g;   // a state was uploaded meanwhile: these are the old state's counters
    if (g.failed) {
      this.parity = (this.uploadParity + Number((g.step - this.uploadStep) & 1n)) & 1;
      this.#stopped = true;
    }
    this.known = g;
    return g;
  }

  // Copies the whole state back (for the checker and for comparisons). With morphogens present
  // the state has morphA and morphB (Float32Array n), as a checkpoint has.
  async download() {
    // The counters must be those of the state that is copied: read again if steps were
    // submitted while the read was on its way.
    let g, mark;
    do {
      mark = this.#submitted;
      g = await this.readGlobals();
    } while (mark !== this.#submitted && !g.failed);
    const n = g.n;
    const d = this.device;
    const mk = (bytes) => d.createBuffer({ size: Math.max(16, bytes), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const sg = mk(n * 16), sn = mk(n * 16), st = mk(n * 64), sm = this.morphPresent ? mk(n * 8) : null;
    const encoder = d.createCommandEncoder();
    if (n > 0) {
      encoder.copyBufferToBuffer(this.buf.geom[this.parity], 0, sg, 0, n * 16);
      encoder.copyBufferToBuffer(this.buf.nrm[this.parity], 0, sn, 0, n * 16);
      encoder.copyBufferToBuffer(this.buf.topo, 0, st, 0, n * 64);
      if (sm) encoder.copyBufferToBuffer(this.buf.morph[this.parity], 0, sm, 0, n * 8);
    }
    d.queue.submit([encoder.finish()]);
    const staged = sm ? [sg, sn, st, sm] : [sg, sn, st];
    await Promise.all(staged.map((b) => b.mapAsync(GPUMapMode.READ)));
    const geom = new Float32Array(sg.getMappedRange().slice(0));
    const nrm = new Float32Array(sn.getMappedRange().slice(0));
    const topo = new Uint32Array(st.getMappedRange().slice(0));
    const ab = sm ? new Float32Array(sm.getMappedRange().slice(0)) : null;
    for (const b of staged) { b.unmap(); b.destroy(); }
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
    if (ab) {
      s.morphA = new Float32Array(n);
      s.morphB = new Float32Array(n);
      for (let v = 0; v < n; v++) { s.morphA[v] = ab[2 * v]; s.morphB[v] = ab[2 * v + 1]; }
    }
    return s;
  }

  destroy() {
    const b = this.buf;
    for (const x of [b.G, b.ind, ...b.geom, ...b.nrm, b.topo, b.sel, b.scanData, b.cellFill, b.sortedId, b.sortedPos, b.levels,
      ...(b.morph ?? []), b.morphScratch, b.shadow, ...b.dummy, this.readback]) if (x) x.destroy();
  }
}
