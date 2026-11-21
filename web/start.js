// Start states in the page: a start shape (a state file without food, web/app/starts) gets
// a seed, the parameters of a run, its first food and, with morphogens switched on, its
// starting concentrations, as cf_run --new does it, so that the same start, seed and
// parameters give the same run here as on the PC.
//
// The random numbers are the counter-based hashes of src/core/hash.hpp, on BigInt; the float
// arithmetic is the CPU's single precision (every operation rounded with Math.fround).

const MASK = (1n << 64n) - 1n;
const f = Math.fround;

export const STREAM = { Food: 1n, Priority: 2n, Pair: 3n, InitFood: 4n, Morph: 5n };

export function mix64(x) {
  x ^= x >> 30n;
  x = (x * 0xBF58476D1CE4E5B9n) & MASK;
  x ^= x >> 27n;
  x = (x * 0x94D049BB133111EBn) & MASK;
  x ^= x >> 31n;
  return x;
}

export function hashU64(seed, step, id, stream) {
  let h = mix64((BigInt(seed) + 0x9E3779B97F4A7C15n * (stream + 1n)) & MASK);
  h = mix64(h ^ BigInt(step));
  h = mix64(h ^ BigInt(id));
  return h;
}

// Uniform float in [0, 1) with 24 random bits (hash_u01).
export function hashU01(seed, step, id, stream) {
  return Number(hashU64(seed, step, id, stream) >> 40n) * (1.0 / 16777216.0);
}

// ---- smooth value noise of src/core/noise.hpp
function lattice01(x, y, z, seed) {
  const k = (BigInt.asUintN(32, BigInt(x)) * 0x9E3779B97F4A7C15n + BigInt.asUintN(32, BigInt(y)) * 0xC2B2AE3D27D4EB4Fn +
    BigInt.asUintN(32, BigInt(z)) * 0x165667B19E3779F9n) & MASK;
  return Number(mix64(seed ^ k) >> 40n) * (1.0 / 16777216.0);
}
const lerp1 = (a, b, t) => f(a + f(f(b - a) * t));

// value_noise3: trilinear interpolation of hashed lattice values with smoothstep weights, in [0, 1).
export function valueNoise3(px, py, pz, seed) {
  const fx = Math.floor(px), fy = Math.floor(py), fz = Math.floor(pz);
  const ix = fx | 0, iy = fy | 0, iz = fz | 0;
  const tx = f(px - fx), ty = f(py - fy), tz = f(pz - fz);
  const smooth = (t) => f(f(t * t) * f(3 - f(2 * t)));
  const sx = smooth(tx), sy = smooth(ty), sz = smooth(tz);
  const c = (dx, dy, dz) => lattice01((ix + dx) | 0, (iy + dy) | 0, (iz + dz) | 0, seed);
  const x00 = lerp1(c(0, 0, 0), c(1, 0, 0), sx), x10 = lerp1(c(0, 1, 0), c(1, 1, 0), sx);
  const x01 = lerp1(c(0, 0, 1), c(1, 0, 1), sx), x11 = lerp1(c(0, 1, 1), c(1, 1, 1), sx);
  return lerp1(lerp1(x00, x10, sy), lerp1(x01, x11, sy), sz);
}

// Gives every cell of `state` its starting morphogen concentrations (state.morphA, the
// substrate, and state.morphB, the activator), from the seed, the state's step and the cells'
// positions: init_morphogens / morph_initial of src/core. The activator starts in the patches
// where a smooth random field in space (features of size morphSeedScale) is above
// morphSeedLevel; inside them every cell gets a slightly different amount.
export function initMorphogens(state) {
  const n = state.n;
  const seed = BigInt(state.seed), step = BigInt(state.step);
  const seedLevel = f(state.params.morphSeedLevel ?? 0.75);
  const inv = f(1 / f(state.params.morphSeedScale ?? 3));
  const noiseSeed = mix64(seed ^ ((0x4D4F5250n + step) & MASK));
  state.morphA = new Float32Array(n);
  state.morphB = new Float32Array(n);
  for (let v = 0; v < n; v++) {
    const field = valueNoise3(f(state.pos[3 * v] * inv), f(state.pos[3 * v + 1] * inv), f(state.pos[3 * v + 2] * inv), noiseSeed);
    if (field > seedLevel) {
      state.morphA[v] = 0.5;
      state.morphB[v] = f(0.25 + f(f(0.1) * hashU01(seed, step, v, STREAM.Morph)));
    } else {
      state.morphA[v] = 1;
      state.morphB[v] = 0;
    }
  }
  return state;
}

// A state to upload: `mesh` is a parsed start shape (parseCheckpoint of a file of
// web/app/starts), `params` the parameters that differ from the defaults, `food` 'random'
// (uniform in [0, threshold), randomise_initial_food) or 'zero'. With morphogens switched on
// in the parameters, the state gets its starting concentrations (as the CPU reference gives
// them to a run that starts with morphogens on).
export function makeStart(mesh, { seed = 1, food = 'random', params = {}, defaults = {} } = {}) {
  const n = mesh.n;
  const state = {
    n, step: 0n, seed: BigInt(seed), params: { ...mesh.params, ...defaults, ...params },
    pos: mesh.pos.slice(), normal: mesh.normal.slice(), food: new Float32Array(n), birth: new Uint32Array(n), gen: new Uint32Array(n),
    deg: mesh.deg.slice(), nbr: mesh.nbr.slice(),
  };
  if (food === 'random') {
    const threshold = Math.fround(state.params.foodThreshold ?? 1);
    for (let v = 0; v < n; v++) state.food[v] = Math.fround(threshold * Math.fround(hashU01(state.seed, 0, v, STREAM.InitFood)));
  }
  if (f(Number(state.params.morphogens ?? 0)) !== 0) initMorphogens(state);
  return state;
}
