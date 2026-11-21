// Scripted tasks of the WebGPU port: verify against CPU checkpoints, rerun, benchmark.
// They take the device, the URL query, a log function and optional hooks, so they can run on
// the page or in a worker. (In a worker they keep their speed when the page is hidden; a
// hidden page has its timers and callbacks throttled to about one per second.)
import { Sim } from './sim.js';
import { parseCheckpoint, checkInvariants, linkStats, fingerprint, kMaxDeg } from './cfs.js';

const FAIL = ['', 'non-finite position', 'grid too large', 'non-finite new position', 'capacity reached'];

export async function fetchState(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error(`cannot load ${url}: ${r.status}`);
  return parseCheckpoint(await r.arrayBuffer());
}

export async function requestDevice() {
  if (!navigator.gpu) throw new Error('WebGPU is not available here');
  const adapter = await navigator.gpu.requestAdapter({ powerPreference: 'high-performance' });
  if (!adapter) throw new Error('no WebGPU adapter');
  const L = adapter.limits;
  const device = await adapter.requestDevice({
    requiredLimits: {
      maxStorageBufferBindingSize: L.maxStorageBufferBindingSize,
      maxBufferSize: L.maxBufferSize,
      maxStorageBuffersPerShaderStage: Math.min(L.maxStorageBuffersPerShaderStage, 10),
      maxComputeWorkgroupsPerDimension: L.maxComputeWorkgroupsPerDimension,
    },
  });
  const info = adapter.info ?? {};
  const describe = `adapter: ${info.vendor ?? '?'} ${info.architecture ?? ''} ${info.device ?? ''} ${info.description ?? ''}`.trim() +
    `\nlimits: buffer ${(L.maxBufferSize / 1048576).toFixed(0)} MiB, storage binding ${(L.maxStorageBufferBindingSize / 1048576).toFixed(0)} MiB, ` +
    `${L.maxStorageBuffersPerShaderStage} storage buffers per stage, ${L.maxComputeWorkgroupsPerDimension} workgroups per dimension`;
  return { device, describe };
}

// One step (or a few) from a CPU state, compared with the CPU state that many steps later.
async function taskVerify(device, query, log) {
  const manifest = await (await fetch(query.get('data') ?? '/runs/web_verify/manifest.json')).json();
  let allOk = true, compared = 0, worst = 0;
  for (const pair of manifest.pairs) {
    const before = await fetchState(pair.before), after = await fetchState(pair.after);
    const sim = await Sim.create(device, Math.max(after.n + 1024, 4096));
    sim.upload(before);
    const steps = Number(after.step - before.step);
    sim.step(steps);
    const got = await sim.download();
    sim.destroy();
    let rings = 0, scalars = 0, maxPos = 0, maxNrm = 0;
    if (got.n === after.n) {
      for (let v = 0; v < after.n; v++) {
        if (got.deg[v] !== after.deg[v]) rings++;
        for (let k = 0; k < kMaxDeg; k++) if (got.nbr[kMaxDeg * v + k] !== after.nbr[kMaxDeg * v + k]) rings++;
        if (got.food[v] !== after.food[v] || got.birth[v] !== after.birth[v] || got.gen[v] !== after.gen[v]) scalars++;
      }
      if (rings === 0) {
        for (let i = 0; i < 3 * after.n; i++) {
          maxPos = Math.max(maxPos, Math.abs(got.pos[i] - after.pos[i]));
          maxNrm = Math.max(maxNrm, Math.abs(got.normal[i] - after.normal[i]));
        }
      }
    }
    const check = checkInvariants(got);
    const ok = got.n === after.n && rings === 0 && scalars === 0 && maxPos <= 2e-4 && check.ok && got.step === after.step;
    allOk = allOk && ok;
    compared++;
    worst = Math.max(worst, maxPos);
    log(`verify ${pair.label}: ${before.n} -> ${got.n} cells (cpu ${after.n}), ${steps} step(s): ring slots differing ${rings}, ` +
      `food/birth/generation differing ${scalars}, max position diff ${maxPos.toExponential(2)}, max normal diff ${maxNrm.toExponential(2)}, ` +
      `checker ${check.ok ? 'ok' : check.message} -> ${ok ? 'ok' : 'MISMATCH'}`);
  }
  log(`verify: ${compared} comparisons, largest position difference ${worst.toExponential(2)} -> ${allOk ? 'PASS' : 'FAIL'}`);
  return allOk;
}

// Runs from the start state until the cell count reaches `target`. Steps go out in batches; the
// cell count is read back once per batch (about every 100 ms of simulation).
async function growTo(device, query, start, target, capacity, onProgress) {
  const sim = await Sim.create(device, capacity);
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

// Runs exactly `steps` steps from the start state.
async function runSteps(device, start, steps, capacity) {
  const sim = await Sim.create(device, capacity);
  sim.upload(start);
  let left = steps;
  while (left > 0) {
    const batch = Math.min(left, 100);
    sim.step(batch);
    left -= batch;
    await sim.readGlobals();
  }
  return sim;
}

async function taskBench(device, query, log, hooks) {
  const target = Number(query.get('cells') ?? 1000000);
  const start = await fetchState(query.get('start') ?? '/runs/web_verify/start.cfs');
  for (const [k, v] of query) if (k.startsWith('p.')) start.params[k.slice(2)] = Number(v);
  const capacity = Math.ceil(target * 1.12) + 4096;
  start.params.maxCells = capacity;
  log(`bench: growing to ${target} cells (capacity ${capacity}), parameters: foodMax=${start.params.foodMax}, roi=${start.params.radiusOfInfluence}`);
  const r = await growTo(device, query, start, target, capacity, hooks?.onProgress);
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
async function taskRerun(device, query, log) {
  const target = Number(query.get('cells') ?? 200000);
  const start = await fetchState(query.get('start') ?? '/runs/web_verify/start.cfs');
  const capacity = Math.ceil(target * 1.25) + 4096;
  start.params.maxCells = capacity;
  const first = await growTo(device, query, start, target, capacity, null);
  const a = await first.sim.download();
  first.sim.destroy();
  const steps = Number(a.step - start.step);
  const prints = [fingerprint(a)];
  log(`rerun 1: ${a.n} cells at step ${a.step}, fingerprint ${prints[0]}, checker ${checkInvariants(a).ok ? 'ok' : 'FAILED'}`);
  for (let run = 2; run <= 3; run++) {
    const sim = await runSteps(device, start, steps, capacity);
    const s = await sim.download();
    sim.destroy();
    prints.push(fingerprint(s));
    log(`rerun ${run}: ${s.n} cells at step ${s.step}, fingerprint ${prints[run - 1]}, checker ${checkInvariants(s).ok ? 'ok' : 'FAILED'}`);
  }
  const same = prints.every((x) => x === prints[0]);
  log(`rerun: ${same ? 'identical -> PASS' : 'DIFFERENT -> FAIL'}`);
  return same;
}

// Runs the task named in the query. Returns true when everything passed.
export async function runTask(device, query, log, hooks) {
  const task = query.get('task');
  let ok = true;
  if (task === 'verify') ok = await taskVerify(device, query, log);
  else if (task === 'bench') ok = await taskBench(device, query, log, hooks);
  else if (task === 'rerun') ok = await taskRerun(device, query, log);
  else if (task === 'all') {
    ok = await taskVerify(device, query, log);
    ok = (await taskRerun(device, query, log)) && ok;
    ok = (await taskBench(device, query, log, hooks)) && ok;
  } else throw new Error(`unknown task '${task}'`);
  return ok;
}
