// Cellular Forms, the web viewer: grows a form with the WebGPU simulation (../sim.js) and draws
// it from the simulation's own buffers (renderer.js). This file is the page: the run loop, the
// panel, the gallery view, links, and what is said when something goes wrong.
import * as SimModule from '../sim.js';
import { makeStart } from '../start.js';
import { parseCheckpoint } from '../cfs.js';
import { requestDevice } from '../tasks.js';
import { GROUPS, START_SHAPES } from './params.js';
import { Renderer } from './renderer.js';
import { OrbitCamera, attachCameraInput, CAMERA_DEFAULTS } from './camera.js';
import { LOOK, LOOK_DEFAULTS, DEFAULT_MATERIAL, toHex, fromHex } from './look.js';
import { MATERIALS, findMaterial, materialLook } from './materials.js';
import { encodeLink, decodeLink, SETUP_DEFAULTS, MODEL_DEFAULTS, START_PARAMS } from './link.js';
import { loadGallery, buildGallery, tileStyle, sourceGroup } from './gallery.js';
import { el, section, sliderRow, checkRow, selectRow, colorRow, numberRow, buttons, button, formatCount } from './ui.js';

window.cellularFormsStarted = true;

const { Sim } = SimModule;
const CAPACITIES = [50000, 100000, 250000, 500000, 1000000, 2000000];
const FAIL = ['', 'non-finite position', 'grid too large', 'non-finite new position', 'capacity reached'];
const READ_MS = 250;      // how often the cell count is read back from the GPU
const MAX_STEPS = 300;    // steps in one frame at most
const $ = (id) => document.getElementById(id);
const clamp = (x, lo, hi) => Math.min(Math.max(x, lo), hi);

// ------------------------------------------------------------------------------------ state
const state = {
  // What a run is made from. `gallery`: the id of the gallery form this still is;
  // `basedOn`: the gallery form it was before something was changed.
  setup: { ...SETUP_DEFAULTS, params: { ...START_PARAMS }, gallery: null, basedOn: null },
  look: structuredClone(LOOK_DEFAULTS),
  material: DEFAULT_MATERIAL,
  withColor: true,
  speed: { unlimited: true, limit: 60, budget: 14 },
  pauseAt: 0,            // pause when this step is reached (0: never)
  running: true,
  view: 'viewer',
};
const camera = new OrbitCamera();

// The engine: device, simulation, and what is known about the run.
const eng = {
  device: null, renderer: null, gpuName: '',
  sim: null,             // the simulation while it holds a run (null between runs, and when a run could not start)
  pool: null,            // the Sim object itself, kept from run to run while the capacity stays
  restarts: Promise.resolve(),  // restarts take turns
  epoch: 0,              // counts restarts, so that late answers of an old run are dropped
  known: null,           // the globals as last read: { n, step, failed, ... }
  bounds: null,          // { min, max } of the form
  reading: false, readPromise: null, lastRead: 0,
  submitted: 0,          // steps submitted in this run: the step it is at, while it has not stopped by itself
  pending: 0,            // frames submitted whose GPU work has not finished
  stepsPerFrame: 1, credit: 0, stepOnce: 0,
  paramsDirty: false, dirty: true, snap: true,
  restarting: false,     // between two runs: the view keeps its last picture
  pauseDone: false,      // the pause at state.pauseAt has been taken in this run
  stopped: null,         // { code, step } once the run has stopped by itself
  runError: null,        // why the setup could not be started
  missing: new Map(),    // switch parameter -> why the simulation here cannot do that mechanism
  gpuErrors: 0,          // WebGPU errors since the run was last started
  rate: { t: 0, step: 0 }, stepsPerSecond: 0, frameMs: 16.7, gpuMs: 0, lastFrame: 0, lastStatus: 0, lastStepAt: 0,
};
const ui = { params: {}, switches: [], look: [], lookById: {}, gallery: null, galleryData: null };
const canvas = $('view');

// ------------------------------------------------------------------------------------ notices
const notices = new Map();
function notify(kind, text, { id = text, actions = [], extra = null } = {}) {
  dismiss(id);
  const body = el('div', {}, el('p', { text }), extra);
  const all = [...actions, { label: 'Dismiss', run: () => {} }];
  body.append(el('div', { class: 'actions' }, all.map((a) => button(a.label, () => { dismiss(id); a.run(); }))));
  const node = el('div', { class: 'notice ' + kind }, body);
  notices.set(id, node);
  $('notices').append(node);
  if (kind === 'error') console.warn('[cellular forms] ' + text);
}
function dismiss(id) {
  notices.get(id)?.remove();
  notices.delete(id);
}

function fatal(title, lines, action) {
  clearFatal();
  const box = el('div', {}, el('h2', { text: title }), lines.map((t) => el('p', {}, t)));
  if (action) box.append(button(action.label, action.run));
  $('stage').append(el('div', { class: 'fatal', id: 'fatal', role: 'alert' }, box));
  $('stage-foot').hidden = true;
}
function clearFatal() {
  $('fatal')?.remove();
  $('stage-foot').hidden = false;
}

// ------------------------------------------------------------------------------------ start shapes
const shapes = { files: null, meshes: new Map() };
async function loadShape(shape) {
  if (shapes.meshes.has(shape)) return shapes.meshes.get(shape);
  if (!shapes.files) {
    const r = await fetch(new URL('starts/starts.json', import.meta.url));
    if (!r.ok) throw new Error(`the list of start shapes could not be loaded (${r.status})`);
    shapes.files = new Map((await r.json()).map((s) => [s.shape, s]));
  }
  const entry = shapes.files.get(shape);
  if (!entry) throw new Error(`there is no start shape '${shape}' on this server`);
  // The state file itself, or (where a host serves no binary files, as claude.ai artifacts) its
  // bytes in base64 inside starts.json.
  let bytes;
  if (entry.b64) {
    bytes = Uint8Array.from(atob(entry.b64), (c) => c.charCodeAt(0)).buffer;
  } else {
    const r = await fetch(new URL('starts/' + entry.file, import.meta.url));
    if (!r.ok) throw new Error(`start shape '${shape}' could not be loaded (${r.status})`);
    bytes = await r.arrayBuffer();
  }
  const mesh = parseCheckpoint(bytes);
  shapes.meshes.set(shape, mesh);
  return mesh;
}

function boundsOf(pos, n) {
  const min = [Infinity, Infinity, Infinity], max = [-Infinity, -Infinity, -Infinity];
  for (let v = 0; v < n; v++)
    for (let a = 0; a < 3; a++) {
      const x = pos[3 * v + a];
      if (x < min[a]) min[a] = x;
      if (x > max[a]) max[a] = x;
    }
  return { min, max };
}

// ------------------------------------------------------------------------------------ the run
// The parameters as the simulation gets them.
function simParams() {
  const p = { ...state.setup.params };
  if (p.foodMin > p.foodMax) p.foodMin = p.foodMax;
  p.maxCells = state.setup.capacity;
  return p;
}

// Starts the run again from the start shape. The Sim object is kept from run to run and made
// anew only when the capacity changes (its buffers have that size): a draw takes the cell count
// from the simulation's counter on the GPU, so whatever an earlier run left in the slots past
// the last cell is never drawn. Restarts take turns, and one that a newer one has overtaken is
// skipped.
function restart() {
  const epoch = ++eng.epoch;
  eng.sim = null;
  eng.known = null;
  eng.stopped = null;
  eng.runError = null;
  eng.restarting = true;
  eng.gpuErrors = 0;
  for (const id of ['run', 'stopped', 'params', 'gpu', 'pause', 'read']) dismiss(id);
  updateRunButtons();
  const turn = () => (epoch === eng.epoch ? startRun(epoch) : undefined);
  eng.restarts = eng.restarts.then(turn, turn);
  return eng.restarts;
}

async function startRun(epoch) {
  const device = eng.device;
  if (!device) return;
  try {
    const { shape, food, seed, capacity } = state.setup;
    // Exact arithmetic when asked for; otherwise the simulation decides (exact as soon as
    // anything beyond the base model is on).
    const exact = state.setup.exact ? true : 'auto';
    const mesh = await loadShape(shape);
    if (mesh.n > capacity) throw new Error(`the start shape has ${formatCount(mesh.n)} cells, more than the capacity of ${formatCount(capacity)}`);
    let sim = eng.pool;
    if (sim && (sim.capacity !== capacity || sim.device !== device)) {
      if (eng.readPromise) await eng.readPromise;
      sim.destroy();
      sim = eng.pool = null;
    }
    if (!sim) sim = eng.pool = await Sim.create(device, capacity, { exact });
    else sim.exact = exact;
    if (device !== eng.device) return;
    const params = simParams();
    // Throws when the simulation's check of the parameters finds an error.
    sim.upload(makeStart(mesh, { seed, food, params, defaults: MODEL_DEFAULTS }));
    if (epoch !== eng.epoch) return;  // a newer restart is waiting for its turn
    eng.sim = sim;
    eng.known = { n: mesh.n, step: 0n, failed: 0 };
    eng.pauseDone = false;
    const issues = SimModule.validateParams?.(params);
    if (issues?.warnings?.length) notify('warn', `The simulation warns: ${issues.warnings.join('; ')}.`, { id: 'params' });
    eng.bounds = boundsOf(mesh.pos, mesh.n);
    Object.assign(eng, { reading: false, submitted: 0, credit: 0, stepsPerFrame: 1, stepOnce: 0, paramsDirty: false, snap: true, stepsPerSecond: 0, lastRead: performance.now() });
    eng.rate = { t: 0, step: 0 };
  } catch (e) {
    if (epoch !== eng.epoch) return;
    eng.runError = e.message ?? String(e);
    const mechanisms = GROUPS.some((g) => g.switch && state.setup.params[g.switch.name] !== g.switch.off);
    notify('error', `This setup cannot be grown here: ${eng.runError}.`, {
      id: 'run',
      actions: mechanisms ? [{ label: 'Switch the optional mechanisms off', run: switchMechanismsOff }] : [],
    });
  }
  if (epoch !== eng.epoch) return;
  eng.restarting = false;
  eng.dirty = true;
  updateRunButtons();
  updateStatus();
  noteMechanisms();
}

function switchMechanismsOff() {
  for (const g of GROUPS) if (g.switch) state.setup.params[g.switch.name] = g.switch.off;
  setupChanged();
  syncParams();
  restart();
}

// Asks a small simulation which optional mechanisms it can run, so the page can say so where
// one is switched on that the simulation does not have (yet).
async function probeMechanisms() {
  const device = eng.device;
  try {
    const mesh = await loadShape('icosphere:0');
    const sim = await Sim.create(device, 1024);
    const missing = new Map();
    for (const g of GROUPS) {
      if (!g.switch) continue;
      try {
        sim.upload(makeStart(mesh, { seed: 1, food: 'zero', params: { ...MODEL_DEFAULTS, [g.switch.name]: g.switch.on, maxCells: 1024 }, defaults: MODEL_DEFAULTS }));
      } catch (e) {
        missing.set(g.switch.name, e.message ?? String(e));
      }
    }
    sim.destroy();
    if (device !== eng.device) return;
    eng.missing = missing;
  } catch (e) {
    console.warn('[cellular forms] could not ask the simulation about its mechanisms: ' + (e.message ?? e));
    return;
  }
  for (const s of ui.switches) s.note.hidden = !eng.missing.has(s.group.switch.name);
  ui.gallery?.setRunnableFilter(eng.missing.size > 0);
  ui.gallery?.refresh();
  noteMechanisms();
}

// A mechanism that is switched on while the run goes on, in a simulation that lacks it, does
// nothing: say so instead of leaving the switch a silent dud.
function noteMechanisms() {
  const lacking = GROUPS.filter((g) => g.switch && state.setup.params[g.switch.name] !== g.switch.off && eng.missing.has(g.switch.name));
  if (lacking.length && eng.sim && !eng.runError) {
    notify('warn', `${lacking.map((g) => g.title).join(', ')}: switched on, but the simulation of this page does not have ${lacking.length > 1 ? 'these mechanisms' : 'this mechanism'} yet. ` +
      'It has no effect on this run.', { id: 'mechanisms' });
  } else dismiss('mechanisms');
}

function pushParams() {
  eng.paramsDirty = false;
  try {
    // The simulation checks the parameters: with an error it keeps the ones it had.
    const issues = eng.sim.setParams(simParams());
    if (issues && issues.ok === false) notify('error', `These parameters were not taken, the run goes on with the ones before: ${issues.errors.join('; ')}.`, { id: 'params' });
    else if (issues?.warnings?.length) notify('warn', `The simulation warns: ${issues.warnings.join('; ')}.`, { id: 'params' });
    else dismiss('params');
  } catch (e) {
    notify('error', `The parameters could not be changed while it runs: ${e.message ?? e}.`, { id: 'params' });
  }
  noteMechanisms();
}

// Asks for the counters of the run. Steps go on while the answer is on its way.
function startRead() {
  const sim = eng.sim, epoch = eng.epoch;
  eng.reading = true;
  const promise = sim.readGlobals().then(
    (g) => {
      if (epoch === eng.epoch) onGlobals(g);
    },
    (e) => {
      if (epoch === eng.epoch && eng.device) notify('error', `The state of the run could not be read: ${e.message ?? e}.`, { id: 'read' });
    },
  ).finally(() => {
    if (eng.readPromise === promise) eng.readPromise = null;
    if (epoch === eng.epoch) {
      eng.reading = false;
      eng.lastRead = performance.now();
    }
  });
  eng.readPromise = promise;
}

function onGlobals(g) {
  const now = performance.now(), step = Number(g.step);
  if (eng.rate.t && now > eng.rate.t) {
    const rate = ((step - eng.rate.step) * 1000) / (now - eng.rate.t);
    eng.stepsPerSecond = eng.stepsPerSecond ? 0.5 * eng.stepsPerSecond + 0.5 * rate : rate;
  }
  eng.rate = { t: now, step };
  eng.known = g;
  const ok = (v) => Array.isArray(v) && v.every(Number.isFinite);
  if (step > 0 && ok(g.boundsMin) && ok(g.boundsMax) && g.boundsMin.every((x, i) => x <= g.boundsMax[i])) eng.bounds = { min: g.boundsMin, max: g.boundsMax };
  if (g.failed && !eng.stopped) {
    eng.stopped = { code: g.failCode, step: g.failStep };
    const what = FAIL[g.failCode] || 'unknown';
    if (g.failCode === 4) {
      notify('warn', `Stopped at step ${formatCount(g.failStep)}: capacity reached (code 4). The buffers hold ${formatCount(state.setup.capacity)} cells. ` +
        'Choose a larger capacity under Simulation to grow further; the run then starts again.', { id: 'stopped' });
    } else {
      notify('error', `Stopped at step ${formatCount(g.failStep)}: the simulation reported a failure, code ${g.failCode} (${what}). The last good state is shown.`, { id: 'stopped' });
    }
    updateRunButtons();
  }
  eng.dirty = true;
  updateStatus();
}

function sceneSphere() {
  const b = eng.bounds;
  if (!b) return { centre: [0, 0, 0], radius: 5 };
  const centre = [0, 1, 2].map((i) => (b.min[i] + b.max[i]) / 2);
  return { centre, radius: Math.max(0.5 * Math.hypot(b.max[0] - b.min[0], b.max[1] - b.min[1], b.max[2] - b.min[2]), 1) };
}

// The step the run is at: every step submitted is one step further (a run starts at step 0),
// until the simulation stops by itself.
function stepNow() {
  if (!eng.known) return 0;
  return eng.stopped ? Number(eng.known.step) : eng.submitted;
}

function draw() {
  const sim = eng.sim, known = eng.known;
  // How many cells there are is taken on the GPU when the draw runs (renderer.js); the count
  // here is the one read last, for the ranges of the colours.
  eng.renderer.draw({
    buffers: sim && known ? sim.renderBuffers() : null, look: state.look, camera, scene: sceneSphere(),
    step: stepNow(), cells: known ? known.n : 0, params: simParams(),
  });
}

function frame(now) {
  requestAnimationFrame(frame);
  const dt = clamp(now - eng.lastFrame, 0, 250);
  eng.lastFrame = now;
  eng.frameMs += (dt - eng.frameMs) * 0.08;
  if (!eng.renderer || state.view !== 'viewer') return;

  const dpr = Math.min(window.devicePixelRatio || 1, 2);
  const w = Math.max(1, Math.round(canvas.clientWidth * dpr)), h = Math.max(1, Math.round(canvas.clientHeight * dpr));
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
    eng.dirty = true;
  }

  const sim = eng.sim;
  const t0 = performance.now();
  let stepped = 0;
  const allot = Math.max(1, Math.round(eng.stepsPerFrame));
  if (sim && eng.pending < 3) {
    if (eng.paramsDirty) pushParams();
    const wanted = !eng.stopped && (state.running || eng.stepOnce > 0);
    // With a limit on the steps per second: steps are owed by the time that passes, also in
    // the frames that cannot step, and made up for afterwards (up to a quarter of a second).
    if (state.running && !eng.stopped && !state.speed.unlimited) {
      eng.credit = Math.min(eng.credit + (state.speed.limit * dt) / 1000, Math.max(allot, state.speed.limit * 0.25, 1));
    }
    // No steps while the GPU has not caught up with the frames before.
    if (wanted && eng.pending < 2) {
      let n;
      const single = eng.stepOnce > 0;
      if (single) {
        n = eng.stepOnce;
        eng.stepOnce = 0;
      } else if (state.speed.unlimited) n = allot;
      else {
        n = Math.min(Math.floor(eng.credit), allot);
        eng.credit -= n;
      }
      // Not past the step at which a pause was asked for.
      if (state.pauseAt > 0 && !eng.pauseDone && !single) {
        const room = state.pauseAt - eng.submitted;
        if (room < 0) eng.pauseDone = true;  // the run is past it already
        else {
          n = Math.min(n, room);
          if (n === room) pauseAtStep();
        }
      }
      if (n > 0) {
        sim.step(n);
        stepped = n;
        eng.submitted += n;
        eng.lastStepAt = now;
        eng.dirty = true;
      }
    }
  }

  // The camera: a slow turn, and following the form as it grows.
  if (camera.spin !== 0) {
    camera.yaw += (camera.spin * Math.PI) / 180 * (dt / 1000);
    eng.dirty = true;
  }
  const scene = sceneSphere();
  const cellRadius = state.look.draw === 'membrane' ? 0.55 : state.look.radius;
  if (camera.track(scene.centre, scene.radius + cellRadius, w / h, eng.snap ? 1 : 1 - Math.exp(-dt / 160))) eng.dirty = true;
  eng.snap = false;

  let drew = false;
  if (eng.dirty && eng.pending < 3 && !eng.restarting) {
    draw();
    eng.dirty = false;
    drew = true;
  }
  if (stepped || drew) {
    eng.pending++;
    eng.device.queue.onSubmittedWorkDone().then(() => {
      eng.pending--;
      const ms = performance.now() - t0;
      eng.gpuMs += (ms - eng.gpuMs) * 0.2;
      if (!stepped) return;
      // Steer the steps per frame towards the time a frame may take.
      const budget = state.speed.budget;
      if (stepped >= allot) eng.stepsPerFrame = clamp(eng.stepsPerFrame * clamp(budget / Math.max(ms, 0.25), 0.7, 1.3), 1, MAX_STEPS);
      else if (ms > budget) eng.stepsPerFrame = clamp((stepped * budget) / ms, 1, MAX_STEPS);
    }, () => { eng.pending--; });
  }

  // The counters are read a few times a second, while they are behind the steps submitted.
  if (sim && !eng.reading && !eng.stopped && eng.submitted > Number(eng.known.step) && now - eng.lastRead >= READ_MS) startRead();
  if (now - eng.lastStepAt > 1000) eng.stepsPerSecond = 0;
  if (now - eng.lastStatus > 250) updateStatus();
}

// ------------------------------------------------------------------------------------ status
function updateStatus() {
  eng.lastStatus = performance.now();
  const status = $('status');
  if (!eng.device) return;
  const parts = [];
  const sep = () => el('span', { class: 'sep', text: '·' });
  const add = (node) => {
    if (parts.length) parts.push(sep());
    parts.push(node);
  };
  const k = eng.known;
  if (eng.runError) add(el('span', { class: 'stopped', text: 'Not running' }));
  else if (!k) add(el('span', { text: 'Preparing ...' }));
  else {
    if (eng.stopped) {
      const what = FAIL[eng.stopped.code] || 'failure';
      add(el('span', { class: 'stopped', text: `Stopped at step ${formatCount(eng.stopped.step)}: ${what} (code ${eng.stopped.code})` }));
    } else if (!state.running) add(el('span', { class: 'dim', text: 'Paused' }));
    add(el('span', { text: `${formatCount(k.n)} cells` }));
    add(el('span', { text: `step ${formatCount(Number(k.step))}` }));
    if (state.running && !eng.stopped) add(el('span', { text: `${eng.stepsPerSecond >= 100 ? Math.round(eng.stepsPerSecond) : eng.stepsPerSecond.toFixed(1)} steps/s` }));
  }
  add(el('span', { text: `${eng.frameMs.toFixed(1)} ms/frame` }));
  if (eng.gpuName) add(el('span', { class: 'dim', text: eng.gpuName }));
  status.replaceChildren(...parts);
  lookNotes();
}

function updateRunButtons() {
  const stopped = !!eng.stopped || !!eng.runError;
  for (const b of document.querySelectorAll('[data-act="run"]')) {
    b.textContent = stopped ? 'Stopped' : state.running ? 'Pause' : 'Run';
    b.disabled = stopped;
    b.setAttribute('aria-pressed', String(state.running && !stopped));
  }
  for (const b of document.querySelectorAll('[data-act="step"]')) b.disabled = stopped;
}

function pauseAtStep() {
  eng.pauseDone = true;
  setRunning(false);
  const item = state.setup.gallery != null ? galleryItem(state.setup.gallery) : null;
  const text = item && item.step === state.pauseAt
    ? `Paused at step ${formatCount(state.pauseAt)}, where the picture of gallery form ${item.id} was taken (${formatCount(item.cells)} cells there). Run grows it further.`
    : `Paused at step ${formatCount(state.pauseAt)}, as set under Simulation. Run goes on.`;
  notify('info', text, { id: 'pause' });
}

function setRunning(on) {
  state.running = on;
  eng.credit = 0;
  if (on) {
    eng.gpuErrors = 0;
    dismiss('gpu');
    dismiss('pause');
  }
  updateRunButtons();
  updateStatus();
}

// ------------------------------------------------------------------------------------ gallery forms
function galleryItem(id) {
  return ui.galleryData?.byId.get(id) ?? null;
}
function sameAsItem(item) {
  const s = state.setup;
  if (!item || s.shape !== item.init || s.food !== item.food || s.seed !== item.seed) return false;
  const want = { ...MODEL_DEFAULTS, ...item.params };
  return Object.keys(want).every((k) => Math.fround(s.params[k]) === Math.fround(want[k]));
}

// Called after anything of the setup changed: is it still the gallery form it came from?
function setupChanged() {
  const s = state.setup;
  const id = s.gallery ?? s.basedOn;
  if (id != null) {
    const same = sameAsItem(galleryItem(id));
    s.gallery = same ? id : null;
    s.basedOn = same ? null : id;
  }
  showOrigin();
}

function showOrigin() {
  const s = state.setup, box = $('origin');
  const id = s.gallery ?? s.basedOn;
  const item = id != null ? galleryItem(id) : null;
  ui.gallery?.setCurrent(s.gallery);
  if (!item) {
    box.hidden = true;
    return;
  }
  const pic = el('span', { class: 'pic', role: 'img', 'aria-label': `picture of form ${item.id}` });
  Object.assign(pic.style, tileStyle(ui.galleryData, item));
  const kind = ui.galleryData.kindName.get(item.kind) ?? '';
  const first = s.gallery != null ? `Growing gallery form ${item.id}` : `Based on gallery form ${item.id}, changed since`;
  box.replaceChildren(pic, el('span', {}, el('span', { text: first }), el('br'),
    el('span', { class: 'dim', text: `${kind}; pictured at ${formatCount(item.cells)} cells, step ${formatCount(item.step)}` })));
  box.hidden = false;
}

function missingFor(item) {
  return item.sources.filter((s) => {
    const g = sourceGroup(s);
    return g && eng.missing.has(g.switch.name);
  });
}

function chooseItem(item) {
  // Exact arithmetic and a pause at the step of its picture: the form grows into the very
  // form the gallery shows.
  Object.assign(state.setup, { shape: item.init, food: item.food, seed: item.seed, params: { ...MODEL_DEFAULTS, ...item.params }, gallery: item.id, basedOn: null, exact: true });
  state.pauseAt = item.step;
  Object.assign(camera, { follow: true, zoom: 1 });
  eng.snap = true;
  syncSetup();
  syncCamera();
  showOrigin();
  setView('viewer');
  state.running = true;
  restart();
}

async function setView(view) {
  state.view = view;
  $('app').dataset.view = view;
  $('gallery').hidden = view !== 'gallery';
  $('tab-viewer').setAttribute('aria-pressed', String(view === 'viewer'));
  $('tab-gallery').setAttribute('aria-pressed', String(view === 'gallery'));
  for (const id of ['copy-link', 'save-picture', 'toggle-panel']) $(id).hidden = view !== 'viewer';
  eng.dirty = true;
  if (view !== 'gallery' || ui.gallery) return;
  const root = $('gallery');
  root.replaceChildren(el('p', { class: 'gallery-intro', text: 'Loading the gallery ...' }));
  try {
    const data = await ensureGallery();
    ui.gallery = buildGallery(root, data, { onChoose: chooseItem, missing: missingFor });
    ui.gallery.setRunnableFilter(eng.missing.size > 0);
    ui.gallery.setCurrent(state.setup.gallery);
  } catch (e) {
    root.replaceChildren(el('p', { class: 'gallery-intro', text: `${e.message ?? e}.` }), button('Try again', () => setView('gallery')));
  }
}

async function ensureGallery() {
  ui.galleryData ??= await loadGallery();
  return ui.galleryData;
}

// ------------------------------------------------------------------------------------ links
function currentLink() {
  const hash = encodeLink({ setup: state.setup, look: state.look, material: state.material, camera });
  return location.origin + location.pathname + location.search + (hash ? '#' + hash : '');
}

async function copyLink() {
  const url = currentLink();
  try {
    history.replaceState(null, '', url);
  } catch { /* a sandboxed page may not rewrite its address: the link is still copied */ }
  try {
    await navigator.clipboard.writeText(url);
    notify('info', 'Link copied. It opens this setup: start, seed, parameters, look and camera.', { id: 'link' });
    setTimeout(() => dismiss('link'), 4000);
  } catch {
    const field = el('input', { type: 'text', readOnly: true, value: url, 'aria-label': 'Link to this setup' });
    notify('info', 'The browser did not allow copying. Copy the link from here:', { id: 'link', extra: field });
    field.focus();
    field.select();
  }
}

// Takes a setup from a link (the hash of the address) into the state. Returns false when there is none.
async function applyLink(hash) {
  const link = decodeLink(hash);
  if (!link) return false;
  const s = state.setup;
  // What the link does not name is the model's own default (not what the panel starts with).
  Object.assign(s, { ...SETUP_DEFAULTS, params: { ...MODEL_DEFAULTS }, gallery: null, basedOn: null });
  state.pauseAt = 0;
  const problems = [...link.problems];
  if (link.gallery != null) {
    try {
      const item = (await ensureGallery()).byId.get(link.gallery);
      if (item) {
        Object.assign(s, { shape: item.init, food: item.food, seed: item.seed, params: { ...MODEL_DEFAULTS, ...item.params }, gallery: item.id, exact: true });
        state.pauseAt = item.step;
      }
      else problems.push(`there is no gallery form ${link.gallery}`);
    } catch (e) {
      problems.push(e.message ?? String(e));
    }
  } else {
    if (link.setup.shape) s.shape = link.setup.shape;
    if (link.setup.food) s.food = link.setup.food;
    if (link.setup.seed !== undefined) s.seed = link.setup.seed;
    Object.assign(s.params, link.setup.params ?? {});
  }
  if (link.setup.exact !== undefined) s.exact = link.setup.exact;
  if (link.setup.capacity) s.capacity = CAPACITIES.find((c) => c >= link.setup.capacity) ?? CAPACITIES[CAPACITIES.length - 1];
  state.look = link.look;
  state.material = link.material;
  Object.assign(camera, CAMERA_DEFAULTS);
  if (link.camera) {
    const { target, ...rest } = link.camera;
    Object.assign(camera, rest);
    if (target) camera.target = [...target];
  }
  if (problems.length) notify('warn', `The link was not understood in full: ${problems.join('; ')}.`, { id: 'link-problems' });
  return true;
}

async function savePicture() {
  if (!eng.renderer) return;
  try {
    draw();  // the canvas can only be read in the task that drew it
    const blob = await eng.renderer.png();
    const k = eng.known;
    const a = el('a', { href: URL.createObjectURL(blob), download: `cellular-form-${k ? k.n : 0}-cells-step-${k ? Number(k.step) : 0}.png` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(a.href), 2000);
  } catch (e) {
    notify('error', `The picture could not be saved: ${e.message ?? e}.`, { id: 'picture' });
  }
}

// ------------------------------------------------------------------------------------ the panel
function setParam(name, value) {
  const p = state.setup.params;
  p[name] = value;
  // The least food per step cannot be above the most.
  if (name === 'foodMax' && p.foodMin > value) {
    p.foodMin = value;
    ui.params.foodMin?.set(value);
  }
  if (name === 'foodMin' && value > p.foodMax) {
    p.foodMax = value;
    ui.params.foodMax?.set(value);
  }
  eng.paramsDirty = true;
  setupChanged();
  syncSwitches();
}

function syncSwitches() {
  for (const s of ui.switches) {
    const on = state.setup.params[s.group.switch.name] !== s.group.switch.off;
    s.box.checked = on;
    s.fields.classList.toggle('off', !on);
    s.state.textContent = on ? 'on' : 'off';
  }
}

function syncParams() {
  for (const [name, row] of Object.entries(ui.params)) row.set(state.setup.params[name]);
  syncSwitches();
}

// Shows the whole setup in the panel (after a link, a gallery form, a reset).
function syncSetup() {
  syncParams();
  ui.seed.set(state.setup.seed);
  ui.shape.set(state.setup.shape);
  ui.food.set(state.setup.food);
  ui.capacity.set(state.setup.capacity);
  ui.exact.set(state.setup.exact);
  ui.pauseAt.set(state.pauseAt);
}

function syncLook() {
  for (const c of ui.look) c.row.set(Array.isArray(state.look[c.spec.id]) ? toHex(state.look[c.spec.id]) : state.look[c.spec.id]);
  ui.material.set(state.material);
  ui.materialNote.textContent = findMaterial(state.material)?.note ?? '';
  lookVisibility();
  eng.dirty = true;
}

function lookVisibility() {
  for (const c of ui.look) c.row.root.hidden = c.spec.when ? !c.spec.when(state.look) : false;
  lookNotes();
}

// Says so where the look asks for the morphogen activator and the run has no morphogens.
function lookNotes() {
  if (!ui.colourNote) return;
  const l = state.look;
  const without = !(eng.sim && eng.sim.renderBuffers().morph);
  ui.colourNote.hidden = !(without && l.colorBy === 'activator');
  ui.driveNote.hidden = !(without && [l.drive0, l.drive1, l.drive2, l.drive3].includes('activator'));
}

function syncCamera() {
  ui.follow.set(camera.follow);
  ui.spin.set(camera.spin);
  ui.fov.set(camera.fov);
}

function buildSimulation() {
  const sec = section('Simulation', { open: true });
  const run = button('Pause', () => setRunning(!state.running), { 'data-act': 'run', class: 'primary' });
  sec.body.append(buttons(
    run,
    button('Step', () => { eng.stepOnce = 1; }, { 'data-act': 'step', title: 'One step of the simulation' }),
    button('Restart', () => restart(), { title: 'Start again from the start shape, with the same seed' }),
    button('New seed', () => {
      state.setup.seed = 1 + Math.floor(Math.random() * 999999);
      ui.seed.set(state.setup.seed);
      setupChanged();
      restart();
    }, { title: 'Start again with another seed: the same parameters, another form' }),
  ));

  ui.seed = numberRow({
    label: 'seed', about: 'The same seed, start and parameters give the same form', value: state.setup.seed, min: 0, max: 4294967295,
    onChange: (v) => { state.setup.seed = v; ui.seed.set(v); setupChanged(); restart(); },
  });
  ui.shape = selectRow({
    label: 'start shape', options: START_SHAPES.map((s) => [s.shape, s.label]), value: state.setup.shape,
    onChange: (v) => { state.setup.shape = v; setupChanged(); restart(); },
  });
  ui.food = selectRow({
    label: 'first food', about: 'What the cells hold at the start: a random amount each (they divide at different times), or nothing',
    options: [['random', 'random'], ['zero', 'none']], value: state.setup.food,
    onChange: (v) => { state.setup.food = v; setupChanged(); restart(); },
  });
  ui.capacity = selectRow({
    label: 'capacity', about: 'How many cells the buffers hold; the run stops there. About 180 bytes of GPU memory per cell. A new capacity starts the run again',
    options: CAPACITIES.map((c) => [c, formatCount(c) + ' cells']), value: state.setup.capacity,
    onChange: (v) => { state.setup.capacity = Number(v); restart(); },
  });
  ui.exact = checkRow({
    label: 'exact arithmetic', value: state.setup.exact,
    about: 'Every step computed exactly as the PC version does, so the same start, seed and parameters give the same form cell for cell, the form of a gallery picture too. A little slower. ' +
      'Without it the simulation decides: exact as soon as a mechanism beyond the base model is on, the faster arithmetic of the GPU otherwise (a form of the same kind, not the same form). Applies from the next step',
    onChange: (v) => {
      state.setup.exact = v;
      if (eng.sim) eng.sim.exact = v ? true : 'auto';
    },
  });
  ui.pauseAt = numberRow({
    label: 'pause at step', about: 'The run pauses when it reaches this step; 0: never. A gallery form pauses at the step of its picture', value: state.pauseAt, min: 0, max: 100000000,
    onChange: (v) => { state.pauseAt = v; ui.pauseAt.set(v); eng.pauseDone = false; },
  });
  sec.body.append(ui.seed.root, ui.shape.root, ui.food.root, ui.capacity.root, ui.exact.root, ui.pauseAt.root);

  sec.body.append(el('h3', { text: 'Speed' }));
  const limit = sliderRow({
    label: 'steps per second', about: 'At most this many steps per second', min: 1, max: 2000, step: 1, log: true, value: state.speed.limit,
    onInput: (v) => { state.speed.limit = Math.round(v); },
  });
  const budget = sliderRow({
    label: 'time per frame', about: 'Milliseconds the simulation and the drawing may take per frame: lower keeps the view smooth, higher grows faster',
    min: 6, max: 40, step: 1, value: state.speed.budget, onInput: (v) => { state.speed.budget = v; },
  });
  const unlimited = checkRow({
    label: 'as fast as it goes', about: 'As many steps per frame as fit in the time per frame', value: state.speed.unlimited,
    onChange: (v) => { state.speed.unlimited = v; limit.root.hidden = v; eng.credit = 0; },
  });
  limit.root.hidden = state.speed.unlimited;
  sec.body.append(unlimited.root, limit.root, budget.root);
  return sec.root;
}

function paramRow(p) {
  const row = sliderRow({
    label: p.label, about: p.about, min: p.min, max: p.max, step: p.step, log: p.log, value: state.setup.params[p.name],
    onInput: (v) => setParam(p.name, v),
  });
  ui.params[p.name] = row;
  return row.root;
}

function buildParameters() {
  const sec = section('Parameters', { open: true, tag: 'applied while it runs' });
  for (const g of GROUPS) {
    if (!g.switch) {
      sec.body.append(el('h3', { text: g.title }), el('p', { class: 'about', text: g.about }), ...g.params.map(paramRow));
      continue;
    }
    // An optional mechanism: its switch, then its parameters.
    const id = 'switch-' + g.switch.name;
    const lastOn = { value: g.switch.on };
    const box = el('input', { type: 'checkbox', id });
    const stateEl = el('span', { class: 'state' });
    const fields = el('fieldset', { class: 'group' }, g.params.map(paramRow));
    const note = el('p', { class: 'note', hidden: true, text: 'Not in the simulation of this page yet: switched on, it does nothing here.' });
    box.addEventListener('change', () => {
      const p = state.setup.params;
      if (box.checked) {
        // While a mechanism is off its other parameters are the model's defaults; where the
        // panel has another starting value for one (params.js), it is taken now.
        for (const q of g.params) {
          if (q.name === g.switch.name || Math.fround(q.value) === Math.fround(MODEL_DEFAULTS[q.name])) continue;
          if (Math.fround(p[q.name]) === Math.fround(MODEL_DEFAULTS[q.name])) {
            p[q.name] = q.value;
            ui.params[q.name]?.set(q.value);
          }
        }
        setParam(g.switch.name, lastOn.value);
      } else {
        if (p[g.switch.name] !== g.switch.off) lastOn.value = p[g.switch.name];
        setParam(g.switch.name, g.switch.off);
      }
      ui.params[g.switch.name]?.set(p[g.switch.name]);
    });
    ui.switches.push({ group: g, box, fields, state: stateEl, note });
    sec.body.append(el('div', { class: 'group-head' }, box, el('label', { htmlFor: id, text: g.title }), stateEl),
      el('p', { class: 'about', text: g.about }), note, fields);
  }
  sec.body.append(el('h3', { text: 'All parameters' }), buttons(button('Back to defaults', () => {
    state.setup.params = { ...START_PARAMS };
    eng.paramsDirty = true;
    setupChanged();
    syncParams();
  }, { title: 'Every parameter back to its default; the run goes on' })));
  return sec.root;
}

const LOOK_SECTIONS = [
  ['draw', 'Draw', true], ['colour', 'Colour', false], ['material', 'Material', false], ['light', 'Light', false],
  ['shade', 'Shadows and occlusion', false], ['picture', 'Picture', false],
];

function lookRow(spec) {
  const value = state.look[spec.id];
  const change = (v) => {
    state.look[spec.id] = v;
    eng.dirty = true;
    lookVisibility();
  };
  let row;
  if (spec.options) row = selectRow({ label: spec.label, about: spec.about, options: spec.options, value, onChange: change });
  else if (spec.color) row = colorRow({ label: spec.label, about: spec.about, value: toHex(value), onInput: (hex) => change(fromHex(hex) ?? value) });
  else if (typeof spec.def === 'boolean') row = checkRow({ label: spec.label, about: spec.about, value, onChange: change });
  else row = sliderRow({ label: spec.label, about: spec.about, min: spec.min, max: spec.max, step: spec.step, log: spec.log, value, onInput: change });
  const entry = { spec, row };
  ui.look.push(entry);
  ui.lookById[spec.id] = entry;
  return row.root;
}

function buildLook() {
  return LOOK_SECTIONS.map(([id, title, open]) => {
    const sec = section(title, { open });
    if (id === 'material') {
      ui.material = selectRow({
        label: 'library', about: 'A material sets everything below; change any part of it afterwards', options: MATERIALS.map((m) => [m.name, m.name]),
        value: state.material, onChange: (name) => applyMaterial(name),
      });
      ui.materialNote = el('p', { class: 'about' });
      const withColor = checkRow({
        label: 'with its colour', about: 'Also colour every cell in the material\'s own colour', value: state.withColor,
        onChange: (v) => { state.withColor = v; applyMaterial(state.material); },
      });
      sec.body.append(ui.material.root, ui.materialNote, withColor.root);
    }
    for (const spec of LOOK.filter((s) => s.section === id)) {
      if (spec.group) sec.body.append(el('h3', { text: spec.group }));
      sec.body.append(lookRow(spec));
    }
    if (id === 'colour') {
      ui.colourNote = el('p', { class: 'note', hidden: true, text: 'This run has no morphogens, so there is no activator to colour by: switch Morphogens on under Parameters.' });
      sec.body.append(ui.colourNote);
    }
    if (id === 'material') {
      ui.driveNote = el('p', { class: 'note', hidden: true, text: 'A part of this material follows the morphogen activator, and this run has no morphogens: switch Morphogens on under Parameters.' });
      sec.body.append(ui.driveNote);
    }
    if (id === 'picture') {
      sec.body.append(buttons(button('Reset the look', () => {
        const keep = state.look.draw;
        state.look = structuredClone(LOOK_DEFAULTS);
        state.look.draw = keep;
        state.material = DEFAULT_MATERIAL;
        syncLook();
      }, { title: 'Colour, material, light and picture back to how the viewer starts' })));
    }
    return sec.root;
  });
}

function applyMaterial(name) {
  const m = findMaterial(name);
  if (!m) return;
  state.material = name;
  Object.assign(state.look, materialLook(m, state.withColor));
  syncLook();
}

function buildCamera() {
  const sec = section('Camera', { open: false });
  ui.follow = checkRow({
    label: 'follow the form as it grows', about: 'Keeps the whole form in view; moving the view sideways switches it off', value: camera.follow,
    onChange: (v) => { camera.follow = v; if (v) camera.zoom = 1; eng.dirty = true; },
  });
  ui.spin = sliderRow({
    label: 'slow turn', about: 'Degrees per second around the vertical; 0 is off', min: -60, max: 60, step: 1, value: camera.spin,
    onInput: (v) => { camera.spin = v; },
  });
  ui.fov = sliderRow({
    label: 'field of view', about: 'Degrees, top to bottom', min: 10, max: 90, step: 1, value: camera.fov,
    onInput: (v) => { camera.fov = v; eng.dirty = true; },
  });
  sec.body.append(ui.follow.root, ui.spin.root, ui.fov.root, buttons(button('Reset the view', () => {
    Object.assign(camera, CAMERA_DEFAULTS);
    eng.snap = true;
    eng.dirty = true;
    syncCamera();
  })), el('p', { class: 'about', text: 'Drag to turn, scroll or pinch to zoom, drag with the right button, with shift, or with two fingers to move. With the view focused: arrow keys, + and -, space to pause.' }));
  return sec.root;
}

function buildPanel() {
  $('panel').replaceChildren(buildSimulation(), buildParameters(), ...buildLook(), buildCamera());
  syncSwitches();
  syncLook();
}

function setPanel(open) {
  $('app').dataset.panel = open ? 'open' : 'closed';
  $('toggle-panel').setAttribute('aria-expanded', String(open));
  eng.dirty = true;
}

// ------------------------------------------------------------------------------------ GPU
function needsWebGpu(reason) {
  const lines = [
    'This app simulates and draws on the graphics card through WebGPU, which is not available here.',
    'What is needed: a current browser with WebGPU (Chrome, Edge, Safari or Firefox in a recent version), on a device whose graphics driver it supports.',
  ];
  if (!window.isSecureContext) lines.push('This page is not served securely: browsers offer WebGPU only over https, or on localhost.');
  lines.push(el('span', {}, 'The browser said: ', el('code', { text: reason })));
  fatal('WebGPU is needed', lines, { label: 'Try again', run: boot });
  $('status').textContent = 'WebGPU is not available';
}

async function boot() {
  clearFatal();
  $('status').textContent = 'Starting ...';
  let got;
  try {
    got = await requestDevice();
  } catch (e) {
    return needsWebGpu(e.message ?? String(e));
  }
  const device = got.device;
  // The GPU's name: what the adapter says about itself, as far as the browser lets it.
  const about = got.info ?? {};
  eng.gpuName = [about.description || [about.vendor, about.architecture].filter(Boolean).join(' '), about.description ? '' : about.device].filter(Boolean).join(' ').trim() ||
    (got.describe?.split('\n')[0] ?? '').replace(/^adapter:\s*/, '').replace(/\?/g, '').replace(/\s+/g, ' ').trim();
  device.lost.then((info) => {
    if (eng.device !== device) return;
    eng.device = null;
    eng.renderer = null;
    eng.sim = null;
    eng.pool = null;
    eng.known = null;
    eng.pending = 0;
    eng.readPromise = null;
    eng.epoch++;
    fatal('The graphics device was lost', [
      'The browser took the GPU away from this page (a driver reset, the GPU ran out of memory, or the computer slept). The run is gone; the settings are kept.',
      el('span', {}, 'The browser said: ', el('code', { text: info.message || info.reason || 'no reason given' })),
    ], { label: 'Start again', run: boot });
    $('status').textContent = 'The graphics device was lost';
  });
  device.addEventListener('uncapturederror', (e) => {
    // The first error is the one that says what went wrong; pause, or every frame adds more.
    if (eng.gpuErrors++ > 0) return;
    setRunning(false);
    notify('error', `WebGPU reported an error and the run was paused: ${String(e.error.message).trim().split(/\s*[\r\n]+\s*/).join(' ')}`, { id: 'gpu' });
  });
  try {
    const renderer = await Renderer.create(device, canvas);
    eng.device = device;
    eng.renderer = renderer;
  } catch (e) {
    device.destroy();
    fatal('The viewer could not start', [el('span', {}, 'Setting up the drawing failed: ', el('code', { text: e.message ?? String(e) }))], { label: 'Try again', run: boot });
    return;
  }
  eng.pending = 0;
  await restart();
  probeMechanisms();
}

// ------------------------------------------------------------------------------------ start
// What the page grows when it is opened without a link: this gallery form, which keeps growing
// past the step of its picture (opened from the gallery or by #g= it pauses there).
const DEFAULT_FORM = 5031;

async function main() {
  let fromLink = await applyLink(location.hash);
  if (!fromLink && (await applyLink('#g=' + DEFAULT_FORM)) && state.setup.gallery === DEFAULT_FORM) {
    state.pauseAt = 0;
    fromLink = true;
  }
  buildPanel();
  syncSetup();
  syncCamera();
  if (fromLink) showOrigin();
  setPanel(window.matchMedia('(min-width: 761px)').matches);

  for (const [id, act] of [['run', 'run'], ['step', 'step']]) $(id).dataset.act = act;
  $('run').addEventListener('click', () => setRunning(!state.running));
  $('step').addEventListener('click', () => { eng.stepOnce = 1; });
  $('restart').addEventListener('click', () => restart());
  $('tab-viewer').addEventListener('click', () => setView('viewer'));
  $('tab-gallery').addEventListener('click', () => setView('gallery'));
  $('copy-link').addEventListener('click', copyLink);
  $('save-picture').addEventListener('click', savePicture);
  $('toggle-panel').addEventListener('click', () => setPanel($('app').dataset.panel !== 'open'));
  updateRunButtons();

  attachCameraInput(canvas, camera, (what) => {
    eng.dirty = true;
    if (what === 'follow') ui.follow.set(camera.follow);
  });
  canvas.addEventListener('keydown', (e) => {
    if (e.key === ' ' && !eng.stopped && !eng.runError) {
      e.preventDefault();
      setRunning(!state.running);
    }
  });
  window.addEventListener('hashchange', async () => {
    if (await applyLink(location.hash)) {
      syncSetup();
      syncLook();
      syncCamera();
      showOrigin();
      eng.snap = true;
      setView('viewer');
      state.running = true;
      restart();
    }
  });
  window.addEventListener('resize', () => { eng.dirty = true; });

  requestAnimationFrame(frame);
  await boot();
}

// For looking at the page from the console.
window.cellularForms = { state, eng, camera, currentLink, draw };

main().catch((e) => {
  console.error(e);
  fatal('The app could not start', [el('code', { text: e.message ?? String(e) })]);
});
