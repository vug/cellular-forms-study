// WGSL compute shaders of the WebGPU port (M9). Same pipeline as shaders/sim (the Vulkan
// compute backend), with what WebGPU makes different:
//   - no 64-bit integers: the hashes of core/hash.hpp run on (lo, hi) pairs of u32;
//   - no push constants and a limit of 8 storage buffers per shader: everything global lives in
//     one `Globals` buffer, and per-cell data is packed (position + food, 16 u32 of topology,
//     the two morphogens in one vec2);
//   - reading anything back is asynchronous and slow, so the cell count, the step counter and
//     the dispatch sizes stay on the GPU (indirect dispatch) and many steps go out per submit.
//
// The whole model of src/core is here: the base model, the food sources of core/food.hpp
// (field, pulses, light with a shadow map, diffusion and decay, crowding), oriented division,
// the field-dependent bulge and the morphogens of core/morph.hpp.
//
// Arithmetic. A GPU's `a / b` and `sqrt` are not the correctly rounded IEEE operations (measured
// on the test GPU, ?task=floats: about a quarter of the quotients and one root in eight are
// not), it flushes subnormal numbers to zero, and it has no doubles. `+`, `-` and `*` on normal
// numbers are IEEE there, and `a * b + c` was not contracted into a fused multiply-add (WGSL
// cannot forbid that; ?task=floats tests it). So:
//   - everything that decides a cell's food, the morphogens, the cell's normal when a model
//     extension is on, and the oriented cleavage pair is computed with the exact operations of
//     `exactf` below (integer arithmetic on the bits), and the shadow map's frame with the
//     software doubles of `softd`. Those values are bit-identical to the CPU reference;
//   - the base model's geometry keeps the GPU's own division and sqrt, as before (positions
//     agree with the CPU to an ulp or two per step), unless the simulation runs in "exact" mode
//     (sim.js): then the move, the grid, the cleavage lengths and the split use the exact
//     operations as well, and a step is bit-identical to the CPU's.

export const ROW = 32768 * 256;  // invocations per dispatch row (x), so y extends past 65535 groups
export const GLOBALS_BYTES = 608;
export const PARAM_BLOCK = { offset: 320, bytes: 256 };  // the extension parameters inside Globals

// Bits of Globals.flags (the food sources that are on; core/food.hpp) and Globals.xflags.
export const F = { PULSE: 1, FIELD: 2, LIGHT: 4, DIFFUSE: 8, CROWD: 16, MORPH: 32 };
export const X = { FIELD_BULGE: 1, MORPH_ON: 2, MORPH_PRESENT: 4, ORIENT: 8, EXACT: 16 };

const fbits = (x) => `bitcast<f32>(${new Uint32Array(new Float32Array([x]).buffer)[0]}u)`;
const u64lit = (v) => `U64(${Number(v & 0xFFFFFFFFn)}u, ${Number((v >> 32n) & 0xFFFFFFFFn)}u)`;

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
  // --- the model beyond the base model: parameters (written by the page; the values of
  // FoodParams in core/food.hpp and MorphParams in core/morph.hpp, prepared on the host)
  flags: u32, xflags: u32, zero: u32, pulsePeriod: u32,                        // 320
  pulseOn: u32, pulseLow: f32, fieldAmount: f32, fieldInvScale: f32,           // 336
  fieldContrast: f32, fieldOctaves: u32, fieldBulge: f32, lightAmount: f32,    // 352
  lightAmbient: f32, lightBias: f32, lightInvPixel0: f32, lightPixExp: i32,    // 368
  lightPixLo: u32, lightPixHi: u32, diffusion: f32, keep: f32,                 // 384
  lightDir: vec4f, lightU: vec4f, lightV: vec4f,                               // 400, 416, 432
  crowdLow: f32, crowdInvRange: f32, invL0: f32, invRoi: f32,                  // 448
  morphFood: f32, morphBulge: f32, morphPlanar: f32, morphRest: f32,           // 464
  morphDiffA: f32, morphDiffB: f32, morphFeed: f32, morphKillFeed: f32,        // 480
  morphSeedLevel: f32, morphSeedInvScale: f32, orientMargin: u32, tableCap: u32,  // 496
  orientAxis: vec4f,                                                           // 512
  fieldSeed: array<vec4<u32>, 3>,                                              // 528: (lo, hi) of the noise seed of octaves 0..5
  // --- per step, computed on the GPU: the pulse's scale and the frame of the shadow map
  pulse: f32, lightU0: f32, lightV0: f32, lightInvPixel: f32,                  // 576
  lightNu: u32, lightNv: u32, pad1: u32, pad2: u32,                            // 592
}
const kMaxDeg = 12u;
const kInvalid = 0xFFFFFFFFu;
const kNoPair = 0xFFu;
const ROW = ${ROW}u;
// topology record: 16 u32 per cell
const T_DEG = 0u; const T_BIRTH = 1u; const T_GEN = 2u; const T_PAIR = 3u; const T_NBR = 4u;
// Globals.flags: the food sources that are on (core/food.hpp)
const F_PULSE = 1u; const F_FIELD = 2u; const F_LIGHT = 4u; const F_DIFFUSE = 8u; const F_CROWD = 16u; const F_MORPH = 32u;
// Globals.xflags
const X_FIELD_BULGE = 1u;     // the field scales the bulge factor
const X_MORPH_ON = 2u;        // the morphogens react and set local parameters
const X_MORPH_PRESENT = 4u;   // the cells have morphogen concentrations (they stay once created)
const X_ORIENT = 8u;          // oriented division
const X_EXACT = 16u;          // geometry in exact arithmetic (bit-identical to the CPU)
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
const streamKey = (stream) => u64lit((0x9E3779B97F4A7C15n * BigInt(stream + 1)) & 0xFFFFFFFFFFFFFFFFn);

// 64-bit arithmetic on (lo, hi) pairs, and the mixing function of src/core/hash.hpp.
const u64 = /* wgsl */ `
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
`;

// More 64-bit arithmetic, for the exact float operations and the software doubles.
const u64x = /* wgsl */ `
fn sub64(a: U64, b: U64) -> U64 { return U64(a.lo - b.lo, a.hi - b.hi - select(0u, 1u, a.lo < b.lo)); }
fn lt64(a: U64, b: U64) -> bool { return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
// Shifts by 0 <= s < 64.
fn shl64(a: U64, s: u32) -> U64 {
  if (s == 0u) { return a; }
  if (s >= 32u) { return U64(0u, a.lo << (s - 32u)); }
  return U64(a.lo << s, (a.hi << s) | (a.lo >> (32u - s)));
}
fn shr64(a: U64, s: u32) -> U64 {
  if (s == 0u) { return a; }
  if (s >= 32u) { return U64(a.hi >> (s - 32u), 0u); }
  return U64((a.lo >> s) | (a.hi << (32u - s)), a.hi >> s);
}
`;

// The hashes of src/core/hash.hpp.
const hash = /* wgsl */ `
${u64}
// 0x9E3779B97F4A7C15 * (stream + 1) for streams Food = 1, Priority = 2, Pair = 3, Morph = 5
const kStreamFood = ${streamKey(1)};
const kStreamPriority = ${streamKey(2)};
const kStreamPair = ${streamKey(3)};
const kStreamMorph = ${streamKey(5)};
fn hashU64(streamKey: U64, id: u32) -> U64 {
  var h = mix64(add64(U64(G.seedLo, G.seedHi), streamKey));
  h = mix64(xor64(h, U64(G.stepLo, G.stepHi)));
  return mix64(xor64(h, U64(id, 0u)));
}
// Uniform float in [0, 1) from the top 24 bits.
fn hashU01(streamKey: U64, id: u32) -> f32 { return f32(hashU64(streamKey, id).hi >> 8u) * (1.0 / 16777216.0); }
`;

// Exact single-precision arithmetic (needs u64 and u64x). Every function works on the bits with
// integer operations, so its result is the IEEE one (round to nearest, ties to even) whatever
// the GPU's own float division, square root and treatment of subnormal numbers are.
//   fdivx, fsqrtx   correctly rounded a / b and sqrt(x)
//   fmul, fadd      a * b and a + b: the GPU's own (IEEE for normal numbers), handed to sfmul /
//                   sfadd when an operand or the result can be subnormal
const exactf = /* wgsl */ `
fn expOf(u: u32) -> u32 { return (u >> 23u) & 0xFFu; }
// |x| = fman * 2^fexp, fman < 2^24
fn fman(u: u32) -> u32 { return select((u & 0x7FFFFFu) | 0x800000u, u & 0x7FFFFFu, expOf(u) == 0u); }
fn fexp(u: u32) -> i32 { return select(i32(expOf(u)) - 150, -149, expOf(u) == 0u); }

fn fdivx(a: f32, b: f32) -> f32 {
  let ua = bitcast<u32>(a); let ub = bitcast<u32>(b);
  let sign = (ua ^ ub) & 0x80000000u;
  var ea = i32(expOf(ua)); var ma = ua & 0x7FFFFFu;
  var eb = i32(expOf(ub)); var mb = ub & 0x7FFFFFu;
  if (ea == 255 || eb == 255 || (eb == 0 && mb == 0u)) { return a / b; }   // infinite, NaN, division by zero: as the GPU has them
  if (ea == 0 && ma == 0u) { return bitcast<f32>(sign); }
  if (ea == 0) { let s = countLeadingZeros(ma) - 8u; ma = ma << s; ea = 1 - i32(s); } else { ma |= 0x800000u; }
  if (eb == 0) { let s = countLeadingZeros(mb) - 8u; mb = mb << s; eb = 1 - i32(s); } else { mb |= 0x800000u; }
  var e = ea - eb + 127;
  if (ma < mb) { ma = ma << 1u; e -= 1; }
  // 26 bits of the quotient (it is in [1, 2)): the leading 1, then 8 + 8 + 8 + 1 more.
  // (Starting from the GPU's quotient and correcting it with one exact product was tried
  // instead of the three integer divisions: no faster on the test GPU.)
  var r = ma - mb;
  var q = 1u;
  for (var i = 0u; i < 3u; i++) {
    let t = r << 8u;
    let d = t / mb;
    r = t - d * mb;
    q = (q << 8u) | d;
  }
  r = r << 1u;
  q = q << 1u;
  if (r >= mb) { r -= mb; q |= 1u; }
  let sticky = r != 0u;
  if (e >= 1) {
    var m = q >> 2u;
    let rb = q & 3u;
    if (rb == 3u || (rb == 2u && (sticky || (m & 1u) == 1u))) { m += 1u; }
    if (m == 0x1000000u) { m = 0x800000u; e += 1; }
    if (e >= 255) { return bitcast<f32>(sign | 0x7F800000u); }
    return bitcast<f32>(sign | (u32(e) << 23u) | (m & 0x7FFFFFu));
  }
  // subnormal result: one rounding, at the 2^-149 grid
  let sh = u32(3 - e);
  if (sh > 27u) { return bitcast<f32>(sign); }
  var m = q >> sh;
  let rem = q & ((1u << sh) - 1u);
  let half = 1u << (sh - 1u);
  if (rem > half || (rem == half && (sticky || (m & 1u) == 1u))) { m += 1u; }
  return bitcast<f32>(sign | m);
}

fn fsqrtx(x: f32) -> f32 {
  let ux = bitcast<u32>(x);
  var e = i32(expOf(ux)); var m = ux & 0x7FFFFFu;
  if ((ux >> 31u) != 0u || e == 255 || (ux << 1u) == 0u) { return sqrt(x); }   // negative, infinite, NaN, zero
  if (e == 0) { let s = countLeadingZeros(m) - 8u; m = m << s; e = 1 - i32(s); } else { m |= 0x800000u; }
  // x = m * 2^(e - 150). X = m * 2^s with e - 150 - s even: sqrt(X) then has exactly 24 bits.
  let s = select(23u, 24u, (e & 1) == 0);
  let X = shl64(U64(m, 0u), s);
  // The GPU's root as a first guess, then exact: r^2 <= X < (r + 1)^2, stepping r by one
  // ((r - 1)^2 = r^2 - (2 r - 1) and (r + 1)^2 = r^2 + 2 r + 1, so a step costs no product).
  var r = clamp(u32(sqrt(f32(m) * select(8388608.0, 16777216.0, s == 24u))), 0x800000u, 0xFFFFFFu);
  var rr = mul32(r, r);
  for (var i = 0u; i < 64u && lt64(X, rr); i++) { rr = sub64(rr, U64(2u * r - 1u, 0u)); r -= 1u; }
  for (var i = 0u; i < 64u; i++) {
    let up = add64(rr, U64(2u * r + 1u, 0u));
    if (lt64(X, up)) { break; }
    rr = up; r += 1u;
  }
  let rem = sub64(X, rr);
  if (rem.hi != 0u || rem.lo > r) { r += 1u; }             // sqrt(X) > r + 1/2
  var er = (e - 150 - i32(s)) / 2 + 150;
  if (r == 0x1000000u) { r = 0x800000u; er += 1; }
  return bitcast<f32>((u32(er) << 23u) | (r & 0x7FFFFFu));
}

fn sfmul(a: f32, b: f32) -> f32 {
  let ua = bitcast<u32>(a); let ub = bitcast<u32>(b);
  let sign = (ua ^ ub) & 0x80000000u;
  var ea = i32(expOf(ua)); var ma = ua & 0x7FFFFFu;
  var eb = i32(expOf(ub)); var mb = ub & 0x7FFFFFu;
  if (ea == 255 || eb == 255) { return a * b; }
  if ((ea == 0 && ma == 0u) || (eb == 0 && mb == 0u)) { return bitcast<f32>(sign); }
  if (ea == 0) { let s = countLeadingZeros(ma) - 8u; ma = ma << s; ea = 1 - i32(s); } else { ma |= 0x800000u; }
  if (eb == 0) { let s = countLeadingZeros(mb) - 8u; mb = mb << s; eb = 1 - i32(s); } else { mb |= 0x800000u; }
  var p = mul32(ma, mb);                 // in [2^46, 2^48)
  var e = ea + eb - 126;                 // exponent field when the leading bit is bit 47
  if (p.hi < 0x8000u) { p = shl64(p, 1u); e -= 1; }
  if (e >= 1) {
    var m = (p.hi << 8u) | (p.lo >> 24u);
    let rem = p.lo & 0xFFFFFFu;
    if (rem > 0x800000u || (rem == 0x800000u && (m & 1u) == 1u)) { m += 1u; }
    if (m == 0x1000000u) { m = 0x800000u; e += 1; }
    if (e >= 255) { return bitcast<f32>(sign | 0x7F800000u); }
    return bitcast<f32>(sign | (u32(e) << 23u) | (m & 0x7FFFFFu));
  }
  let sh = u32(25 - e);
  if (sh > 48u) { return bitcast<f32>(sign); }
  let mm = shr64(p, sh);
  var m = mm.lo;
  let rem = sub64(p, shl64(mm, sh));
  let half = shl64(U64(1u, 0u), sh - 1u);
  if (lt64(half, rem) || (rem.lo == half.lo && rem.hi == half.hi && (m & 1u) == 1u)) { m += 1u; }
  return bitcast<f32>(sign | m);
}

fn sfadd(a: f32, b: f32) -> f32 {
  var ua = bitcast<u32>(a); var ub = bitcast<u32>(b);
  if ((ua << 1u) == 0u) {
    if ((ub << 1u) == 0u) { return bitcast<f32>(ua & ub); }
    return b;
  }
  if ((ub << 1u) == 0u) { return a; }
  if ((ua & 0x7FFFFFFFu) < (ub & 0x7FFFFFFFu)) { let t = ua; ua = ub; ub = t; }   // |a| >= |b|
  let sign = ua & 0x80000000u;
  var ea = i32(expOf(ua)); var ma = ua & 0x7FFFFFu;
  var eb = i32(expOf(ub)); var mb = ub & 0x7FFFFFu;
  if (ea == 255) { return a + b; }
  if (ea == 0) { ea = 1; } else { ma |= 0x800000u; }
  if (eb == 0) { eb = 1; } else { mb |= 0x800000u; }
  // three more bits below the mantissa: guard, round, and "something was lost"
  let big = ma << 3u;
  var small = mb << 3u;
  let shift = u32(ea - eb);
  if (shift >= 27u) { small = 1u; }
  else if (shift > 0u) { small = (small >> shift) | select(0u, 1u, (small & ((1u << shift) - 1u)) != 0u); }
  var s = 0u;
  var e = ea;
  if (((ua ^ ub) & 0x80000000u) == 0u) {
    s = big + small;
    if (s >= 0x8000000u) { s = (s >> 1u) | (s & 1u); e += 1; }
  } else {
    s = big - small;
    if (s == 0u) { return 0.0; }
    let lead = 31u - countLeadingZeros(s);
    if (lead < 26u) { let sh = min(26u - lead, u32(e - 1)); s = s << sh; e -= i32(sh); }
  }
  var m = s >> 3u;
  let rb = s & 7u;
  if (rb > 4u || (rb == 4u && (m & 1u) == 1u)) { m += 1u; }
  if (m == 0x1000000u) { m = 0x800000u; e += 1; }
  if (m >= 0x800000u) {
    if (e >= 255) { return bitcast<f32>(sign | 0x7F800000u); }
    return bitcast<f32>(sign | (u32(e) << 23u) | (m & 0x7FFFFFu));
  }
  return bitcast<f32>(sign | m);
}

// a * b. The GPU's product is the IEEE one unless an operand is subnormal or the product can be
// below the smallest normal number (2^-126): those go through sfmul.
fn fmul(a: f32, b: f32) -> f32 {
  let ua = bitcast<u32>(a); let ub = bitcast<u32>(b);
  let ea = expOf(ua); let eb = expOf(ub);
  if ((ua << 1u) != 0u && (ub << 1u) != 0u && (ea == 0u || eb == 0u || ea + eb < 128u)) { return sfmul(a, b); }
  return a * b;
}
// a + b. Through sfadd when an operand is subnormal, or when the operands have opposite signs
// and are small enough for their difference to be subnormal.
fn fadd(a: f32, b: f32) -> f32 {
  let ua = bitcast<u32>(a); let ub = bitcast<u32>(b);
  let ea = expOf(ua); let eb = expOf(ub);
  let za = (ua << 1u) == 0u; let zb = (ub << 1u) == 0u;
  if ((ea == 0u && !za) || (eb == 0u && !zb) || (!za && !zb && ((ua ^ ub) >> 31u) != 0u && min(ea, eb) < 26u)) { return sfadd(a, b); }
  return a + b;
}
// -x, on the bits: the GPU's own negation (and abs()) flush a subnormal number to zero as well.
// The mask is the sign bit as a value the shader compiler cannot see through: it turns
// "flip the sign bit" back into a float negation otherwise (found with a gallery form whose
// activator fades through the subnormal range). opaqueZero() comes from the including shader.
fn fneg(x: f32) -> f32 { return bitcast<f32>(bitcast<u32>(x) ^ (0x80000000u | opaqueZero())); }
// x < 0 ? 0 : (x > 1 ? 1 : x), with the first test on the bits for the same reason.
fn clamp01(x: f32) -> f32 {
  let u = bitcast<u32>(x);
  if ((u >> 31u) != 0u && (u << 1u) != 0u && (u & 0x7FFFFFFFu) <= 0x7F800000u) { return 0.0; }
  if (x > 1.0) { return 1.0; }
  return x;
}
// 1 / d for d = 0 .. 12 as the correctly rounded floats (kRecip[0] is unused).
const kRecip = array<f32, 13>(0.0, 1.0, 0.5, ${[3, 4, 5, 6, 7, 8, 9, 10, 11, 12].map((d) => fbits(1 / d)).join(', ')});
fn recip(d: u32) -> f32 {
  var table = kRecip;
  return table[min(d, 12u)];
}
`;

// exactf for the shaders of the simulation: the zero the compiler cannot see through is a
// field of Globals that the page keeps at 0.
const exactG = /* wgsl */ `
${exactf}
fn opaqueZero() -> u32 { return G.zero; }
`;

// Software doubles (needs u64, u64x, exactf): the CPU reference sizes the grid and the shadow
// map in double precision, and a difference of one texel or one grid cell would change which
// cells shade each other, or the order of a sum. Only what those two computations need:
// non-negative values, round to nearest even, no subnormal doubles (the operands are floats).
const softd = /* wgsl */ `
struct UD { m: U64, e: i32 }       // m * 2^e with m = 0 or 2^52 <= m < 2^53
fn top64(a: U64) -> u32 {
  if (a.hi != 0u) { return 63u - countLeadingZeros(a.hi); }
  return 31u - countLeadingZeros(a.lo);
}
// The double nearest to s * 2^e. (A lost remainder is passed in as bit 0 of s.)
fn roundD(s: U64, e: i32) -> UD {
  if ((s.lo | s.hi) == 0u) { return UD(s, 0); }
  let lead = top64(s);
  if (lead <= 52u) { return UD(shl64(s, 52u - lead), e - i32(52u - lead)); }
  let r = lead - 52u;
  var m = shr64(s, r);
  let rem = sub64(s, shl64(m, r)).lo;
  let half = 1u << (r - 1u);
  var er = e + i32(r);
  if (rem > half || (rem == half && (m.lo & 1u) == 1u)) {
    m = add64(m, U64(1u, 0u));
    if (m.hi == 0x200000u) { m = U64(0u, 0x100000u); er += 1; }
  }
  return UD(m, er);
}
// double(hi) - double(lo) for floats hi >= lo.
fn dsubF(hi: f32, lo: f32) -> UD {
  let uh = bitcast<u32>(hi); let ul = bitcast<u32>(lo);
  var ua = uh; var ub = ul;
  if ((ua & 0x7FFFFFFFu) < (ub & 0x7FFFFFFFu)) { ua = ul; ub = uh; }     // the larger magnitude first
  let ma = fman(ua); let mb = fman(ub);
  let ea = fexp(ua); let eb = fexp(ub);
  let A = shl64(U64(ma, 0u), 38u);
  var B = shl64(U64(mb, 0u), 38u);
  let shift = u32(ea - eb);
  if (mb != 0u) {
    if (shift >= 64u) { B = U64(1u, 0u); }
    else if (shift > 0u) {
      let t = shr64(B, shift);
      let lost = sub64(B, shl64(t, shift));
      B = U64(t.lo | select(0u, 1u, (lost.lo | lost.hi) != 0u), t.hi);
    }
  }
  var S = sub64(A, B);
  if (((uh ^ ul) & 0x80000000u) != 0u) { S = add64(A, B); }
  return roundD(S, ea - 38);
}
// a / b, b > 0.
fn ddiv(a: UD, b: UD) -> UD {
  if ((a.m.lo | a.m.hi) == 0u) { return a; }
  var rem = a.m;
  var e = a.e - b.e - 54;
  if (lt64(rem, b.m)) { rem = shl64(rem, 1u); e -= 1; }
  var q = U64(0u, 0u);
  for (var i = 0u; i < 55u; i++) {
    q = shl64(q, 1u);
    if (!lt64(rem, b.m)) { rem = sub64(rem, b.m); q.lo |= 1u; }
    rem = shl64(rem, 1u);
  }
  if ((rem.lo | rem.hi) != 0u) { q.lo |= 1u; }
  return roundD(q, e);
}
// a * double(f), f > 0.
fn dmulF(a: UD, f: f32) -> UD {
  if ((a.m.lo | a.m.hi) == 0u) { return a; }
  let uf = bitcast<u32>(f);
  var mf = fman(uf);
  var ef = fexp(uf);
  if (mf == 0u) { return UD(U64(0u, 0u), 0); }
  if (mf < 0x800000u) { let s = countLeadingZeros(mf) - 8u; mf = mf << s; ef -= i32(s); }
  // the 96-bit product (w2, w1, w0), then its top 61 bits with the rest as a sticky bit
  let p0 = mul32(a.m.lo, mf);
  let p1 = mul32(a.m.hi, mf);
  let w0 = p0.lo;
  let w1 = p0.hi + p1.lo;
  let w2 = p1.hi + select(0u, 1u, w1 < p0.hi);
  let S = U64((w0 >> 16u) | (w1 << 16u) | select(0u, 1u, (w0 & 0xFFFFu) != 0u), (w1 >> 16u) | (w2 << 16u));
  return roundD(S, a.e + ef + 16);
}
// floor(a), saturated at 2^32 - 1.
fn floorU(a: UD) -> u32 {
  if ((a.m.lo | a.m.hi) == 0u || a.e <= -53) { return 0u; }
  if (a.e > -21) { return 0xFFFFFFFFu; }
  return shr64(a.m, u32(-a.e)).lo;
}
`;

// Smooth value noise of src/core/noise.hpp (needs u64).
const noise = /* wgsl */ `
fn lattice01(x: i32, y: i32, z: i32, seed: U64) -> f32 {
  let k = add64(add64(mul64(U64(bitcast<u32>(x), 0u), ${u64lit(0x9E3779B97F4A7C15n)}), mul64(U64(bitcast<u32>(y), 0u), ${u64lit(0xC2B2AE3D27D4EB4Fn)})),
                mul64(U64(bitcast<u32>(z), 0u), ${u64lit(0x165667B19E3779F9n)}));
  return f32(mix64(xor64(seed, k)).hi >> 8u) * (1.0 / 16777216.0);
}
fn lerp1(a: f32, b: f32, t: f32) -> f32 { return a + (b - a) * t; }
fn valueNoise3(p: vec3f, seed: U64) -> f32 {
  let fx = floor(p.x); let fy = floor(p.y); let fz = floor(p.z);
  let ix = i32(fx); let iy = i32(fy); let iz = i32(fz);
  let tx = p.x - fx; let ty = p.y - fy; let tz = p.z - fz;
  let sx = tx * tx * (3.0 - 2.0 * tx); let sy = ty * ty * (3.0 - 2.0 * ty); let sz = tz * tz * (3.0 - 2.0 * tz);
  let c000 = lattice01(ix, iy, iz, seed); let c100 = lattice01(ix + 1, iy, iz, seed);
  let c010 = lattice01(ix, iy + 1, iz, seed); let c110 = lattice01(ix + 1, iy + 1, iz, seed);
  let c001 = lattice01(ix, iy, iz + 1, seed); let c101 = lattice01(ix + 1, iy, iz + 1, seed);
  let c011 = lattice01(ix, iy + 1, iz + 1, seed); let c111 = lattice01(ix + 1, iy + 1, iz + 1, seed);
  let x00 = lerp1(c000, c100, sx); let x10 = lerp1(c010, c110, sx); let x01 = lerp1(c001, c101, sx); let x11 = lerp1(c011, c111, sx);
  return lerp1(lerp1(x00, x10, sy), lerp1(x01, x11, sy), sz);
}
`;

// The spatial field of core/food.hpp (needs G, noise, exactf): contrast-shaped value noise in [0, 1].
const field = /* wgsl */ `
fn fieldValue(P: vec3f) -> f32 {
  var q = P * G.fieldInvScale;
  var sum = 0.0; var norm = 0.0; var amp = 0.5;
  for (var o = 0u; o < G.fieldOctaves; o++) {
    let s = G.fieldSeed[o >> 1u];
    let odd = (o & 1u) == 1u;
    sum += amp * valueNoise3(q, U64(select(s.x, s.z, odd), select(s.y, s.w, odd)));
    norm += amp;
    amp = amp * 0.5;
    q = q * 2.0;
  }
  var f = fdivx(sum, norm);
  f = (f - 0.5) * G.fieldContrast + 0.5;
  return clamp01(f);
}
// The activator as a signal in [0, 1], and the scale of a local parameter (core/morph.hpp).
fn morphSignal(b: f32) -> f32 { return clamp01(b * 3.0); }
fn morphFactor(coupling: f32, b: f32) -> f32 { return 1.0 + coupling * (2.0 * morphSignal(b) - 1.0); }
`;

const index = /* wgsl */ `
// Invocation index of a dispatch laid out by prepare(): rows of 32768 workgroups.
fn cellIndex(gid: vec3<u32>) -> u32 { return gid.x + gid.y * ROW; }          // workgroup_size(256) passes
fn blockIndex(gid: vec3<u32>) -> u32 { return gid.x + gid.y * (ROW / 4u); }  // workgroup_size(64) passes
`;

const dispatch = /* wgsl */ `
fn groups(items: u32, per: u32) -> vec3<u32> {
  let g = (items + per - 1u) / per;
  if (g <= 32768u) { return vec3<u32>(g, 1u, 1u); }
  return vec3<u32>(32768u, (g + 32767u) / 32768u, 1u);
}
fn put(slot: u32, g: vec3<u32>) { ind[slot * 3u] = g.x; ind[slot * 3u + 1u] = g.y; ind[slot * 3u + 2u] = g.z; }
`;

const ordered = /* wgsl */ `
// Floats as ints that sort the same way, so an integer min / max is a float min / max.
fn orderedInt(f: f32) -> i32 { let i = bitcast<i32>(f); return i ^ ((i >> 31u) & 0x7FFFFFFF); }
fn fromOrdered(i: i32) -> f32 { return bitcast<f32>(i ^ ((i >> 31u) & 0x7FFFFFFF)); }
`;

// The shadow map of the light (core/food.hpp): texel and depth of a position.
const shadowFn = /* wgsl */ `
fn shadowTexel(P: vec3f) -> u32 {
  let fu = floor((dot3(P, G.lightU.xyz) - G.lightU0) * G.lightInvPixel);
  let fv = floor((dot3(P, G.lightV.xyz) - G.lightV0) * G.lightInvPixel);
  var iu = 0u; var iv = 0u;
  if (fu > 0.0) { iu = u32(fu); }
  if (fv > 0.0) { iv = u32(fv); }
  if (iu >= G.lightNu) { iu = G.lightNu - 1u; }
  if (iv >= G.lightNv) { iv = G.lightNv - 1u; }
  return iu + G.lightNu * iv;
}
fn shadowDepth(P: vec3f) -> f32 { return dot3(P, G.lightDir.xyz); }
`;

// ---------------------------------------------------------------------------------------------

// One invocation: resets the counters, sizes this step's dispatches from the cell count, and
// works out whether this step is in the "on" part of the food pulses.
export const prepare = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> ind: array<u32>;
${dispatch}
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
// (hi * 2^32 + lo) mod m, for 0 < m <= 2^30
fn mod64(lo: u32, hi: u32, m: u32) -> u32 {
  var r = hi % m;
  for (var i = 0u; i < 32u; i++) {
    r = (r << 1u) | ((lo >> (31u - i)) & 1u);
    if (r >= m) { r -= m; }
  }
  return r;
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
  while (t < n / 2u && t < G.tableCap) { t = t << 1u; }
  G.tableMask = t - 1u;
  put(0u, groups(n, 256u));            // per-cell passes
  put(1u, groups(select(t + 1u, 0u, n == 0u), 64u));  // per-bucket passes
  levels(0u, select(t + 1u, 0u, n == 0u), 2u);        // slots 2, 3, 4: grid scan levels 1..3
  put(5u, vec3<u32>(select(1u, 0u, n == 0u), 1u, 1u));  // single-invocation passes
  levels(1u, n, 6u);                    // slots 6, 7, 8: winner scan levels 1..3
  put(9u, vec3<u32>(0u, 1u, 1u));       // per-texel passes of the shadow map: sized by gridSetup

  // Food pulses (core/food.cpp): "on" for the first pulseOn steps of every period.
  var pulse = 1.0;
  if ((G.flags & F_PULSE) != 0u && mod64(G.stepLo, G.stepHi, max(G.pulsePeriod, 1u)) >= G.pulseOn) { pulse = G.pulseLow; }
  G.pulse = pulse;
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

// Steps 1 and 2: food of the base model, normals, bounds and the finite check.
// exactNormal: the normal in exact arithmetic, as the CPU has it (used whenever anything beyond
// the base model is on: the light and the oriented division read the normal).
export const pre = (exactNormal) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(3) var<storage, read> nrmCur: array<vec4f>;
@group(0) @binding(4) var<storage, read_write> nrmNew: array<vec4f>;
@group(0) @binding(5) var<storage, read> topo: array<u32>;
${math}
${hash}
${exactNormal ? u64x + exactG : ''}
${index}
${ordered}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let cur = geomCur[v];
  let P = cur.xyz;

  // food: uniform random increment (hash of seed, step, cell id). With a food source on, the
  // food pass computes it later in the step (the light needs the normals and the shadow map).
  var food = cur.w;
  if (G.flags == 0u) { food = cur.w + (G.foodMin + G.foodSpan * hashU01(kStreamFood, v)); }

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
    ${exactNormal ? 'nrm = sum * fdivx(1.0, fsqrtx(l2));' : 'nrm = sum * (1.0 / sqrt(l2));'}
  } else {
    atomicAdd(&G.fallbacks, 1u);
  }
  nrmNew[v] = vec4f(nrm, 0.0);
  // The position part is filled in by the move pass; the food is final here (base model).
  geomNew[v] = vec4f(P, food);
}
`;

// One invocation: grid origin and dimensions from the bounds; with the light on, the frame of
// its shadow map (make_food_params in core/food.cpp).
export const gridSetup = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> ind: array<u32>;
${math}
${u64}
${u64x}
${exactG}
${softd}
${dispatch}
${ordered}

@compute @workgroup_size(1)
fn main() {
  if (G.failed != 0u || G.n == 0u) { return; }
  let lo = vec3f(fromOrdered(atomicLoad(&G.minX)), fromOrdered(atomicLoad(&G.minY)), fromOrdered(atomicLoad(&G.minZ)));
  let hi = vec3f(fromOrdered(atomicLoad(&G.maxX)), fromOrdered(atomicLoad(&G.maxY)), fromOrdered(atomicLoad(&G.maxZ)));
  let bad = atomicLoad(&G.firstBadPos) != kInvalid;
  var usable = !bad;
  if ((G.xflags & X_EXACT) == 0u) {
    let inv = 1.0 / G.roi;
    G.originX = lo.x; G.originY = lo.y; G.originZ = lo.z; G.invCell = inv;
    // (The CPU computes these in double precision; f32 can differ by one cell at the far edge,
    // which only shifts where the last grid cell ends.)
    let dims = floor((hi - lo) * inv) + vec3f(1.0);
    if (bad || !(dims.x <= 2097152.0 && dims.y <= 2097152.0 && dims.z <= 2097152.0) || !(dims.x >= 1.0 && dims.y >= 1.0 && dims.z >= 1.0)) {
      if (!bad) { G.gridTooLarge = 1u; }
      G.nx = 1u; G.ny = 1u; G.nz = 1u;
      usable = false;
    } else {
      G.nx = u32(dims.x); G.ny = u32(dims.y); G.nz = u32(dims.z);
    }
  } else {
    // Exact mode: 1 / roi from the host and the dimensions in double precision, as Grid::build
    // (core/grid.cpp) computes them: floor((double(hi) - lo) * invCell) + 1.
    let inv = G.invRoi;
    G.originX = lo.x; G.originY = lo.y; G.originZ = lo.z; G.invCell = inv;
    var fx = 0u; var fy = 0u; var fz = 0u;
    if (!bad) {
      fx = floorU(dmulF(dsubF(hi.x, lo.x), inv)); fy = floorU(dmulF(dsubF(hi.y, lo.y), inv)); fz = floorU(dmulF(dsubF(hi.z, lo.z), inv));
    }
    if (bad || fx >= 2097152u || fy >= 2097152u || fz >= 2097152u) {
      if (!bad) { G.gridTooLarge = 1u; }
      G.nx = 1u; G.ny = 1u; G.nz = 1u;
      usable = false;
    } else {
      G.nx = fx + 1u; G.ny = fy + 1u; G.nz = fz + 1u;
    }
  }

  if ((G.flags & F_LIGHT) != 0u) {
    var nu = 1u; var nv = 1u;
    var u0 = 0.0; var v0 = 0.0;
    var invPixel = G.lightInvPixel0;
    if (usable) {
      // extent of the bounding box in the map plane (from its eight corners)
      var u1 = 0.0; var v1 = 0.0;
      for (var c = 0u; c < 8u; c++) {
        let corner = vec3f(select(lo.x, hi.x, (c & 1u) != 0u), select(lo.y, hi.y, (c & 2u) != 0u), select(lo.z, hi.z, (c & 4u) != 0u));
        let u = dot3(corner, G.lightU.xyz);
        let v = dot3(corner, G.lightV.xyz);
        if (c == 0u) {
          u0 = u; u1 = u; v0 = v; v1 = v;
        } else {
          if (u < u0) { u0 = u; }
          if (u > u1) { u1 = u; }
          if (v < v0) { v0 = v; }
          if (v > v1) { v1 = v; }
        }
      }
      // One texel per lightPixel rest lengths, doubled until the map has at most 2^24 texels:
      // nu = floor((double(u1) - u0) / pixel) + 1 in double precision. Doubling the pixel only
      // lowers the exponent of the quotient.
      let pixel = UD(U64(G.lightPixLo, G.lightPixHi), G.lightPixExp);
      let qu = ddiv(dsubF(u1, u0), pixel);
      let qv = ddiv(dsubF(v1, v0), pixel);
      var doublings = 0;
      for (var i = 0; i < 512; i++) {
        let a = floorU(UD(qu.m, qu.e - doublings));
        let b = floorU(UD(qv.m, qv.e - doublings));
        if (a < 16777216u && b < 16777216u) {
          let texels = mul32(a + 1u, b + 1u);
          if (texels.hi == 0u && texels.lo <= 16777216u) { nu = a + 1u; nv = b + 1u; break; }
        }
        doublings += 1;
      }
      // 1 / pixel: the host's float(1 / pixel0), with the exponent lowered by the doublings
      let bits = bitcast<u32>(G.lightInvPixel0);
      let e = i32(expOf(bits)) - doublings;
      invPixel = 0.0;
      if (e >= 1) { invPixel = bitcast<f32>((bits & 0x807FFFFFu) | (u32(e) << 23u)); }
    }
    G.lightU0 = u0; G.lightV0 = v0; G.lightInvPixel = invPixel; G.lightNu = nu; G.lightNv = nv;
    put(9u, groups(nu * nv, 256u));
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

// The light's shadow map (core/food.hpp): per texel, the depth of the cell nearest to the light,
// as an integer max over order-preserving float bits, so it is exact and independent of timing.
export const shadowClear = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> shadow: array<i32>;
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let t = cellIndex(gid);
  if (t >= G.lightNu * G.lightNv || G.failed != 0u) { return; }
  shadow[t] = -2147483647 - 1;
}
`;

export const shadowBuild = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> shadow: array<atomic<i32>>;
${math}
${index}
${ordered}
${shadowFn}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let P = geomCur[v].xyz;
  atomicMax(&shadow[shadowTexel(P)], orderedInt(shadowDepth(P)));
}
`;

// The food stage with any source beyond uniform random on (food_update in core/food.hpp): the
// uniform random increment scaled by the pulse, the field, the light, the crowding and the
// morphogen activator, then diffusion along the links and decay. Reads the old food and
// positions and this step's normals; writes the food part of the new buffer.
export const food = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(3) var<storage, read> nrmNew: array<vec4f>;
@group(0) @binding(4) var<storage, read> topo: array<u32>;
@group(0) @binding(5) var<storage, read> shadow: array<i32>;
@group(0) @binding(6) var<storage, read> morphCur: array<vec2f>;
${math}
${hash}
${u64x}
${exactG}
${noise}
${field}
${index}
${ordered}
${shadowFn}

// Scale from the light: ambient for a cell in shadow or facing away, ambient + (1 - ambient) N.L
// for a lit one.
fn lightFactor(P: vec3f, N: vec3f) -> f32 {
  let nearest = fromOrdered(shadow[shadowTexel(P)]);
  let lit = shadowDepth(P) >= nearest - G.lightBias;
  let facing = dot3(N, G.lightDir.xyz);
  var received = G.lightAmbient;
  if (lit && facing > 0.0) { received = G.lightAmbient + (1.0 - G.lightAmbient) * facing; }
  return 1.0 - G.lightAmount * (1.0 - received);
}
// 0 where the cell's links are compressed to crowdLow rest lengths or less, 1 at crowdHigh.
fn crowdFactor(v: u32, P: vec3f) -> f32 {
  let d = topo[v * 16u + T_DEG];
  var sum = 0.0;
  for (var k = 0u; k < d; k++) {
    let e = geomCur[topo[v * 16u + T_NBR + k]].xyz - P;
    sum += fsqrtx(dot3(e, e));
  }
  let mean = fdivx(sum, f32(d)) * G.invL0;
  return clamp01((mean - G.crowdLow) * G.crowdInvRange);
}

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u || G.flags == 0u) { return; }
  let cur = geomCur[v];
  let P = cur.xyz;
  let base = fadd(G.foodMin, fmul(G.foodSpan, hashU01(kStreamFood, v)));
  var factor = G.pulse;
  if ((G.flags & F_FIELD) != 0u) { factor = fmul(factor, fadd(1.0, fmul(G.fieldAmount, 2.0 * fieldValue(P) - 1.0))); }
  if ((G.flags & F_LIGHT) != 0u) { factor = fmul(factor, lightFactor(P, nrmNew[v].xyz)); }
  if ((G.flags & F_CROWD) != 0u) { factor = fmul(factor, crowdFactor(v, P)); }
  if ((G.flags & F_MORPH) != 0u) { factor = fmul(factor, morphFactor(G.morphFood, morphCur[v].y)); }
  var f = fadd(cur.w, fmul(base, factor));
  if ((G.flags & F_DIFFUSE) != 0u) {
    let d = topo[v * 16u + T_DEG];
    var sum = 0.0;
    for (var k = 0u; k < d; k++) { sum = fadd(sum, geomCur[topo[v * 16u + T_NBR + k]].w); }
    f = fadd(f, fmul(G.diffusion, fadd(fdivx(sum, f32(d)), fneg(cur.w))));
    f = fmul(f, G.keep);
  }
  geomNew[v] = vec4f(geomNew[v].xyz, f);
}
`;

// Steps 4 and 5: move (reads the old positions, writes the new buffer) and clamp. Invocations
// walk the cells in grid order so neighbours share the buckets they search.
// exact: division and square roots in exact arithmetic, so the new position is the CPU's bit
// for bit; otherwise the GPU's own, as the port always had them.
export const move = (exact) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(3) var<storage, read> nrmNew: array<vec4f>;
@group(0) @binding(4) var<storage, read> topo: array<u32>;
@group(0) @binding(5) var<storage, read> scanData: array<u32>;
@group(0) @binding(6) var<storage, read> sortedPos: array<vec4f>;
@group(0) @binding(7) var<storage, read> morphCur: array<vec2f>;
${math}
${mathG}
${u64}
${u64x}
${exactG}
${noise}
${field}
${index}
${exact ? `
fn unitExact(v: vec3f) -> vec3f {
  let l2 = dot3(v, v);
  if (l2 < G.unitEps2) { return vec3f(0.0); }
  let inv = fdivx(1.0, fsqrtx(l2));
  return vec3f(v.x * inv, v.y * inv, v.z * inv);
}` : ''}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let q = cellIndex(gid);
  if (q >= G.n || G.failed != 0u) { return; }
  let v = bitcast<u32>(sortedPos[q].w);
  let P = geomCur[v].xyz;
  let d = topo[v * 16u + T_DEG];
  let base = v * 16u + T_NBR;
  let N = nrmNew[v].xyz;

  // The cell's own constants: the field scales its bulge factor, the morphogen activator its
  // bulge factor, planar factor and rest length (core/sim_cpu.cpp, morph_apply).
  var l0 = G.l0; var planarFactor = G.planar; var bulgeFactor = G.bulge;
  if ((G.xflags & X_FIELD_BULGE) != 0u) { bulgeFactor = G.bulge * (1.0 + G.fieldBulge * (2.0 * fieldValue(P) - 1.0)); }
  if ((G.xflags & X_MORPH_ON) != 0u) {
    let b = morphCur[v].y;
    bulgeFactor = bulgeFactor * morphFactor(G.morphBulge, b);
    planarFactor = planarFactor * morphFactor(G.morphPlanar, b);
    l0 = l0 * morphFactor(G.morphRest, b);
  }

  // link terms
  var springSum = vec3f(0.0);
  var planarSum = vec3f(0.0);
  var bulgeSum = 0.0;
  let l0sq = l0 * l0;
  for (var k = 0u; k < d; k++) {
    let L = geomCur[topo[base + k]].xyz;
    let toP = P - L;
    springSum += L + l0 * ${exact ? 'unitExact' : 'unitOrZero'}(toP);
    planarSum += L;
    let dotN = -dot3(toP, N);
    let arg = max(l0sq - dot3(toP, toP) + dotN * dotN, 0.0);   // over-stretched link: clamp
    bulgeSum += ${exact ? 'fsqrtx' : 'sqrt'}(arg) + dotN;
  }
  let inv = ${exact ? 'recip(d)' : '1.0 / f32(d)'};
  let spring = G.spring * (springSum * inv - P);
  let planar = planarFactor * (planarSum * inv - P);
  let bulge = (bulgeFactor * (bulgeSum * inv)) * N;

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
          ${exact ? 'sum += fdivx(roi2 - d2, roi2) * unitExact(diff);' : 'sum += ((roi2 - d2) / roi2) * unitOrZero(diff);'}
        }
      }
    }
  }
  let rep = G.repulsion * sum;

  var delta = ((spring + planar) + bulge) + rep;
  if (G.maxMove > 0.0) {
    let l2 = dot3(delta, delta);
    if (l2 > G.maxMove * G.maxMove) {
      ${exact ? 'delta = delta * fdivx(G.maxMove, fsqrtx(l2));' : 'delta = delta * (G.maxMove / sqrt(l2));'}
      atomicAdd(&G.clamped, 1u);
    }
  }
  let Pn = P + delta;
  geomNew[v] = vec4f(Pn, geomNew[v].w);   // keep the food written earlier in the step
  if (!finite3(Pn)) { atomicMin(&G.firstBadMove, v); }
}
`;

// Morphogens (core/morph.hpp): two concentrations per cell, (A substrate, B activator).
// Starting concentrations when morphogens are switched on (morph_initial): the activator starts
// in the patches where a smooth random field in space is above the seed level.
export const morphInit = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomCur: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> morphCur: array<vec2f>;
${hash}
${noise}
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n) { return; }
  // mix64(seed ^ (0x4D4F5250 + step))
  let seed = mix64(xor64(U64(G.seedLo, G.seedHi), add64(U64(0x4D4F5250u, 0u), U64(G.stepLo, G.stepHi))));
  let fieldHere = valueNoise3(geomCur[v].xyz * G.morphSeedInvScale, seed);
  var ab = vec2f(1.0, 0.0);
  if (fieldHere > G.morphSeedLevel) { ab = vec2f(0.5, 0.25 + 0.1 * hashU01(kStreamMorph, v)); }
  morphCur[v] = ab;
}
`;

// One reaction-diffusion iteration (morph_update; Jacobi: reads one buffer, writes another).
// With the morphogens switched off but present, it only carries the concentrations over.
export const morph = /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> topo: array<u32>;
@group(0) @binding(2) var<storage, read> morphSrc: array<vec2f>;
@group(0) @binding(3) var<storage, read_write> morphDst: array<vec2f>;
${u64}
${u64x}
${exactG}
${index}
@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let v = cellIndex(gid);
  if (v >= G.n || G.failed != 0u) { return; }
  let own = morphSrc[v];
  if ((G.xflags & X_MORPH_ON) == 0u) { morphDst[v] = own; return; }
  let d = topo[v * 16u + T_DEG];
  var sumA = 0.0; var sumB = 0.0;
  for (var k = 0u; k < d; k++) {
    let other = morphSrc[topo[v * 16u + T_NBR + k]];
    sumA = fadd(sumA, other.x);
    sumB = fadd(sumB, other.y);
  }
  let inv = recip(d);
  let a = own.x; let b = own.y;
  let react = fmul(fmul(a, b), b);
  let na = fadd(fadd(fadd(a, fmul(G.morphDiffA, fadd(fmul(sumA, inv), fneg(a)))), fneg(react)), fmul(G.morphFeed, fadd(1.0, fneg(a))));
  let nb = fadd(fadd(fadd(b, fmul(G.morphDiffB, fadd(fmul(sumB, inv), fneg(b)))), react), fneg(fmul(G.morphKillFeed, b)));
  morphDst[v] = vec2f(clamp01(na), clamp01(nb));
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
// exact: the lengths of the cleavage links in exact arithmetic (the tie-break between pairs).
// The alignment of the oriented division is always computed exactly.
export const candidates = (exact) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read> geomNew: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> topo: array<u32>;
@group(0) @binding(3) var<storage, read_write> sel: array<u32>;   // 6 per cell: prio, m1, m2 as (lo, hi)
@group(0) @binding(4) var<storage, read> nrmNew: array<vec4f>;
${math}
${hash}
${u64x}
${exactG}
${index}
${division}
fn cleavageLength(v: u32, i: u32, j: u32, P: vec3f) -> f32 {
  ${exact ? `let a = geomNew[ringAt(v, i)].xyz - P; let b = geomNew[ringAt(v, j)].xyz - P;
  return fsqrtx(dot3(a, a)) + fsqrtx(dot3(b, b));`
    : 'return length3(geomNew[ringAt(v, i)].xyz - P) + length3(geomNew[ringAt(v, j)].xyz - P);'}
}
// Oriented division: how well the new parent-daughter link (in the surface, across the line
// a-b) lies along the axis.
fn alignment(v: u32, i: u32, j: u32, N: vec3f) -> f32 {
  let c = cross3(N, geomNew[ringAt(v, j)].xyz - geomNew[ringAt(v, i)].xyz);
  let l2 = dot3(c, c);
  var u = vec3f(0.0);
  if (!(l2 < G.unitEps2)) {
    let inv = fdivx(1.0, fsqrtx(l2));
    u = vec3f(c.x * inv, c.y * inv, c.z * inv);
  }
  return abs(dot3(u, G.orientAxis.xyz));
}
// Packed (i << 4) | j, or kNoPair when no pair respects the degree cap.
fn chooseCleavagePair(v: u32, tie: U64) -> u32 {
  let d = degOf(v);
  let offs0 = d / 2u; let offs1 = (d + 1u) / 2u;
  let nOffs = select(2u, 1u, offs0 == offs1);
  // every near-opposite pair inside the degree cap, with its degree cost
  var allI: array<u32, 24>;
  var allJ: array<u32, 24>;
  var allCost: array<u32, 24>;
  var nAll = 0u;
  var best = 0xFFFFFFFFu;
  for (var i = 0u; i < d; i++) {
    for (var o = 0u; o < nOffs; o++) {
      let j = (i + select(offs1, offs0, o == 0u)) % d;
      if (!canSplit(v, i, j)) { continue; }
      let da = degOf(ringAt(v, i)); let db = degOf(ringAt(v, j));
      let cost = max(da, db) * 100u + da + db;
      allI[nAll] = i; allJ[nAll] = j; allCost[nAll] = cost; nAll++;
      if (cost < best) { best = cost; }
    }
  }
  if (nAll == 0u) { return kNoPair; }
  // the pairs to choose among: the best degree cost, or (oriented division) close to it
  let oriented = (G.xflags & X_ORIENT) != 0u;
  let margin = select(0u, G.orientMargin, oriented);
  var tiesI: array<u32, 24>;
  var tiesJ: array<u32, 24>;
  var nTies = 0u;
  for (var k = 0u; k < nAll; k++) {
    if (allCost[k] <= best + margin) { tiesI[nTies] = allI[k]; tiesJ[nTies] = allJ[k]; nTies++; }
  }
  // tie % nTies for a 64-bit tie value
  let pow32 = ((0xFFFFFFFFu % nTies) + 1u) % nTies;
  var pick = ((tie.hi % nTies) * pow32 + tie.lo % nTies) % nTies;
  if (oriented && nTies > 1u) {
    // the pair whose new link is most aligned with the axis, walking from the hashed choice
    let N = nrmNew[v].xyz;
    let start = pick;
    var bestAlign = alignment(v, tiesI[pick], tiesJ[pick], N);
    for (var k = 1u; k < nTies; k++) {
      let q = (start + k) % nTies;
      let al = alignment(v, tiesI[q], tiesJ[q], N);
      if (al > bestAlign) { bestAlign = al; pick = q; }
    }
  } else if (nTies > 1u) {
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

// Step 7: every winner splits, unless the step failed or would exceed the capacity. A daughter
// starts with its parent's morphogen concentrations.
// exact: the daughter's direction in exact arithmetic.
export const split = (exact) => /* wgsl */ `
${globals}
@group(0) @binding(0) var<storage, read_write> G: Globals;
@group(0) @binding(1) var<storage, read_write> geomNew: array<vec4f>;
@group(0) @binding(2) var<storage, read_write> nrmNew: array<vec4f>;
@group(0) @binding(3) var<storage, read_write> topo: array<u32>;
@group(0) @binding(4) var<storage, read> sel: array<u32>;
@group(0) @binding(5) var<storage, read> scanData: array<u32>;
@group(0) @binding(6) var<storage, read_write> morphNew: array<vec2f>;
${math}
${exact ? u64 + u64x + exactG : ''}
${index}
${division}
fn unitOrZero(v: vec3f) -> vec3f {
  let l2 = dot3(v, v);
  if (l2 < G.unitEps2) { return vec3f(0.0); }
  let inv = ${exact ? 'fdivx(1.0, fsqrtx(l2))' : '1.0 / sqrt(l2)'};
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
    dir = unitOrZero(c * ${exact ? 'recip(moved)' : '(1.0 / f32(moved))'} - P);
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
  if ((G.xflags & X_MORPH_PRESENT) != 0u) { morphNew[vp] = morphNew[v]; }
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

// Self-test of the exact arithmetic (tasks.js, ?task=floats): per item 8 u32 in
// (a, b, c as float bits; the mantissa and exponent of a double; two words that are 0), 16 u32
// out: eight results of the exact operations, the GPU's own a * b + c, a / b, sqrt and a * b
// for the record, then a - b, clamp01(a) and -a of the exact operations.
export const floatTest = /* wgsl */ `
@group(0) @binding(0) var<storage, read> inp: array<u32>;
@group(0) @binding(1) var<storage, read_write> outp: array<u32>;
${u64}
${u64x}
${exactf}
${softd}
fn opaqueZero() -> u32 { return inp[7]; }
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3<u32>) {
  let i = gid.x;
  if (i * 8u + 7u >= arrayLength(&inp)) { return; }
  let a = bitcast<f32>(inp[i * 8u]); let b = bitcast<f32>(inp[i * 8u + 1u]); let c = bitcast<f32>(inp[i * 8u + 2u]);
  let dbl = UD(U64(inp[i * 8u + 3u], inp[i * 8u + 4u]), bitcast<i32>(inp[i * 8u + 5u]));
  let o = i * 16u;
  let pa = bitcast<f32>(inp[i * 8u] & 0x7FFFFFFFu);   // |a|
  outp[o] = bitcast<u32>(fdivx(a, b));
  outp[o + 1u] = bitcast<u32>(fsqrtx(pa));
  outp[o + 2u] = bitcast<u32>(sfmul(a, b));
  outp[o + 3u] = bitcast<u32>(sfadd(a, b));
  outp[o + 4u] = bitcast<u32>(fmul(a, b));
  outp[o + 5u] = bitcast<u32>(fadd(a, b));
  var hi = a; var lo = b;
  if (lo > hi) { hi = b; lo = a; }
  outp[o + 6u] = floorU(ddiv(dsubF(hi, lo), dbl));
  outp[o + 7u] = floorU(dmulF(dsubF(hi, lo), abs(c)));
  outp[o + 8u] = bitcast<u32>(a * b + c);
  outp[o + 9u] = bitcast<u32>(a / b);
  outp[o + 10u] = bitcast<u32>(sqrt(pa));
  outp[o + 11u] = bitcast<u32>(a * b);
  outp[o + 12u] = bitcast<u32>(fadd(a, fneg(b)));
  outp[o + 13u] = bitcast<u32>(clamp01(a));
  outp[o + 14u] = bitcast<u32>(fneg(a));
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
