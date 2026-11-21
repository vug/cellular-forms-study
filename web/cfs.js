// Reader for the simulation's checkpoint files (layout: src/core/io.hpp), and the invariant
// checker of docs/model.md section 11, in JavaScript, so the WebGPU port can be checked in the
// page against states written by the CPU reference.

const kMaxDeg = 12;
const kInvalid = 0xFFFFFFFF;

// Parses a checkpoint (.cfs, kind 1). Returns
//   { n, step, seed (BigInt), params {name: number}, pos, normal (Float32Array 3n), food,
//     birth, gen (Uint32Array n), deg (Uint8Array n), nbr (Uint32Array 12n) }
export function parseCheckpoint(buffer) {
  const view = new DataView(buffer);
  const magic = new TextDecoder().decode(new Uint8Array(buffer, 0, 7));
  if (magic !== 'CFSTATE') throw new Error('not a Cellular Forms state file');
  const version = view.getUint32(8, true);
  const kind = view.getUint32(16, true);
  const slots = view.getUint32(20, true);
  if (version !== 1) throw new Error(`unsupported file version ${version}`);
  if (kind !== 1) throw new Error('this file is a snapshot; a checkpoint is needed');
  if (slots !== kMaxDeg) throw new Error(`file has ${slots} neighbour slots, expected ${kMaxDeg}`);
  const n = Number(view.getBigUint64(24, true));
  const state = { n, step: view.getBigUint64(32, true), seed: view.getBigUint64(40, true), params: {} };
  let at = 48;
  while (at + 16 <= buffer.byteLength) {
    const tag = new TextDecoder().decode(new Uint8Array(buffer, at, 4));
    const bytes = Number(view.getBigUint64(at + 8, true));
    const body = at + 16;
    const copy = (Type, count) => new Type(buffer.slice(body, body + count * Type.BYTES_PER_ELEMENT));
    if (tag === 'PARM') {
      for (const line of new TextDecoder().decode(new Uint8Array(buffer, body, bytes)).split('\n')) {
        const eq = line.indexOf('=');
        if (eq > 0) state.params[line.slice(0, eq).trim()] = Number(line.slice(eq + 1));
      }
    } else if (tag === 'POS ') state.pos = copy(Float32Array, 3 * n);
    else if (tag === 'NRM ') state.normal = copy(Float32Array, 3 * n);
    else if (tag === 'FOOD') state.food = copy(Float32Array, n);
    else if (tag === 'BRTH') state.birth = copy(Uint32Array, n);
    else if (tag === 'GEN ') state.gen = copy(Uint32Array, n);
    else if (tag === 'DEG ') state.deg = copy(Uint8Array, n);
    else if (tag === 'NBR ') state.nbr = copy(Uint32Array, kMaxDeg * n);
    else if (tag === 'END ') break;
    at = body + bytes;
  }
  for (const key of ['pos', 'normal', 'food', 'birth', 'gen', 'deg', 'nbr'])
    if (!state[key]) throw new Error(`checkpoint has no ${key} data`);
  return state;
}

// docs/model.md section 11. Returns { ok, message }.
export function checkInvariants(s) {
  const n = s.n;
  const fail = (message) => ({ ok: false, message });
  let degreeSum = 0;
  for (let v = 0; v < n; v++) {
    const d = s.deg[v];
    if (d < 3 || d > kMaxDeg) return fail(`cell ${v}: degree ${d}`);
    degreeSum += d;
    const base = v * kMaxDeg;
    for (let k = 0; k < d; k++) {
      const a = s.nbr[base + k];
      if (a >= n) return fail(`cell ${v}: neighbour id ${a} out of range`);
      if (a === v) return fail(`cell ${v} lists itself`);
      for (let q = k + 1; q < d; q++) if (s.nbr[base + q] === a) return fail(`cell ${v}: duplicate neighbour ${a}`);
    }
  }
  const indexIn = (v, u) => {
    const base = v * kMaxDeg;
    for (let k = 0; k < s.deg[v]; k++) if (s.nbr[base + k] === u) return k;
    return -1;
  };
  for (let v = 0; v < n; v++) {
    const d = s.deg[v];
    const base = v * kMaxDeg;
    for (let k = 0; k < d; k++) {
      const a = s.nbr[base + k];
      const b = s.nbr[base + ((k + 1) % d)];
      if (indexIn(a, v) < 0) return fail(`symmetry: ${a} is in ring(${v}) but not the reverse`);
      const ib = indexIn(a, b);
      if (ib < 0) return fail(`ring(${v}): consecutive neighbours ${a} and ${b} are not linked`);
      if (s.nbr[a * kMaxDeg + ((ib + 1) % s.deg[a])] !== v) return fail(`orientation at cell ${v} (${a}, ${b})`);
    }
  }
  if (degreeSum !== 6 * n - 12) return fail(`Euler: degree sum ${degreeSum}, expected ${6 * n - 12}`);
  const seen = new Uint8Array(n);
  const queue = new Uint32Array(n);
  let head = 0, tail = 0, reached = 1;
  seen[0] = 1;
  queue[tail++] = 0;
  while (head < tail) {
    const x = queue[head++];
    const base = x * kMaxDeg;
    for (let k = 0; k < s.deg[x]; k++) {
      const y = s.nbr[base + k];
      if (!seen[y]) {
        seen[y] = 1;
        queue[tail++] = y;
        reached++;
      }
    }
  }
  if (reached !== n) return fail(`not connected: reached ${reached} of ${n} cells`);
  for (let i = 0; i < 3 * n; i++) if (!Number.isFinite(s.pos[i])) return fail(`position of cell ${Math.floor(i / 3)} is not finite`);
  for (let v = 0; v < n; v++) if (!Number.isFinite(s.food[v])) return fail(`food of cell ${v} is not finite`);
  return { ok: true, message: '' };
}

// Mean and standard deviation of the link lengths (each link counted once).
export function linkStats(s) {
  let sum = 0, sum2 = 0, count = 0;
  for (let v = 0; v < s.n; v++) {
    for (let k = 0; k < s.deg[v]; k++) {
      const u = s.nbr[v * kMaxDeg + k];
      if (u <= v) continue;
      const dx = s.pos[3 * u] - s.pos[3 * v], dy = s.pos[3 * u + 1] - s.pos[3 * v + 1], dz = s.pos[3 * u + 2] - s.pos[3 * v + 2];
      const len = Math.sqrt(dx * dx + dy * dy + dz * dz);
      sum += len;
      sum2 += len * len;
      count++;
    }
  }
  const mean = sum / count;
  return { mean, std: Math.sqrt(Math.max(sum2 / count - mean * mean, 0)), links: count };
}

// FNV-1a 64 over the state arrays (a fingerprint for rerun comparisons inside the page).
export function fingerprint(s) {
  let h = 0xCBF29CE484222325n;
  const mix = (bytes) => {
    for (let i = 0; i < bytes.length; i++) {
      h ^= BigInt(bytes[i]);
      h = (h * 0x100000001B3n) & 0xFFFFFFFFFFFFFFFFn;
    }
  };
  // BigInt per byte is slow; fold the arrays through a 32-bit mix first.
  const fold = (arr) => {
    const u = new Uint32Array(arr.buffer, arr.byteOffset, Math.floor(arr.byteLength / 4));
    let a = 0x811C9DC5 | 0, b = 0x9E3779B9 | 0;
    for (let i = 0; i < u.length; i++) {
      a = Math.imul(a ^ u[i], 0x01000193);
      b = (Math.imul(b, 31) + u[i]) | 0;
    }
    return new Uint8Array(new Uint32Array([a, b, u.length]).buffer);
  };
  mix(fold(s.pos));
  mix(fold(s.normal));
  mix(fold(s.food));
  mix(fold(s.birth));
  mix(fold(s.gen));
  mix(fold(s.nbr));
  mix(new Uint8Array(new Uint32Array([s.n]).buffer));
  return h.toString(16).padStart(16, '0');
}

export { kMaxDeg, kInvalid };
