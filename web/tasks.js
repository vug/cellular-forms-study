// Scripted tasks of the WebGPU port: verify against CPU checkpoints, rerun, benchmark, and for
// the whole model: the exact-arithmetic self-test, the many-step comparison with the forms of
// the gallery, and the speed of every mechanism.
// They take the device, the URL query, a log function and optional hooks, so they can run on
// the page or in a worker. (In a worker they keep their speed when the page is hidden; a
// hidden page has its timers and callbacks throttled to about one per second.)
import { Sim, lightFrame } from './sim.js';
import { parseCheckpoint, checkInvariants, linkStats, fingerprint, kMaxDeg } from './cfs.js';
import { makeStart } from './start.js';
import { floatTest } from './shaders.js';

const FAIL = ['', 'non-finite position', 'grid too large', 'non-finite new position', 'capacity reached'];

export async function fetchState(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`cannot load ${url}: ${r.status}`);
  return parseCheckpoint(await r.arrayBuffer());
}

// Returns { device, describe (two lines of text), info (the adapter's GPUAdapterInfo: vendor,
// architecture, device, description), adapter }.
export async function requestDevice() {
  if (!navigator.gpu) throw new Error('WebGPU is not available here');
  // On Windows Chrome ignores powerPreference and only logs a warning about it; elsewhere it
  // picks the fast GPU of a machine with two.
  const windows = /^win/i.test(navigator.userAgentData?.platform ?? navigator.platform ?? '') || /Windows/.test(navigator.userAgent ?? '');
  const adapter = await navigator.gpu.requestAdapter(windows ? {} : { powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter');
  const L = adapter.limits;
  // The simulation's shaders bind at most 8 storage buffers each (the default limit; with
  // ?limits=default in the page's address the device is created with exactly that, to prove
  // it). The higher limit is asked for as before, for other users of this device (the app's
  // renderer).
  const defaultLimits = typeof location !== 'undefined' && new URLSearchParams(location.search).get('limits') === 'default';
  const device = await adapter.requestDevice({
    requiredLimits: {
      maxStorageBufferBindingSize: L.maxStorageBufferBindingSize,
      maxBufferSize: L.maxBufferSize,
      ...(defaultLimits ? {} : { maxStorageBuffersPerShaderStage: Math.min(L.maxStorageBuffersPerShaderStage, 10) }),
      maxComputeWorkgroupsPerDimension: L.maxComputeWorkgroupsPerDimension,
    },
  });
  const info = adapter.info ?? {};
  const describe = `adapter: ${info.vendor ?? '?'} ${info.architecture ?? ''} ${info.device ?? ''} ${info.description ?? ''}`.trim() +
    `\nlimits: buffer ${(L.maxBufferSize / 1048576).toFixed(0)} MiB, storage binding ${(L.maxStorageBufferBindingSize / 1048576).toFixed(0)} MiB, ` +
    `${device.limits.maxStorageBuffersPerShaderStage} storage buffers per stage (the simulation uses 8 at most), ${L.maxComputeWorkgroupsPerDimension} workgroups per dimension`;
  return { device, describe, info, adapter };
}

const bitsOf = (a) => new Uint32Array(a.buffer, a.byteOffset, a.length);

// Two states of the same step, value by value. Counts are of values whose bits differ.
function compareStates(got, want) {
  const r = { cells: got.n === want.n, rings: 0, scalars: 0, morph: 0, posBits: 0, nrmBits: 0, maxPos: 0, maxNrm: 0,
    morphPresent: !!got.morphA === !!want.morphA };
  if (!r.cells) return r;
  const n = want.n;
  const gf = bitsOf(got.food), wf = bitsOf(want.food);
  for (let v = 0; v < n; v++) {
    if (got.deg[v] !== want.deg[v]) r.rings++;
    for (let k = 0; k < kMaxDeg; k++) if (got.nbr[kMaxDeg * v + k] !== want.nbr[kMaxDeg * v + k]) r.rings++;
    if (gf[v] !== wf[v] || got.birth[v] !== want.birth[v] || got.gen[v] !== want.gen[v]) r.scalars++;
  }
  if (got.morphA && want.morphA) {
    const ga = bitsOf(got.morphA), wa = bitsOf(want.morphA), gb = bitsOf(got.morphB), wb = bitsOf(want.morphB);
    for (let v = 0; v < n; v++) if (ga[v] !== wa[v] || gb[v] !== wb[v]) r.morph++;
  }
  if (r.rings === 0) {
    const gp = bitsOf(got.pos), wp = bitsOf(want.pos), gn = bitsOf(got.normal), wn = bitsOf(want.normal);
    for (let i = 0; i < 3 * n; i++) {
      if (gp[i] !== wp[i]) { r.posBits++; r.maxPos = Math.max(r.maxPos, Math.abs(got.pos[i] - want.pos[i])); }
      if (gn[i] !== wn[i]) { r.nrmBits++; r.maxNrm = Math.max(r.maxNrm, Math.abs(got.normal[i] - want.normal[i])); }
    }
  }
  return r;
}

// One step (or a few) from a CPU state, compared with the CPU state that many steps later.
// Every pair runs twice: as the port runs by default, and in exact mode (sim.js). Both must give
// the CPU's cell count, rings, food, birth, generation and morphogens exactly, positions within
// 2e-4 and a clean invariant check; exact mode must also give the positions and normals exactly.
// A pair may carry `set`: parameters applied with setParams after the upload (the CPU state
// after it was then written by a run that resumed with those parameters).
// The manifest's `starts` compare the start states made in the page with the CPU's.
async function taskVerify(device, query, log) {
  const manifest = await (await fetch(query.get('data') ?? '/runs/web_verify/manifest.json')).json();
  const modes = { fast: [false], exact: [true], both: [false, true] }[query.get('mode') ?? 'both'];
  const only = query.get('only');
  let allOk = true, compared = 0, worst = 0;
  for (const pair of manifest.pairs) {
    if (only && !pair.label.includes(only)) continue;
    const before = await fetchState(pair.before), after = await fetchState(pair.after);
    const steps = Number(after.step - before.step);
    for (const exact of modes) {
      const sim = await Sim.create(device, Math.max(after.n + 1024, 4096), { exact });
      sim.upload(before);
      if (pair.set) {
        const issues = sim.setParams({ ...before.params, ...pair.set });
        if (!issues.ok) throw new Error(`${pair.label}: ${issues.errors.join('; ')}`);
      }
      sim.step(steps);
      const got = await sim.download();
      const g = sim.known;
      sim.destroy();
      const c = compareStates(got, after);
      const check = checkInvariants(got);
      // The frame of the light's shadow map of the last step, against the CPU's formula in doubles.
      let lightNote = '', lightOk = true;
      if ((after.params.lightAmount ?? 0) > 0 && steps === 1) {
        const want = lightFrame(after.params, g.boundsMin, g.boundsMax);
        lightOk = want.nu === g.light.nu && want.nv === g.light.nv && want.u0 === g.light.u0 && want.v0 === g.light.v0 && want.invPixel === g.light.invPixel;
        lightNote = `, shadow map ${g.light.nu} x ${g.light.nv} ${lightOk ? 'as the CPU sizes it' : `BUT the CPU has ${want.nu} x ${want.nv}`}`;
      }
      const ok = c.cells && c.rings === 0 && c.scalars === 0 && c.morph === 0 && c.morphPresent && c.maxPos <= 2e-4 && check.ok && got.step === after.step &&
        lightOk && (!exact || (c.posBits === 0 && c.nrmBits === 0));
      allOk = allOk && ok;
      compared++;
      worst = Math.max(worst, c.maxPos);
      log(`verify ${pair.label}${exact ? ' [exact]' : ''}: ${before.n} -> ${got.n} cells (cpu ${after.n}), ${steps} step(s): ring slots differing ${c.rings}, ` +
        `food/birth/generation differing ${c.scalars}, ` + (after.morphA ? `morphogens differing ${c.morph}${c.morphPresent ? '' : ' (missing)'}, ` : '') +
        `positions differing ${c.posBits} (max ${c.maxPos.toExponential(2)}), normals differing ${c.nrmBits} (max ${c.maxNrm.toExponential(2)})${lightNote}, ` +
        `checker ${check.ok ? 'ok' : check.message} -> ${ok ? 'ok' : 'MISMATCH'}`);
    }
  }
  // Start states: makeStart in the page against the CPU's state at step 0, and the starting
  // morphogens the GPU gives a state uploaded without them.
  for (const s of manifest.starts ?? []) {
    if (only && !s.label.includes(only)) continue;
    const want = await fetchState(s.state);
    const made = makeStart(await fetchState(s.mesh), { seed: s.seed, food: s.food, params: s.params });
    const c = compareStates(made, want);
    let ok = c.cells && c.rings === 0 && c.scalars === 0 && c.morph === 0 && c.morphPresent && c.posBits === 0 && c.nrmBits === 0;
    let note = '';
    if (want.morphA) {
      const sim = await Sim.create(device, 4096 + want.n);
      const bare = { ...made };
      delete bare.morphA;
      delete bare.morphB;
      sim.upload(bare);
      const got = await sim.download();
      sim.destroy();
      const cg = compareStates(got, want);
      ok = ok && cg.morph === 0 && cg.morphPresent;
      note = `; started on the GPU: morphogens differing ${cg.morph}`;
    }
    allOk = allOk && ok;
    compared++;
    log(`verify start ${s.label}: ${made.n} cells: food differing ${c.scalars}, morphogens differing ${c.morph}${c.morphPresent ? '' : ' (missing)'}, ` +
      `positions differing ${c.posBits}${note} -> ${ok ? 'ok' : 'MISMATCH'}`);
  }
  log(`verify: ${compared} comparisons, largest position difference ${worst.toExponential(2)} -> ${allOk ? 'PASS' : 'FAIL'}`);
  return allOk;
}

// The exact arithmetic of shaders.js (integer division, square root, subnormal-safe add and
// multiply, software doubles) against JavaScript's IEEE arithmetic, on random and on awkward
// operands. Also what the GPU's own operations do. One of those the port relies on: a * b + c on
// normal numbers must be the two IEEE operations, not one fused multiply-add (the shading
// language has no way to forbid the fusion). The others are for the record: the GPU's division
// and square root (not correctly rounded on the test GPU) and its results for subnormal numbers
// (flushed to zero there), which is why the exact operations exist.
async function taskFloats(device, query, log) {
  const N = Number(query.get('count') ?? (1 << 18));
  const fr = Math.fround;
  const inp = new Uint32Array(N * 8);
  const asF = new Float32Array(inp.buffer);
  const dv = new DataView(new ArrayBuffer(8));
  const rnd = (n) => Math.floor(Math.random() * n);
  const bitsFloat = (kind) => {
    // random sign, an exponent from a range that depends on the kind, a mantissa that is often awkward
    const sign = rnd(2) << 31;
    const exp = kind === 0 ? 100 + rnd(56) : kind === 1 ? rnd(4) : kind === 2 ? 1 + rnd(60) : kind === 3 ? 120 + rnd(16) : rnd(255);
    const pick = rnd(8);
    const man = pick === 0 ? 0 : pick === 1 ? 0x7FFFFF : pick === 2 ? 1 << rnd(23) : pick === 3 ? (0x7FFFFF ^ (1 << rnd(23))) : rnd(0x800000);
    return (sign | (exp << 23) | man) >>> 0;
  };
  for (let i = 0; i < N; i++) {
    const kind = i % 5;
    inp[8 * i] = bitsFloat(kind);
    inp[8 * i + 1] = bitsFloat(i % 3 === 0 ? kind : rnd(5));
    if (i % 16 === 0) inp[8 * i + 1] = inp[8 * i] ^ (rnd(2) << 31) ^ (rnd(4) === 0 ? 1 : 0);    // nearly equal magnitudes
    if (i % 16 === 1) { asF[8 * i] = rnd(1 << 12); asF[8 * i + 1] = 1 + rnd(1 << 12); }           // small integers
    asF[8 * i + 2] = fr(Math.exp((Math.random() - 0.5) * 20));
    if (i % 16 === 2) {
      // c = -(a * b) as a float: a * b + c is then 0 in two operations and the rounding error
      // of the product in a fused one
      const product = fr(asF[8 * i] * asF[8 * i + 1]);
      if (Number.isFinite(product) && Math.abs(product) >= 1.2e-38) asF[8 * i + 2] = -product;
    }
    // a positive double: the product of two floats, as the shadow map's texel size is
    const d = fr(Math.exp((Math.random() - 0.5) * 12)) * fr(i % 4 === 0 ? 1 : Math.exp((Math.random() - 0.5) * 6));
    dv.setFloat64(0, d, true);
    const hi = dv.getUint32(4, true);
    inp[8 * i + 3] = dv.getUint32(0, true);
    inp[8 * i + 4] = (hi & 0xFFFFF) | 0x100000;
    inp[8 * i + 5] = (((hi >>> 20) & 0x7FF) - 1075) >>> 0;
  }
  const doubles = new Float64Array(N);
  for (let i = 0; i < N; i++) {
    dv.setUint32(0, inp[8 * i + 3], true);
    dv.setUint32(4, (inp[8 * i + 4] & 0xFFFFF) | (((inp[8 * i + 5] | 0) + 1075) << 20), true);
    doubles[i] = dv.getFloat64(0, true);
  }
  const module = device.createShaderModule({ code: floatTest });
  const errors = (await module.getCompilationInfo()).messages.filter((m) => m.type === 'error');
  if (errors.length) throw new Error('float test shader: ' + errors.map((m) => m.message).join('; '));
  const pipeline = await device.createComputePipelineAsync({ layout: 'auto', compute: { module, entryPoint: 'main' } });
  const bin = device.createBuffer({ size: inp.byteLength, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_DST });
  const bout = device.createBuffer({ size: N * 64, usage: GPUBufferUsage.STORAGE | GPUBufferUsage.COPY_SRC });
  const read = device.createBuffer({ size: N * 64, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(bin, 0, inp);
  const encoder = device.createCommandEncoder();
  const pass = encoder.beginComputePass();
  pass.setPipeline(pipeline);
  pass.setBindGroup(0, device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: bin } }, { binding: 1, resource: { buffer: bout } }] }));
  pass.dispatchWorkgroups(Math.ceil(N / 64));
  pass.end();
  encoder.copyBufferToBuffer(bout, 0, read, 0, N * 64);
  device.queue.submit([encoder.finish()]);
  await read.mapAsync(GPUMapMode.READ);
  const out = new Uint32Array(read.getMappedRange().slice(0));
  read.unmap();
  for (const b of [bin, bout, read]) b.destroy();

  const names = ['a / b', 'sqrt |a|', 'a * b (soft)', 'a + b (soft)', 'a * b', 'a + b', 'floor((hi - lo) / d) in doubles', 'floor((hi - lo) * c) in doubles',
    'a - b', 'clamp of a to [0, 1]', '-a'];
  const slot = [0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14];   // where each result is in an item's 16 words
  const bad = names.map(() => 0), tested = names.map(() => 0), first = names.map(() => null);
  // the GPU's own operations: [cases, results that are not the IEEE ones]
  const own = { 'a * b + c on normal numbers': [0, 0], 'a / b on normal numbers': [0, 0], 'sqrt on normal numbers': [0, 0], 'a * b with a subnormal result': [0, 0] };
  const normal = (x) => x === 0 || Math.abs(x) >= 1.1754943508222875e-38;
  const one = new Float32Array(1), oneBits = new Uint32Array(one.buffer);
  const same = (bits, value) => {
    one[0] = value;
    return oneBits[0] === bits || (Number.isNaN(value) && (bits & 0x7F800000) === 0x7F800000 && (bits & 0x7FFFFF) !== 0);
  };
  const sat = (x) => (x >= 4294967295 ? 4294967295 : x);
  for (let i = 0; i < N; i++) {
    const a = asF[8 * i], b = asF[8 * i + 1], c = asF[8 * i + 2], d = doubles[i];
    const finite = Number.isFinite(a) && Number.isFinite(b);
    const hi = Math.max(a, b), lo = Math.min(a, b);
    const want = [fr(a / b), fr(Math.sqrt(Math.abs(a))), fr(a * b), fr(a + b), fr(a * b), fr(a + b), sat(Math.floor((hi - lo) / d)), sat(Math.floor((hi - lo) * Math.abs(c))),
      fr(a - b), a < 0 ? 0 : (a > 1 ? 1 : a), -a];
    for (let k = 0; k < names.length; k++) {
      if (!finite || (k === 0 && b === 0)) continue;
      tested[k]++;
      const got = out[16 * i + slot[k]];
      const isFloat = k !== 6 && k !== 7;
      const ok = isFloat ? same(got, want[k]) : got === want[k];
      if (!ok) {
        bad[k]++;
        if (!first[k]) first[k] = `a=${a} b=${b} c=${c} d=${d}: got ${isFloat ? '0x' + got.toString(16) : got}, want ${isFloat ? (one[0] = want[k], '0x' + oneBits[0].toString(16)) : want[k]}`;
      }
    }
    if (!finite) continue;
    const product = fr(a * b), quotient = fr(a / b), sum = fr(product + c);
    const count = (name, bits, value) => { own[name][0]++; if (!same(bits, value)) own[name][1]++; };
    if (normal(a) && normal(b) && normal(product) && normal(sum) && Number.isFinite(sum) && product !== 0) count('a * b + c on normal numbers', out[16 * i + 8], sum);
    if (normal(a) && normal(b) && b !== 0 && normal(quotient) && Number.isFinite(quotient)) count('a / b on normal numbers', out[16 * i + 9], quotient);
    if (normal(a) && a !== 0) count('sqrt on normal numbers', out[16 * i + 10], want[1]);
    if (normal(a) && normal(b) && !normal(product)) count('a * b with a subnormal result', out[16 * i + 11], product);
  }
  let ok = true;
  names.forEach((name, k) => {
    ok = ok && bad[k] === 0;
    log(`floats ${name}: ${tested[k]} cases, ${bad[k]} wrong${first[k] ? ' (first: ' + first[k] + ')' : ''}`);
  });
  for (const [name, [cases, wrong]] of Object.entries(own)) log(`floats, the GPU's own ${name}: ${cases} cases, ${wrong} not the IEEE result`);
  const fused = own['a * b + c on normal numbers'][1];
  if (fused) log('floats: this GPU does not compute a * b + c as two IEEE operations: the simulation still runs, but will not match the CPU reference bit for bit');
  ok = ok && fused === 0;
  log(`floats: ${ok ? 'PASS' : 'FAIL'}`);
  return ok;
}

// Runs from the start state until the cell count reaches `target`. Steps go out in batches; the
// cell count is read back once per batch (about every 100 ms of simulation).
async function growTo(device, query, start, target, capacity, onProgress, options) {
  const sim = await Sim.create(device, capacity, options);
  sim.upload(start);
  const t0 = performance.now();
  let batch = 4, g = sim.known;
  const milestones = [];
  const marks = (query.get('milestones') ?? '10000,100000,1000000,2000000,4000000').split(',').map(Number).filter((m) => m <= target);
  let next = 0, lastMs = 0;
  while (g.n < target && !g.failed) {
    const tb = performance.now();
    sim.step(batch);
    g = await sim.readGlobals();
    const dt = performance.now() - tb;
    lastMs = dt / batch;
    batch = Math.max(1, Math.min(400, Math.round(100 / Math.max(lastMs, 0.05))));
    while (next < marks.length && g.n >= marks[next]) {
      milestones.push({ target: marks[next], cells: g.n, step: Number(g.step), wall: (performance.now() - t0) / 1000, ms: lastMs });
      next++;
    }
    if (onProgress) await onProgress(sim, g, lastMs);
  }
  return { sim, g, wall: (performance.now() - t0) / 1000, milestones, lastMs };
}

// Takes exactly `steps` more steps.
async function advance(sim, steps, onProgress) {
  let left = steps;
  while (left > 0) {
    const batch = Math.min(left, 100);
    const t = performance.now();
    sim.step(batch);
    left -= batch;
    const g = await sim.readGlobals();
    if (onProgress) await onProgress(sim, g, (performance.now() - t) / batch);
    if (g.failed) break;
  }
}

// Runs exactly `steps` steps from the start state.
async function runSteps(device, start, steps, capacity, options, onProgress) {
  const sim = await Sim.create(device, capacity, options);
  sim.upload(start);
  await advance(sim, steps, onProgress);
  return sim;
}

// exact=1 / exact=0 in the query select the mode; without it the simulation chooses (sim.js, `exact`).
const simOptions = (query) => ({ exact: query.get('exact') === '1' ? true : query.get('exact') === '0' ? false : 'auto' });
function applyQueryParams(query, params) {
  for (const [k, v] of query) if (k.startsWith('p.')) params[k.slice(2)] = Number(v);
}

async function taskBench(device, query, log, hooks) {
  const target = Number(query.get('cells') ?? 1000000);
  const start = await fetchState(query.get('start') ?? '/runs/web_verify/start.cfs');
  applyQueryParams(query, start.params);
  const capacity = Math.ceil(target * 1.12) + 4096;
  start.params.maxCells = capacity;
  log(`bench: growing to ${target} cells (capacity ${capacity}), parameters: foodMax=${start.params.foodMax}, roi=${start.params.radiusOfInfluence}`);
  const r = await growTo(device, query, start, target, capacity, hooks?.onProgress, simOptions(query));
  for (const m of r.milestones)
    log(`milestone cells=${m.target} reached_cells=${m.cells} step=${m.step} wall_s=${m.wall.toFixed(2)} ms_per_step=${m.ms.toFixed(3)}`);
  log(`bench: ${r.g.n} cells at step ${r.g.step} after ${r.wall.toFixed(2)} s; ${(r.sim.allocated / 1048576).toFixed(0)} MiB of buffers ` +
    `(${(r.sim.allocated / capacity).toFixed(0)} bytes per cell)` + (r.g.failed ? `; STOPPED: ${FAIL[r.g.failCode]} at step ${r.g.failStep}` : ''));
  let ok = !r.g.failed || r.g.failCode === 4;
  if (r.g.n <= Number(query.get('checkbelow') ?? 1500000)) {
    const s = await r.sim.download();
    const check = checkInvariants(s);
    const link = linkStats(s);
    log(`bench: invariant checker ${check.ok ? 'ok' : 'FAILED: ' + check.message}; mean link ${link.mean.toFixed(4)} (std ${link.std.toFixed(4)}); ` +
      `divisions ${r.g.divisions}, clamped ${r.g.clampedTotal}`);
    ok = ok && check.ok;
  }
  if (hooks?.onFinished) hooks.onFinished(r.sim);
  else r.sim.destroy();
  log(`bench: ${ok ? 'DONE' : 'FAIL'}`);
  return ok;
}

// Same input twice must give the same state. The first run grows to the cell count (its batches
// depend on timing, so where it stops varies); the other runs take exactly as many steps.
// Any parameter can be set with p.<name>=<value>, and exact=1 selects exact mode.
async function taskRerun(device, query, log) {
  const target = Number(query.get('cells') ?? 200000);
  const start = await fetchState(query.get('start') ?? '/runs/web_verify/start.cfs');
  applyQueryParams(query, start.params);
  const capacity = Math.ceil(target * 1.25) + 4096;
  start.params.maxCells = capacity;
  const first = await growTo(device, query, start, target, capacity, null, simOptions(query));
  const a = await first.sim.download();
  first.sim.destroy();
  const steps = Number(a.step - start.step);
  const prints = [fingerprint(a)];
  log(`rerun 1: ${a.n} cells at step ${a.step}, fingerprint ${prints[0]}, checker ${checkInvariants(a).ok ? 'ok' : 'FAILED'}`);
  for (let run = 2; run <= 3; run++) {
    const sim = await runSteps(device, start, steps, capacity, simOptions(query));
    const s = await sim.download();
    sim.destroy();
    prints.push(fingerprint(s));
    log(`rerun ${run}: ${s.n} cells at step ${s.step}, fingerprint ${prints[run - 1]}, checker ${checkInvariants(s).ok ? 'ok' : 'FAILED'}`);
  }
  const same = prints.every((x) => x === prints[0]);
  log(`rerun: ${same ? 'identical -> PASS' : 'DIFFERENT -> FAIL'}`);
  return same;
}

// Many steps: forms of the gallery (web/app/gallery/gallery.json) grown in the page from the
// same start shape, seed and parameters to the step the CUDA backend stopped at, then compared
// with the gallery's cell count and, where the manifest names one, with a CUDA checkpoint of
// that step. In exact mode (the default here; mode=fast for the other) the state should be the
// CUDA state bit for bit. Manifest: scripts/web_verify_data.ps1 -Gallery.
async function taskGallery(device, query, log, hooks) {
  const manifest = await (await fetch(query.get('data') ?? '/runs/web_verify/gallery.json')).json();
  const modes = { fast: [false], exact: [true], both: [true, false] }[query.get('mode') ?? 'exact'];
  const only = query.get('ids') ? query.get('ids').split(',').map(Number) : null;
  let allOk = true;
  for (const item of manifest.items) {
    if (only && !only.includes(item.id)) continue;
    const mesh = await fetchState(item.mesh);
    const capacity = item.capacity ?? Math.floor(item.cells * 1.5) + 4096;
    const reference = item.reference ? await fetchState(item.reference) : null;
    // An item may have `legs`: [{ step, params }], the parameters changing (setParams) when
    // the step counter reaches the end of a leg.
    const legs = item.legs ?? [{ step: item.step, params: item.params }];
    for (const exact of modes) {
      const start = makeStart(mesh, { seed: item.seed, food: item.food, params: { ...legs[0].params, maxCells: capacity } });
      const t0 = performance.now();
      const sim = await Sim.create(device, capacity, { exact });
      sim.upload(start);
      let at = 0;
      for (const [k, leg] of legs.entries()) {
        if (k > 0) {
          const issues = sim.setParams({ ...mesh.params, ...leg.params, maxCells: capacity });
          if (!issues.ok) throw new Error(`gallery ${item.id}: ${issues.errors.join('; ')}`);
        }
        await advance(sim, leg.step - at, hooks?.onProgress);
        at = leg.step;
      }
      const got = await sim.download();
      const wall = (performance.now() - t0) / 1000;
      const g = sim.known;
      sim.destroy();
      const check = checkInvariants(got);
      let line = `gallery ${item.id} [${item.sources.join(', ') || 'base model'}]${exact ? ' [exact]' : ''}: step ${got.step} of ${item.step}: ${got.n} cells, ` +
        `CUDA ${item.cells} (${got.n === item.cells ? 'same' : (got.n > item.cells ? '+' : '') + (got.n - item.cells) + ', ' + (100 * (got.n - item.cells) / item.cells).toFixed(2) + '%'}), ` +
        `${wall.toFixed(1)} s, ${(1000 * wall / item.step).toFixed(2)} ms/step, checker ${check.ok ? 'ok' : check.message}`;
      let ok = check.ok && !g.failed && Number(got.step) === item.step;
      if (g.failed) line += `; STOPPED: ${FAIL[g.failCode]} at step ${g.failStep}`;
      if (reference && got.n === reference.n) {
        const c = compareStates(got, reference);
        const identical = c.rings === 0 && c.scalars === 0 && c.morph === 0 && c.posBits === 0 && c.nrmBits === 0 && c.morphPresent;
        line += `; against the CUDA checkpoint: ring slots differing ${c.rings}, food/birth/generation ${c.scalars}, morphogens ${c.morph}, ` +
          `positions ${c.posBits}${c.rings === 0 ? ` (max ${c.maxPos.toExponential(2)})` : ''}, normals ${c.nrmBits} -> ${identical ? 'IDENTICAL' : 'different'}`;
        if (exact) ok = ok && identical;
      } else if (reference) {
        line += `; the CUDA checkpoint has ${reference.n} cells`;
        if (exact) ok = false;
      }
      if (exact) ok = ok && got.n === item.cells;
      allOk = allOk && ok;
      log(line + (exact ? ` -> ${ok ? 'ok' : 'MISMATCH'}` : ''));
    }
  }
  log(`gallery: ${allOk ? 'PASS' : 'FAIL'}`);
  return allOk;
}

// The mechanisms as the search switches them on (scripts/explore.py), mid-range values.
export const MECHANISMS = {
  'base model': {},
  'field': { fieldAmount: 0.7, fieldScale: 20, fieldContrast: 2, fieldOctaves: 2 },
  'field on bulge': { fieldBulge: 0.5, fieldScale: 20, fieldOctaves: 2 },
  'pulses': { pulsePeriod: 300, pulseDuty: 0.5, pulseLow: 0.2 },
  'light': { lightAmount: 0.7, lightAmbient: 0.1, lightDirX: 0.4, lightDirY: 0.8, lightDirZ: 0.3 },
  'diffusion': { foodDiffusion: 0.25, foodDecay: 0.001 },
  'crowding': { crowdLow: 0.8, crowdHigh: 1.0 },
  'oriented division': { orientAmount: 0.7 },
  'morphogens': { morphogens: 1, morphFood: 0.5, morphBulge: 0.5, morphPlanar: -0.5, morphRest: 0.2 },
};
MECHANISMS['all of them'] = Object.assign({}, ...Object.values(MECHANISMS));

// Steps per second with each mechanism on, at given cell counts. A base-model run with fast
// food is grown to each size once; every mechanism then starts from that state and is timed
// over `steps` steps (after a few to warm up), `repeat` times, in both modes. So that every
// mechanism is timed at the same cell count, the food per step is next to nothing while the
// clock runs (the form stops growing; every pass of a step still runs in full). With grow=1
// the food stays at its default and the form grows by about half during 300 steps, by an
// amount that depends on the mechanism.
async function taskSpeed(device, query, log) {
  const sizes = (query.get('cells') ?? '30000,100000').split(',').map(Number);
  const steps = Number(query.get('steps') ?? 300), repeat = Number(query.get('repeat') ?? 2);
  const modes = { fast: [false], exact: [true], both: [false, true] }[query.get('mode') ?? 'both'];
  const names = query.get('only') ? query.get('only').split(',') : Object.keys(MECHANISMS);
  const growing = query.get('grow') === '1';
  for (const size of sizes) {
    const start = await fetchState(query.get('start') ?? '/runs/web_verify/start.cfs');
    const capacity = Math.ceil(size * (growing ? 2.5 : 1.2)) + 4096;
    start.params.maxCells = capacity;
    start.params.foodMax = 0.02;
    const grower = await Sim.create(device, capacity, { exact: false });
    grower.upload(start);
    let g = grower.known;
    while (g.n < size && !g.failed) {
      grower.step(g.n < 0.6 * size ? 20 : 1);
      g = await grower.readGlobals();
    }
    const state = await grower.download();
    grower.destroy();
    if (growing) delete state.params.foodMax;
    else state.params.foodMax = 1e-7;
    log(`speed: ${state.n} cells (grown with the base model to step ${state.step}); ${steps} steps timed, ${repeat} time(s), ` +
      (growing ? 'the form growing' : 'the cell count held'));
    const baseMs = {};
    for (const exact of modes) {
      for (const name of names) {
        const sim = await Sim.create(device, capacity, { exact });
        const ms = [];
        let cells = 0, passes = 0;
        for (let r = 0; r < repeat; r++) {
          sim.upload({ ...state, params: { ...state.params, ...MECHANISMS[name] } });
          sim.step(10);
          await sim.readGlobals();
          const t = performance.now();
          sim.step(steps);
          g = await sim.readGlobals();
          ms.push((performance.now() - t) / steps);
          cells = g.n;
          passes = sim.passesPerStep;
          if (g.failed) throw new Error(`speed ${name}: stopped: ${FAIL[g.failCode]}`);
        }
        sim.destroy();
        const best = Math.min(...ms);
        if (name === 'base model') baseMs[exact] = best;
        const against = name !== 'base model' && baseMs[exact] ? `, ${(best / baseMs[exact]).toFixed(2)} x the base model's time`
          : (exact && baseMs.false ? `, ${(best / baseMs.false).toFixed(2)} x the time without exact arithmetic` : '');
        log(`speed ${state.n} ${exact ? 'exact' : 'fast'} ${name}: ${ms.map((x) => (1000 / x).toFixed(0)).join(' / ')} steps/s (${ms.map((x) => x.toFixed(2)).join(' / ')} ms/step), ` +
          `${passes} passes, ${cells} cells at the end${against}`);
      }
    }
  }
  log('speed: DONE');
  return true;
}

// Runs the task named in the query. Returns true when everything passed.
export async function runTask(device, query, log, hooks) {
  const task = query.get('task');
  let ok = true;
  if (task === 'verify') ok = await taskVerify(device, query, log);
  else if (task === 'bench') ok = await taskBench(device, query, log, hooks);
  else if (task === 'rerun') ok = await taskRerun(device, query, log);
  else if (task === 'floats') ok = await taskFloats(device, query, log);
  else if (task === 'gallery') ok = await taskGallery(device, query, log, hooks);
  else if (task === 'speed') ok = await taskSpeed(device, query, log);
  else if (task === 'all') {
    ok = await taskFloats(device, query, log);
    ok = (await taskVerify(device, query, log)) && ok;
    ok = (await taskRerun(device, query, log)) && ok;
    ok = (await taskBench(device, query, log, hooks)) && ok;
  } else throw new Error(`unknown task '${task}'`);
  return ok;
}
