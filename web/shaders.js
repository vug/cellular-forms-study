// WGSL compute shaders of the WebGPU port (M9). Same pipeline as shaders/sim (the Vulkan
// compute backend), with what WebGPU makes different:
//   - no 64-bit integers: the hashes of core/hash.hpp run on (lo, hi) pairs of u32;
//   - no push constants and a limit of 8 storage buffers per shader: everything global lives in
//     one `Globals` buffer, and per-cell data is packed (position + food, 16 u32 of topology);
//   - reading anything back is asynchronous and slow, so the cell count, the step counter and
//     the dispatch sizes stay on the GPU (indirect dispatch) and many steps go out per submit.

export const ROW = 32768 * 256;  // invocations per dispatch row (x), so y extends past 65535 groups

const globals = /* wgsl */ `
struct Globals {
  // --- parameters (written by the page)
  l0: f32, spring: f32, planar: f32, bulge: f32,
  repulsion: f32, roi: f32, maxMove: f32, foodMin: f32,
  foodSpan: f32, threshold: f32, daughterOffset: f32, normalEps2: f32,
  unitEps2: f32, seedLo: u32, seedHi: u32, limit: u32,
  // --- simulation state, kept on the GPU
  n: u32, stepLo: u32, stepHi: u32, tableMask: u32,
  originX: f32, originY: f32, originZ: f32, invCell: f32,
  nx: u32, ny: u32, nz: u32, failed: u32,
  // --- per-step counters
  fallbacks: atomic<u32>, clamped: atomic<u32>, ready: atomic<u32>, postponed: atomic<u32>,
  firstBadMove: atomic<u32>, firstBadPos: atomic<u32>, winners: u32, gridTooLarge: u32,
  minX: atomic<i32>, minY: atomic<i32>, minZ: atomic<i32>, maxX: atomic<i32>,
  maxY: atomic<i32>, maxZ: atomic<i32>, failStepLo: u32, failCode: u32,
  // --- running totals
  divisionsLo: u32, divisionsHi: u32, clampedLo: u32, clampedHi: u32,
  // --- prefix-sum level table: [which * 4 + level] = (entries, offset in levels[], 0, 0)
  scan: array<vec4<u32>, 8>,
}
const kMaxDeg = 12u;
const kInvalid = 0xFFFFFFFFu;
const kNoPair = 0xFFu;
const ROW = ${ROW}u;
// topology record: 16 u32 per cell
const T_DEG = 0u; const T_BIRTH = 1u; const T_GEN = 2u; const T_PAIR = 3u; const T_NBR = 4u;
`;

const math = /* wgsl */ `
fn finite1(x: f32) -> bool { return (bitcast<u32>(x) & 0x7F800000u) != 0x7F800000u; }
fn finite3(v: vec3f) -> bool { return finite1(v.x) && finite1(v.y) && finite1(v.z); }
fn dot3(a: vec3f, b: vec3f) -> f32 { return a.x * b.x + a.y * b.y + a.z * b.z; }
fn cross3(a: vec3f, b: vec3f) -> vec3f {
  return vec3f(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
fn length3(a: vec3f) -> f32 { return sqrt(dot3(a, a)); }
`;

const mathG = /* wgsl */ `
// unit(v), except that a (near-)zero vector gives zero instead of NaN.
fn unitOrZero(v: vec3f) -> vec3f {
  let l2 = dot3(v, v);
  if (l2 < G.unitEps2) { return vec3f(0.0); }
  let inv = 1.0 / sqrt(l2);
  return vec3f(v.x * inv, v.y * inv, v.z * inv);
}
fn gridCoord(p: f32, origin: f32, inv: f32, dim: u32) -> u32 {
  let fl = floor((p - origin) * inv);
  if (!(fl > 0.0)) { return 0u; }
  if (fl >= f32(dim)) { return dim - 1u; }
  return min(u32(fl), dim - 1u);
}
fn cellOf(p: vec3f) -> vec3<u32> {
  return vec3<u32>(gridCoord(p.x, G.originX, G.invCell, G.nx), gridCoord(p.y, G.originY, G.invCell, G.ny),
                   gridCoord(p.z, G.originZ, G.invCell, G.nz));
}
// Bucket of a grid cell: any fixed mixing function will do, it only spreads cells over the table.
fn bucketOf(c: vec3<u32>) -> u32 {
  var h = (c.x * 73856093u) ^ (c.y * 19349663u) ^ (c.z * 83492791u);
  h ^= h >> 16u; h *= 0x85EBCA6Bu; h ^= h >> 13u; h *= 0xC2B2AE35u; h ^= h >> 16u;
  return h & G.tableMask;
}
`;

// Per-stream constant of hash_u64 in core/hash.hpp, computed here so no digit is typed by hand.
const streamKey = (stream) => {
  const v = (0x9E3779B97F4A7C15n * BigInt(stream + 1)) & 0xFFFFFFFFFFFFFFFFn;
  return `U64(${Number(v & 0xFFFFFFFFn)}u, ${Number(v >> 32n)}u)`;
};

// 64-bit arithmetic on (lo, hi) pairs, and the hashes of src/core/hash.hpp.
const hash = /* wgsl */ `
struct U64 { lo: u32, hi: u32 }
fn mul32(a: u32, b: u32) -> U64 {
  let a0 = a & 0xFFFFu; let a1 = a >> 16u; let b0 = b & 0xFFFFu; let b1 = b >> 16u;
  let p00 = a0 * b0; let p01 = a0 * b1; let p10 = a1 * b0; let p11 = a1 * b1;
  let mid = (p00 >> 16u) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
  return U64((p00 & 0xFFFFu) | (mid << 16u), p11 + (p01 >> 16u) + (p10 >> 16u) + (mid >> 16u));
}
fn mul64(a: U64, b: U64) -> U64 {
  let p = mul32(a.lo, b.lo);
  return U64(p.lo, p.hi + a.lo * b.hi + a.hi * b.lo);
}
fn add64(a: U64, b: U64) -> U64 {
  let lo = a.lo + b.lo;
  return U64(lo, a.hi + b.hi + select(0u, 1u, lo < a.lo));
}
fn xor64(a: U64, b: U64) -> U64 { return U64(a.lo ^ b.lo, a.hi ^ b.hi); }
// x ^ (x >> s), for 0 < s < 32
fn xorShr(x: U64, s: u32) -> U64 { return U64(x.lo ^ ((x.lo >> s) | (x.hi << (32u - s))), x.hi ^ (x.hi >> s)); }
fn mix64(v: U64) -> U64 {
  var x = xorShr(v, 30u);
  x = mul64(x, U64(0x1CE4E5B9u, 0xBF58476Du));
  x = xorShr(x, 27u);
  x = mul64(x, U64(0x133111EBu, 0x94D049BBu));
  return xorShr(x, 31u);
}
// 0x9E3779B97F4A7C15 * (stream + 1) for streams Food = 1, Priority = 2, Pair = 3
const kStreamFood = ${streamKey(1)};
const kStreamPriority = ${streamKey(2)};
const kStreamPair = ${streamKey(3)};
fn hashU64(streamKey: U64, id: u32) -> U64 {
  var h = mix64(add64(U64(G.seedLo, G.seedHi), streamKey));
  h = mix64(xor64(h, U64(G.stepLo, G.stepHi)));
  return mix64(xor64(h, U64(id, 0u)));
}
// Uniform float in [0, 1) from the top 24 bits.
fn hashU01(streamKey: U64, id: u32) -> f32 { return f32(hashU64(streamKey, id).hi >> 8u) * (1.0 / 16777216.0); }
`;

const index = /* wgsl */ `
// Invocation index of a dispatch laid out by prepare(): rows of 32768 workgroups.
fn cellIndex(gid: vec3<u32>) -> u32 { return gid.x + gid.y * ROW; }          // workgroup_size(256) passes
fn blockIndex(gid: vec3<u32>) -> u32 { return gid.x + gid.y * (ROW / 4u); }  // workgroup_size(64) passes
`;

// ---------------------------------------------------------------------------------------------

// One invocation: resets the counters and sizes this step's dispatches from the cell count.
export const prepare = (tableCap) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> ind: array<u32>;

fn groups(items: u32, per: u32) -> vec3<u32> {
  let g = (items + per - 1u) / per;
  if (g <= 32768u) { return vec3<u32>(g, 1u, 1u); }
  return vec3<u32>(32768u, (g + 32767u) / 32768u, 1u);
}
fn put(slot: u32, g: vec3<u32>) { ind[slot * 3u] = g.x; ind[slot * 3u + 1u] = g.y; ind[slot * 3u + 2u] = g.z; }
fn levels(which: u32, count: u32, firstSlot: u32) {
  var size = count;
  var offset = 0u;
  G.scan[which * 4u] = vec4<u32>(size, 0u, 0u, 0u);
  for (var j = 1u; j < 4u; j++) {
    size = (size + 255u) / 256u;
    G.scan[which * 4u + j] = vec4<u32>(size, offset, 0u, 0u);
    put(firstSlot + j - 1u, groups(size, 64u));   // one invocation per block of the level below
    offset += size;
  }
}

@compute @workgroup_size(1)
fn main() {
  atomicStore(&G.fallbacks, 0u); atomicStore(&G.clamped, 0u); atomicStore(&G.ready, 0u); atomicStore(&G.postponed, 0u);
  atomicStore(&G.firstBadMove, kInvalid); atomicStore(&G.firstBadPos, kInvalid);
  G.winners = 0u; G.gridTooLarge = 0u;
  atomicStore(&G.minX, 2147483647); atomicStore(&G.minY, 2147483647); atomicStore(&G.minZ, 2147483647);
  atomicStore(&G.maxX, -2147483647 - 1); atomicStore(&G.maxY, -2147483647 - 1); atomicStore(&G.maxZ, -2147483647 - 1);

  let n = select(G.n, 0u, G.failed != 0u);   // after a failure nothing runs any more
  var t = 1024u;
  while (t < n / 2u && t < ${tableCap}u) { t = t << 1u; }
  G.tableMask = t - 1u;
  put(0u, groups(n, 256u));            // per-cell passes
  put(1u, groups(select(t + 1u, 0u, n == 0u), 64u));  // per-bucket passes
  levels(0u, select(t + 1u, 0u, n == 0u), 2u);        // slots 2, 3, 4: grid scan levels 1..3
  put(5u, vec3<u32>(select(1u, 0u, n == 0u), 1u, 1u));  // single-invocation passes
  levels(1u, n, 6u);                    // slots 6, 7, 8: winner scan levels 1..3
}
`;

// Zeroes the grid tables for the buckets in use.
export const clear = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> scanData: array<u32>;
@group(0) @binding(2) var<storage, read_write> cellFill: array<u32>;
${index}
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let b = blockIndex(gid);
  if (b > G.tableMask + 1u) { return; }
  scanData[b] = 0u;
  if (b <= G.tableMask) { cellFill[b] = 0u; }
}
`;

// Steps 1 and 2: food, normals, bounds and the finite check.
export const pre = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(3) var<storage, read> nrmCur: array<vec4f>;
@group(0) @binding(4) var<storage, read_write> nrmNew: array<vec4f>;
@group(0) @binding(5) var<storage, read> topo: array<u32>;
${math}
${hash}
${index}
fn orderedInt(f: f32) -> i32 { let i = bitcast<i32>(f); return i ^ ((i >> 31u) & 0x7FFFFFFF); }

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let cur = geomCur[v];
  let P = cur.xyz;

  // food: uniform random increment (hash of seed, step, cell id)
  let food = cur.w + (G.foodMin + G.foodSpan * hashU01(kStreamFood, v));

  if (!finite3(P)) {
    atomicMin(&G.firstBadPos, v);
  } else {
    atomicMin(&G.minX, orderedInt(P.x)); atomicMin(&G.minY, orderedInt(P.y)); atomicMin(&G.minZ, orderedInt(P.z));
    atomicMax(&G.maxX, orderedInt(P.x)); atomicMax(&G.maxY, orderedInt(P.y)); atomicMax(&G.maxZ, orderedInt(P.z));
  }

  // normal from the neighbour ring, with the fallback to the previous normal
  let base = v * 16u + T_NBR;
  let d = topo[v * 16u + T_DEG];
  var sum = vec3f(0.0);
  let first = geomCur[topo[base]].xyz - P;
  var a = first;
  for (var k = 1u; k < d; k++) {
    let b = geomCur[topo[base + k]].xyz - P;
    sum += cross3(a, b);
    a = b;
  }
  sum += cross3(a, first);
  let l2 = dot3(sum, sum);
  var nrm = nrmCur[v].xyz;
  if (l2 > G.normalEps2) {
    nrm = sum * (1.0 / sqrt(l2));
  } else {
    atomicAdd(&G.fallbacks, 1u);
  }
  nrmNew[v] = vec4f(nrm, 0.0);
  // The position part is filled in by the move pass; the food is final here.
  geomNew[v] = vec4f(P, food);
}
`;

// One invocation: grid origin and dimensions from the bounds.
export const gridSetup = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
fn fromOrdered(i: i32) -> f32 { return bitcast<f32>(i ^ ((i >> 31u) & 0x7FFFFFFF)); }

@compute @workgroup_size(1)
fn main() {
  if (G.failed != 0u || G.n == 0u) { return; }
  let lo = vec3f(fromOrdered(atomicLoad(&G.minX)), fromOrdered(atomicLoad(&G.minY)), fromOrdered(atomicLoad(&G.minZ)));
  let hi = vec3f(fromOrdered(atomicLoad(&G.maxX)), fromOrdered(atomicLoad(&G.maxY)), fromOrdered(atomicLoad(&G.maxZ)));
  let inv = 1.0 / G.roi;
  G.originX = lo.x; G.originY = lo.y; G.originZ = lo.z; G.invCell = inv;
  // (The CPU computes these in double precision; f32 can differ by one cell at the far edge,
  // which only shifts where the last grid cell ends.)
  let dims = floor((hi - lo) * inv) + vec3f(1.0);
  let bad = atomicLoad(&G.firstBadPos) != kInvalid;
  if (bad || !(dims.x <= 2097152.0 && dims.y <= 2097152.0 && dims.z <= 2097152.0) || !(dims.x >= 1.0 && dims.y >= 1.0 && dims.z >= 1.0)) {
    if (!bad) { G.gridTooLarge = 1u; }
    G.nx = 1u; G.ny = 1u; G.nz = 1u;
  } else {
    G.nx = u32(dims.x); G.ny = u32(dims.y); G.nz = u32(dims.z);
  }
}
`;

export const gridCount = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> scanData: array<atomic<u32>>;
${math}
${mathG}
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  atomicAdd(&scanData[bucketOf(cellOf(geomCur[v].xyz))], 1u);
}
`;

// Exclusive prefix sum of scanData[0 .. count), in place: block sums up three levels, a
// sequential scan of the small top level, then back down. `which` 0 = grid buckets, 1 = winner
// flags. Integer adds only. op: 'up' (level j -> j + 1), 'top' (level 3), 'down' (into level j).
export const scan = (which, op, j) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> scanData: array<u32>;
@group(0) @binding(2) var<storage, read_write> levels: array<u32>;
${index}
fn rd(level: u32, i: u32) -> u32 {
  if (level == 0u) { return scanData[i]; }
  return levels[G.scan[${which}u * 4u + level].y + i];
}
fn wr(level: u32, i: u32, x: u32) {
  if (level == 0u) { scanData[i] = x; } else { levels[G.scan[${which}u * 4u + level].y + i] = x; }
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let block = blockIndex(gid);
  ${op === 'top' ? `
  if (block != 0u) { return; }
  let count = G.scan[${which}u * 4u + 3u].x;
  var acc = 0u;
  for (var i = 0u; i < count; i++) {
    let t = rd(3u, i);
    wr(3u, i, acc);
    acc += t;
  }
  ${which === 1 ? 'G.winners = acc;' : ''}
  ` : `
  let count = G.scan[${which}u * 4u + ${j}u].x;
  let begin = block * 256u;
  if (begin >= count) { return; }
  let end = min(begin + 256u, count);
  ${op === 'up' ? `
  var sum = 0u;
  for (var i = begin; i < end; i++) { sum += rd(${j}u, i); }
  wr(${j + 1}u, block, sum);
  ` : `
  var acc = rd(${j + 1}u, block);
  for (var i = begin; i < end; i++) {
    let t = rd(${j}u, i);
    wr(${j}u, i, acc);
    acc += t;
  }
  `}
  `}
}
`;

export const gridFill = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read> scanData: array<u32>;
@group(0) @binding(3) var<storage, read_write> cellFill: array<atomic<u32>>;
@group(0) @binding(4) var<storage, read_write> sortedId: array<u32>;
${math}
${mathG}
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let b = bucketOf(cellOf(geomCur[v].xyz));
  sortedId[scanData[b] + atomicAdd(&cellFill[b], 1u)] = v;
}
`;

// One invocation per bucket: sort its cells by id (which makes the order independent of thread
// timing) and gather their positions, with the id in w.
export const gridSort = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read> scanData: array<u32>;
@group(0) @binding(3) var<storage, read_write> sortedId: array<u32>;
@group(0) @binding(4) var<storage, read_write> sortedPos: array<vec4f>;
${index}
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let b = blockIndex(gid);
  if (b > G.tableMask || G.failed != 0u || G.n == 0u) { return; }
  let begin = scanData[b];
  let end = scanData[b + 1u];
  for (var i = begin + 1u; i < end; i++) {
    let x = sortedId[i];
    var j = i;
    while (j > begin && sortedId[j - 1u] > x) {
      sortedId[j] = sortedId[j - 1u];
      j--;
    }
    sortedId[j] = x;
  }
  for (var i = begin; i < end; i++) {
    let id = sortedId[i];
    sortedPos[i] = vec4f(geomCur[id].xyz, bitcast<f32>(id));
  }
}
`;

// Steps 4 and 5: move (reads the old positions, writes the new buffer) and clamp. Invocations
// walk the cells in grid order so neighbours share the buckets they search.
export const move = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(3) var<storage, read> nrmNew: array<vec4f>;
@group(0) @binding(4) var<storage, read> topo: array<u32>;
@group(0) @binding(5) var<storage, read> scanData: array<u32>;
@group(0) @binding(6) var<storage, read> sortedPos: array<vec4f>;
${math}
${mathG}
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let q = cellIndex(gid);
  if (q >= G.n || G.failed != 0u) { return; }
  let v = bitcast<u32>(sortedPos[q].w);
  let P = geomCur[v].xyz;
  let d = topo[v * 16u + T_DEG];
  let base = v * 16u + T_NBR;
  let N = nrmNew[v].xyz;

  // link terms
  var springSum = vec3f(0.0);
  var planarSum = vec3f(0.0);
  var bulgeSum = 0.0;
  let l0sq = G.l0 * G.l0;
  for (var k = 0u; k < d; k++) {
    let L = geomCur[topo[base + k]].xyz;
    let toP = P - L;
    springSum += L + G.l0 * unitOrZero(toP);
    planarSum += L;
    let dotN = -dot3(toP, N);
    let arg = max(l0sq - dot3(toP, toP) + dotN * dotN, 0.0);   // over-stretched link: clamp
    bulgeSum += sqrt(arg) + dotN;
  }
  let inv = 1.0 / f32(d);
  let spring = G.spring * (springSum * inv - P);
  let planar = G.planar * (planarSum * inv - P);
  let bulge = (G.bulge * (bulgeSum * inv)) * N;

  // repulsion from non-linked cells within roi, in the reference order (z, y, x; ascending id)
  let roi2 = G.roi * G.roi;
  let c = cellOf(P);
  let x0 = select(0u, c.x - 1u, c.x > 0u); let x1 = min(c.x + 1u, G.nx - 1u);
  let y0 = select(0u, c.y - 1u, c.y > 0u); let y1 = min(c.y + 1u, G.ny - 1u);
  let z0 = select(0u, c.z - 1u, c.z > 0u); let z1 = min(c.z + 1u, G.nz - 1u);
  var sum = vec3f(0.0);
  for (var z = z0; z <= z1; z++) {
    for (var y = y0; y <= y1; y++) {
      for (var x = x0; x <= x1; x++) {
        let b = bucketOf(vec3<u32>(x, y, z));
        let end = scanData[b + 1u];
        for (var s = scanData[b]; s < end; s++) {
          let cand = sortedPos[s];
          let u = bitcast<u32>(cand.w);
          if (u == v) { continue; }
          let diff = P - cand.xyz;
          let d2 = dot3(diff, diff);
          if (!(d2 < roi2)) { continue; }
          // several grid cells can share a bucket: take u only for its own cell
          let cu = cellOf(cand.xyz);
          if (cu.x != x || cu.y != y || cu.z != z) { continue; }
          var linked = false;
          for (var k = 0u; k < d; k++) { linked = linked || (topo[base + k] == u); }
          if (linked) { continue; }
          sum += ((roi2 - d2) / roi2) * unitOrZero(diff);
        }
      }
    }
  }
  let rep = G.repulsion * sum;

  var delta = ((spring + planar) + bulge) + rep;
  if (G.maxMove > 0.0) {
    let l2 = dot3(delta, delta);
    if (l2 > G.maxMove * G.maxMove) {
      delta = delta * (G.maxMove / sqrt(l2));
      atomicAdd(&G.clamped, 1u);
    }
  }
  let Pn = P + delta;
  geomNew[v] = vec4f(Pn, geomNew[v].w);   // keep the food written by the first pass
  if (!finite3(Pn)) { atomicMin(&G.firstBadMove, v); }
}
`;

// Division on the rotation system (docs/model.md sections 6 and 7); same code as
// shaders/sim/division.glsl. "vp" is the daughter cell.
const division = /* wgsl */ `
fn degOf(v: u32) -> u32 { return topo[v * 16u + T_DEG]; }
fn ringAt(v: u32, k: u32) -> u32 { return topo[v * 16u + T_NBR + k]; }
fn ringIndex(v: u32, u: u32) -> u32 {
  let d = degOf(v);
  for (var k = 0u; k < d; k++) { if (ringAt(v, k) == u) { return k; } }
  return kInvalid;
}
fn canSplit(v: u32, i: u32, j: u32) -> bool {
  let d = degOf(v);
  if (i == j || i >= d || j >= d) { return false; }
  let jr = (j + d - i) % d;
  if (jr + 2u > kMaxDeg) { return false; }
  if (d - jr + 2u > kMaxDeg) { return false; }
  if (degOf(ringAt(v, i)) + 1u > kMaxDeg) { return false; }
  if (degOf(ringAt(v, j)) + 1u > kMaxDeg) { return false; }
  return true;
}
`;

// Step 6a: division candidates, their priorities and their cleavage pairs.
export const candidates = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomNew: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> topo: array<u32>;
@group(0) @binding(3) var<storage, read_write> sel: array<u32>;   // 6 per cell: prio, m1, m2 as (lo, hi)
${math}
${hash}
${index}
${division}
fn cleavageLength(v: u32, i: u32, j: u32, P: vec3f) -> f32 {
  return length3(geomNew[ringAt(v, i)].xyz - P) + length3(geomNew[ringAt(v, j)].xyz - P);
}
// Packed (i << 4) | j, or kNoPair when no pair respects the degree cap.
fn chooseCleavagePair(v: u32, tie: U64) -> u32 {
  let d = degOf(v);
  let offs0 = d / 2u; let offs1 = (d + 1u) / 2u;
  let nOffs = select(2u, 1u, offs0 == offs1);
  var tiesI: array<u32, 24>;
  var tiesJ: array<u32, 24>;
  var nTies = 0u;
  var best = 0xFFFFFFFFu;
  for (var i = 0u; i < d; i++) {
    for (var o = 0u; o < nOffs; o++) {
      let j = (i + select(offs1, offs0, o == 0u)) % d;
      if (!canSplit(v, i, j)) { continue; }
      let da = degOf(ringAt(v, i)); let db = degOf(ringAt(v, j));
      let cost = max(da, db) * 100u + da + db;
      if (cost < best) { best = cost; nTies = 0u; }
      if (cost == best) { tiesI[nTies] = i; tiesJ[nTies] = j; nTies++; }
    }
  }
  if (nTies == 0u) { return kNoPair; }
  // tie % nTies for a 64-bit tie value
  let pow32 = ((0xFFFFFFFFu % nTies) + 1u) % nTies;
  var pick = ((tie.hi % nTies) * pow32 + tie.lo % nTies) % nTies;
  if (nTies > 1u) {
    // ties: the pair with the shortest cleavage links, walking from the hashed choice
    let P = geomNew[v].xyz;
    var bestLen = cleavageLength(v, tiesI[pick], tiesJ[pick], P);
    let start = pick;
    for (var k = 1u; k < nTies; k++) {
      let q = (start + k) % nTies;
      let len = cleavageLength(v, tiesI[q], tiesJ[q], P);
      if (len < bestLen) { bestLen = len; pick = q; }
    }
  }
  return (tiesI[pick] << 4u) | tiesJ[pick];
}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  var prio = U64(0u, 0u);
  var pair = kNoPair;
  if (geomNew[v].w > G.threshold) {
    atomicAdd(&G.ready, 1u);
    pair = chooseCleavagePair(v, hashU64(kStreamPair, v));
    if (pair == kNoPair) {
      atomicAdd(&G.postponed, 1u);
    } else {
      // (hash31 << 32 | id) + 1: unique and never zero
      prio = U64(v + 1u, hashU64(kStreamPriority, v).hi >> 1u);
    }
  }
  sel[v * 6u] = prio.lo;
  sel[v * 6u + 1u] = prio.hi;
  topo[v * 16u + T_PAIR] = pair;
}
`;

// Step 6b: "max over self and neighbours" (round 0: prio -> m1, round 1: m1 -> m2).
export const maxRing = (round) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> topo: array<u32>;
@group(0) @binding(2) var<storage, read_write> sel: array<u32>;
${index}
fn greater(aLo: u32, aHi: u32, bLo: u32, bHi: u32) -> bool { return aHi > bHi || (aHi == bHi && aLo > bLo); }

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let src = ${round * 2}u;
  var lo = sel[v * 6u + src];
  var hi = sel[v * 6u + src + 1u];
  let d = topo[v * 16u + T_DEG];
  for (var k = 0u; k < d; k++) {
    let u = topo[v * 16u + T_NBR + k];
    let ulo = sel[u * 6u + src];
    let uhi = sel[u * 6u + src + 1u];
    if (greater(ulo, uhi, lo, hi)) { lo = ulo; hi = uhi; }
  }
  sel[v * 6u + src + 2u] = lo;
  sel[v * 6u + src + 3u] = hi;
}
`;

// Step 6c: 1 for every winner; the prefix sum gives the winner's rank.
export const winFlag = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> sel: array<u32>;
@group(0) @binding(2) var<storage, read_write> scanData: array<u32>;
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let lo = sel[v * 6u]; let hi = sel[v * 6u + 1u];
  let winner = (lo != 0u || hi != 0u) && lo == sel[v * 6u + 4u] && hi == sel[v * 6u + 5u];
  scanData[v] = select(0u, 1u, winner);
}
`;

// Step 7: every winner splits, unless the step failed or would exceed the capacity.
export const split = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> nrmNew: array<vec4f>;
@group(0) @binding(3) var<storage, read_write> topo: array<u32>;
@group(0) @binding(4) var<storage, read> sel: array<u32>;
@group(0) @binding(5) var<storage, read> scanData: array<u32>;
${math}
${index}
${division}
fn unitOrZero(v: vec3f) -> vec3f {
  let l2 = dot3(v, v);
  if (l2 < G.unitEps2) { return vec3f(0.0); }
  let inv = 1.0 / sqrt(l2);
  return vec3f(v.x * inv, v.y * inv, v.z * inv);
}
fn setRing(v: u32, k: u32, x: u32) { topo[v * 16u + T_NBR + k] = x; }

fn splitRings(v: u32, i: u32, j: u32, vp: u32) {
  let d = degOf(v);
  var rot: array<u32, 12>;
  for (var k = 0u; k < d; k++) { rot[k] = ringAt(v, (i + k) % d); }
  let jr = (j + d - i) % d;
  let a = rot[0]; let b = rot[jr];
  // new ring(v) = K followed by vp
  for (var k = 0u; k <= jr; k++) { setRing(v, k, rot[k]); }
  setRing(v, jr + 1u, vp);
  for (var k = jr + 2u; k < kMaxDeg; k++) { setRing(v, k, kInvalid); }
  topo[v * 16u + T_DEG] = jr + 2u;
  // new ring(vp) = M followed by v
  var m = 0u;
  for (var k = jr; k < d; k++) { setRing(vp, m, rot[k]); m++; }
  setRing(vp, m, a); m++;
  setRing(vp, m, v); m++;
  for (var k = m; k < kMaxDeg; k++) { setRing(vp, k, kInvalid); }
  topo[vp * 16u + T_DEG] = m;
  // every w strictly inside M: replace v by vp in the same slot
  for (var k = jr + 1u; k < d; k++) {
    let w = rot[k];
    let dw = degOf(w);
    for (var q = 0u; q < dw; q++) {
      if (ringAt(w, q) == v) { setRing(w, q, vp); break; }
    }
  }
  // ring(a): insert vp immediately after v.  ring(b): insert vp immediately before v.
  {
    let slot = ringIndex(a, v) + 1u;
    let dc = degOf(a);
    for (var q = dc; q > slot; q--) { setRing(a, q, ringAt(a, q - 1u)); }
    setRing(a, slot, vp);
    topo[a * 16u + T_DEG] = dc + 1u;
  }
  {
    let slot = ringIndex(b, v);
    let dc = degOf(b);
    for (var q = dc; q > slot; q--) { setRing(b, q, ringAt(b, q - 1u)); }
    setRing(b, slot, vp);
    topo[b * 16u + T_DEG] = dc + 1u;
  }
}

fn applyDivision(v: u32, pair: u32, vp: u32) {
  let d = degOf(v);
  let i = pair >> 4u; let j = pair & 0xFu;
  let jr = (j + d - i) % d;
  let a = ringAt(v, i); let b = ringAt(v, j);
  let P = geomNew[v].xyz;
  let N = nrmNew[v].xyz;
  var dir = vec3f(0.0);
  let moved = d - jr - 1u;
  if (moved > 0u) {
    var c = vec3f(0.0);
    for (var k = jr + 1u; k < d; k++) { c += geomNew[ringAt(v, (i + k) % d)].xyz; }
    dir = unitOrZero(c * (1.0 / f32(moved)) - P);
  }
  if (dot3(dir, dir) == 0.0) { dir = unitOrZero(cross3(N, geomNew[b].xyz - geomNew[a].xyz)); }
  if (dot3(dir, dir) == 0.0) {
    let ax = abs(N.x); let ay = abs(N.y); let az = abs(N.z);
    var axis = vec3f(0.0, 0.0, 1.0);
    if (ax <= ay && ax <= az) { axis = vec3f(1.0, 0.0, 0.0); } else if (ay <= az) { axis = vec3f(0.0, 1.0, 0.0); }
    dir = unitOrZero(cross3(N, axis));
  }
  if (dot3(dir, dir) == 0.0) { dir = vec3f(1.0, 0.0, 0.0); }

  splitRings(v, i, j, vp);

  let delta = G.daughterOffset * G.l0;
  let half = 0.5 * geomNew[v].w;
  geomNew[vp] = vec4f(P + delta * dir, half);
  geomNew[v] = vec4f(P - delta * dir, half);
  nrmNew[vp] = vec4f(N, 0.0);
  topo[vp * 16u + T_BIRTH] = G.stepLo;
  let g = topo[v * 16u + T_GEN] + 1u;
  topo[v * 16u + T_GEN] = g;
  topo[vp * 16u + T_GEN] = g;
}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let ok = atomicLoad(&G.firstBadMove) == kInvalid && atomicLoad(&G.firstBadPos) == kInvalid && G.gridTooLarge == 0u &&
           G.n + G.winners <= G.limit;
  if (!ok) { return; }
  let lo = sel[v * 6u]; let hi = sel[v * 6u + 1u];
  if ((lo != 0u || hi != 0u) && lo == sel[v * 6u + 4u] && hi == sel[v * 6u + 5u]) {
    applyDivision(v, topo[v * 16u + T_PAIR], G.n + scanData[v]);
  }
}
`;

// One invocation: commit the step (cell count, step counter, totals) or record the failure.
// failCode: 1 non-finite position at the start, 2 grid too large, 3 non-finite new position,
// 4 capacity reached. After a failure no further step does anything: the state stays at the
// last good step.
export const commit = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@compute @workgroup_size(1)
fn main() {
  if (G.failed != 0u || G.n == 0u) { return; }
  var code = 0u;
  if (atomicLoad(&G.firstBadPos) != kInvalid) { code = 1u; }
  else if (G.gridTooLarge != 0u) { code = 2u; }
  else if (atomicLoad(&G.firstBadMove) != kInvalid) { code = 3u; }
  else if (G.n + G.winners > G.limit) { code = 4u; }
  if (code != 0u) {
    G.failed = 1u; G.failCode = code; G.failStepLo = G.stepLo;
    return;
  }
  G.n = G.n + G.winners;
  let lo = G.stepLo + 1u;
  if (lo == 0u) { G.stepHi = G.stepHi + 1u; }
  G.stepLo = lo;
  let dl = G.divisionsLo + G.winners;
  if (dl < G.divisionsLo) { G.divisionsHi = G.divisionsHi + 1u; }
  G.divisionsLo = dl;
  let c = atomicLoad(&G.clamped);
  let cl = G.clampedLo + c;
  if (cl < G.clampedLo) { G.clampedHi = G.clampedHi + 1u; }
  G.clampedLo = cl;
}
`;

// Rendering: one camera-facing quad per cell, shaded as a sphere with correct depth.
export const render = /* wgsl */ `
struct Camera { viewProj: mat4x4f, view: mat4x4f, proj: mat4x4f, params: vec4f }   // params: radius, step, ageScale, mode
@group(0) @binding(0) var<uniform> cam: Camera;
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f, @location(1) centre: vec3f, @location(2) shade: f32 }

@vertex
fn vs(@builtin(vertex_index) vi: u32, @location(0) geom: vec4f, @location(1) birth: u32) -> VOut {
  let corner = vec2f(f32(vi & 1u), f32((vi >> 1u) & 1u)) * 2.0 - 1.0;
  let c = (cam.view * vec4f(geom.xyz, 1.0)).xyz;
  let r = cam.params.x;
  var o: VOut;
  o.pos = cam.proj * vec4f(c + vec3f(corner * r * 1.05, 0.0), 1.0);
  o.uv = corner * 1.05;
  o.centre = c;
  let age = (cam.params.y - f32(birth)) / max(cam.params.z, 1.0);
  o.shade = select(1.0, 1.0 - clamp(age, 0.0, 1.0), cam.params.w > 0.5);
  return o;
}

struct FOut { @location(0) color: vec4f, @builtin(frag_depth) depth: f32 }
@fragment
fn fs(i: VOut) -> FOut {
  let r2 = dot(i.uv, i.uv);
  if (r2 > 1.0) { discard; }
  let n = vec3f(i.uv, sqrt(1.0 - r2));
  let hit = i.centre + n * cam.params.x;
  let clip = cam.proj * vec4f(hit, 1.0);
  let l = normalize(vec3f(0.35, 0.55, 0.75));
  let diffuse = max(dot(n, l), 0.0);
  let base = mix(vec3f(0.80, 0.74, 0.66), mix(vec3f(0.10, 0.20, 0.45), vec3f(1.0, 0.86, 0.45), i.shade), select(0.0, 1.0, cam.params.w > 0.5));
  var o: FOut;
  o.color = vec4f(pow(base * (0.35 + 0.65 * diffuse), vec3f(1.0 / 2.2)), 1.0);
  o.depth = clip.z / clip.w;
  return o;
}
`;
