// A whole setup in the URL hash, compact: only what differs from the defaults is written.
//
//   g=<id>            a gallery form (its start shape, first food, seed and parameters)
//   i=<shape>         start shape           f=zero   no first food        s=<seed>
//   n=<capacity>      cells the buffers hold
//   x=1               exact arithmetic (x=0: a gallery form without it; a gallery form has it otherwise)
//   p=name:value,...  parameters that differ from the defaults of the model (src/core/params.hpp,
//                     DEFAULT_PARAMS of web/sim.js): a run gets exactly what the link names, and the
//                     model's own default for everything else, whatever the panel starts with
//   m=<material>      the material of the library the look started from
//   l=key:value,...   look settings that differ from that material's (short keys: look.js)
//   c=yaw,pitch,fov,follow,zoom,spin[,distance,x,y,z]   the camera
//
// Example: #g=2718&m=jade&l=d:membrane&c=0.6,0.35,34,1,1,0
import { DEFAULT_PARAMS } from '../sim.js';
import { GROUPS } from './params.js';
import { LOOK, DEFAULT_MATERIAL, baselineLook, toHex, fromHex, sameValue } from './look.js';
import { findMaterial } from './materials.js';
import { CAMERA_DEFAULTS } from './camera.js';

export const SETUP_DEFAULTS = { shape: 'icosphere:2', food: 'random', seed: 1, capacity: 500000, exact: false };

// The model's own default of every parameter (the capacity is not one of them here).
export const MODEL_DEFAULTS = Object.fromEntries(Object.entries(DEFAULT_PARAMS).filter(([name]) => name !== 'maxCells'));

// What a run starts with when nothing names its parameters: the model's defaults, and the
// panel's starting value (params.js) for the parameters that are always in effect. The
// parameters of an optional mechanism keep the model's default while it is off (they do
// nothing then) and take the panel's starting value when it is switched on (see main.js), so
// what is not deliberately set is always the model's.
export const START_PARAMS = (() => {
  const p = { ...MODEL_DEFAULTS };
  for (const g of GROUPS) {
    if (g.switch) p[g.switch.name] = g.switch.off;
    else for (const q of g.params) p[q.name] = q.value;
  }
  return p;
})();

const num = (x, digits) => String(+Number(x).toPrecision(digits));
const esc = (s) => encodeURIComponent(s).replace(/%3A/gi, ':');

// The parameters that differ from the model's defaults, { name: value }.
export function changedParams(params) {
  const out = {};
  for (const [name, value] of Object.entries(params)) {
    if (name === 'maxCells') continue;
    if (!(name in MODEL_DEFAULTS) || Math.fround(value) !== Math.fround(MODEL_DEFAULTS[name])) out[name] = value;
  }
  return out;
}

// setup: { shape, food, seed, capacity, params, gallery (id, while the setup is still that form) }
export function encodeLink({ setup, look, material, camera }) {
  const parts = [];
  if (setup.gallery != null) parts.push('g=' + setup.gallery);
  else {
    if (setup.shape !== SETUP_DEFAULTS.shape) parts.push('i=' + esc(setup.shape));
    if (setup.food !== SETUP_DEFAULTS.food) parts.push('f=' + esc(setup.food));
    if (setup.seed !== SETUP_DEFAULTS.seed) parts.push('s=' + setup.seed);
    const changed = Object.entries(changedParams(setup.params));
    // Nine digits give back the same single-precision number, so the run is the same run.
    if (changed.length) parts.push('p=' + changed.map(([k, v]) => `${k}:${num(v, 9)}`).join(','));
  }
  if (setup.capacity !== SETUP_DEFAULTS.capacity) parts.push('n=' + setup.capacity);
  if (!!setup.exact !== (setup.gallery != null)) parts.push('x=' + (setup.exact ? 1 : 0));

  const mat = findMaterial(material) ? material : DEFAULT_MATERIAL;
  if (mat !== DEFAULT_MATERIAL) parts.push('m=' + esc(mat));
  const base = baselineLook(mat);
  const diffs = [];
  for (const s of LOOK) {
    const v = look[s.id];
    if (sameValue(v, base[s.id])) continue;
    let text;
    if (Array.isArray(v)) text = toHex(v).slice(1);
    else if (typeof v === 'boolean') text = v ? '1' : '0';
    else if (typeof v === 'number') text = num(v, 5);
    else text = esc(String(v));
    diffs.push(`${s.k}:${text}`);
  }
  if (diffs.length) parts.push('l=' + diffs.join(','));

  const c = camera;
  const cam = [num(c.yaw, 4), num(c.pitch, 4), num(c.fov, 4), c.follow ? 1 : 0, num(c.zoom, 4), num(c.spin, 3)];
  if (!c.follow) cam.push(num(c.distance, 5), ...c.target.map((x) => num(x, 5)));
  const d = CAMERA_DEFAULTS;
  const isDefault = c.follow && cam.join(',') === [num(d.yaw, 4), num(d.pitch, 4), num(d.fov, 4), 1, num(d.zoom, 4), num(d.spin, 3)].join(',');
  if (!isDefault) parts.push('c=' + cam.join(','));
  return parts.join('&');
}

// Reads a hash. Returns { gallery, setup (only what the link names), material, look (the whole
// look), camera (only what the link names), problems: [text] }, or null for an empty hash.
export function decodeLink(hash) {
  const text = hash.replace(/^#/, '');
  if (!text) return null;
  const fields = new Map();
  for (const part of text.split('&')) {
    const eq = part.indexOf('=');
    if (eq > 0) fields.set(part.slice(0, eq), part.slice(eq + 1));
  }
  const problems = [];
  const dec = (s) => {
    try { return decodeURIComponent(s); } catch { return s; }
  };
  const out = { gallery: null, setup: {}, material: DEFAULT_MATERIAL, look: null, camera: null, problems };

  if (fields.has('g')) {
    const id = Number(fields.get('g'));
    if (Number.isFinite(id)) out.gallery = id;
    else problems.push(`'${fields.get('g')}' is not a gallery form`);
  }
  if (fields.has('i')) out.setup.shape = dec(fields.get('i'));
  if (fields.has('f')) out.setup.food = dec(fields.get('f')) === 'zero' ? 'zero' : 'random';
  if (fields.has('s')) {
    const seed = Number(fields.get('s'));
    if (Number.isSafeInteger(seed) && seed >= 0) out.setup.seed = seed;
    else problems.push(`'${fields.get('s')}' is not a seed`);
  }
  if (fields.has('n')) {
    const n = Number(fields.get('n'));
    if (Number.isSafeInteger(n) && n > 0) out.setup.capacity = n;
  }
  if (fields.has('x')) out.setup.exact = fields.get('x') === '1';
  if (fields.has('p')) {
    out.setup.params = {};
    for (const item of fields.get('p').split(',')) {
      const [name, value] = item.split(':');
      const v = Number(value);
      if (name in MODEL_DEFAULTS && value !== undefined && value !== '' && Number.isFinite(v)) out.setup.params[name] = v;
      else if (name) problems.push(`parameter '${dec(name)}' is not known`);
    }
  }

  if (fields.has('m')) {
    const name = dec(fields.get('m'));
    if (findMaterial(name)) out.material = name;
    else problems.push(`material '${name}' is not in the library`);
  }
  out.look = baselineLook(out.material);
  if (fields.has('l')) {
    const byKey = new Map(LOOK.map((s) => [s.k, s]));
    for (const item of fields.get('l').split(',')) {
      const at = item.indexOf(':');
      const s = byKey.get(item.slice(0, at));
      const value = dec(item.slice(at + 1));
      if (at < 1 || !s) continue;
      if (Array.isArray(s.def)) {
        const rgb = fromHex(value);
        if (rgb) out.look[s.id] = rgb;
      } else if (typeof s.def === 'boolean') out.look[s.id] = value === '1';
      else if (typeof s.def === 'number') {
        const v = Number(value);
        if (Number.isFinite(v)) out.look[s.id] = v;
      } else if (!s.options || s.options.some(([id]) => id === value)) out.look[s.id] = value;
    }
  }

  if (fields.has('c')) {
    const v = fields.get('c').split(',').map(Number);
    if (v.length >= 6 && v.every(Number.isFinite)) {
      out.camera = { yaw: v[0], pitch: Math.max(-1.55, Math.min(1.55, v[1])), fov: Math.max(10, Math.min(90, v[2])), follow: v[3] !== 0, zoom: v[4] || 1, spin: v[5] };
      if (v.length >= 10) {
        out.camera.distance = Math.max(1e-3, v[6]);
        out.camera.target = [v[7], v[8], v[9]];
      } else out.camera.follow = true;
    }
  }
  return out;
}
