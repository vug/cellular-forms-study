// WGSL of the web viewer: a port of the raster path of the desktop viewer (shaders/*.frag and
// *.vert): cells as sphere impostors or the membrane into a G-buffer, a shadow map of the key
// light, screen-space ambient occlusion, lighting, the soft tone curve, and a last pass that
// smooths edges and scales the picture to the canvas.
//
// Everything is read from the simulation's buffers in the vertex shaders (no vertex buffers):
//   geom  vec4f per cell: position, food
//   nrm   vec4f per cell: unit normal of the sheet
//   topo  16 u32 per cell: neighbour count, birth step, generation, flags, 12 neighbour ids
//   morph vec2f per cell: the two morphogens (substrate, activator), when the model has them
// One instance is drawn per cell. How many there are is taken on the GPU, at the moment of the
// draw, from the simulation's own counter (an indirect draw, see renderer.js): the count the
// page knows lags behind the steps it has submitted.

// The per-frame uniforms: only mat4x4f and vec4f, so the offsets are plain to compute.
export const FRAME = [
  ['view', 16],          // world -> view (camera looks down -Z, +Y up)
  ['proj', 16],          // view -> clip
  ['invView', 16],       // view -> world
  ['lightViewProj', 16], // world -> clip space of the shadow map
  ['viewport', 4],       // width, height, 1/width, 1/height of the images being rendered
  ['sphere', 4],         // cell radius (world), smallest radius (pixels; shadow pass: world), unused, unused
  ['color', 4],          // colour mode (a cell property, 0: the material's colour), unused, unused, current step
  ['light', 4],          // view-space direction towards the key light, share of the light from around
  ['lightColor', 4],     // key light colour times intensity, shadow strength (0: no shadow)
  ['sky', 4],            // light from above
  ['ground', 4],         // light from below
  ['shadow', 4],         // normal offset (world), softness (texels), 1 / map size, depth bias (clip units)
  ['shadow2', 4],        // depth of the light's box (world), 1 when the map was drawn, map size, unused
  ['ao', 4],             // small radius (world), large radius (world), strength, enabled
  ['baseColor', 4],      // the material's colour, exposure
  ['background', 4],     // colour at the top, vignette
  ['palette', 4],        // palette, reversed, background gradient, unused
  ['matBase', 4],        // roughness, metal, highlight, sheen
  ['matSss', 4],         // subsurface, scatter radius (world), light through, depth it gets through (world)
  ['matSssColor', 4],    // how far red, green, blue travel
  ['matEmission', 4],    // glow colour, strength
  ['matEmission2', 4],   // share of the glow colour against the cell colour, glow lights the neighbours, unused, unused
  ['modSource', 4],      // the cell property that drives glow, roughness, subsurface, light through (0: none)
  ['modLo', 4],
  ['modHi', 4],
  ['modFrom', 4],
  ['modTo', 4],
  ['propMin0', 4],       // ranges of the cell properties: age, food, birth step, generation
  ['propMin1', 4],       // ... curvature, activator
  ['propMax0', 4],
  ['propMax1', 4],
  ['wall', 4],           // cell wall colour, strength (0: off)
  ['style', 4],          // wall width (pixels), 1 when there are morphogens, link rest length, 1 when curvature is needed
  ['env', 4],            // studio surroundings on, intensity, unused, unused
  ['misc', 4],           // smooth edges (FXAA) on, unused, unused, unused
];
export const FRAME_FLOATS = FRAME.reduce((n, [, size]) => n + size, 0);
export const FRAME_OFFSET = (() => {
  const out = {};
  let at = 0;
  for (const [name, size] of FRAME) {
    out[name] = at;
    at += size;
  }
  return out;
})();

const frame = /* wgsl */ `
struct Frame {
${FRAME.map(([name, size]) => `  ${name}: ${size === 16 ? 'mat4x4f' : 'vec4f'},`).join('\n')}
}
@group(0) @binding(0) var<uniform> F: Frame;
`;

const cellBuffers = /* wgsl */ `
@group(1) @binding(0) var<storage, read> geom: array<vec4f>;
@group(1) @binding(1) var<storage, read> nrm: array<vec4f>;
@group(1) @binding(2) var<storage, read> topo: array<u32>;
@group(1) @binding(3) var<storage, read> morph: array<vec2f>;
`;

// Cell properties, colours and materials (shaders/palette.glsl).
const palette = /* wgsl */ `
// The raw value of a property: 1 age, 2 food, 3 birth step, 4 generation, 5 curvature, 6 activator.
fn cellRaw(which: i32, birth: f32, food: f32, extra: vec3f) -> f32 {
  if (which == 1) { return F.color.w - birth; }
  if (which == 2) { return food; }
  if (which == 3) { return birth; }
  if (which == 4) { return extra.x; }
  if (which == 5) { return extra.y; }
  if (which == 6) { return extra.z; }
  return 0.0;
}
// A property as 0..1 over its usual range (for age: 1 is the youngest).
fn cellProperty(which: i32, birth: f32, food: f32, extra: vec3f) -> f32 {
  if (which < 1 || which > 6) { return 0.0; }
  var lo: f32; var hi: f32;
  if (which <= 4) { lo = F.propMin0[which - 1]; hi = F.propMax0[which - 1]; }
  else { lo = F.propMin1[which - 5]; hi = F.propMax1[which - 5]; }
  let s = clamp((cellRaw(which, birth, food, extra) - lo) / max(hi - lo, 1e-6), 0.0, 1.0);
  return select(s, 1.0 - s, which == 1);
}
// The 0..1 value a cell is coloured by.
fn cellScalar(birth: f32, food: f32, extra: vec3f) -> f32 { return cellProperty(i32(F.color.x + 0.5), birth, food, extra); }
// What varies from cell to cell in the material, each 0..1: how much of the emission the cell
// gives off, its roughness, how much it scatters light under the surface, how much it lets through.
fn cellMaterial(birth: f32, food: f32, extra: vec3f) -> vec4f {
  var m = vec4f(1.0, F.matBase.x, F.matSss.x, F.matSss.z);
  for (var i = 0; i < 4; i++) {
    let source = i32(F.modSource[i] + 0.5);
    if (source == 0) { continue; }
    let t = cellProperty(source, birth, food, extra);
    m[i] = mix(F.modLo[i], F.modHi[i], clamp((t - F.modFrom[i]) / max(F.modTo[i] - F.modFrom[i], 1e-4), 0.0, 1.0));
  }
  return clamp(m, vec4f(0.0), vec4f(1.0));
}
fn ramp5(c0: vec3f, c1: vec3f, c2: vec3f, c3: vec3f, c4: vec3f, x: f32) -> vec3f {
  let t = clamp(x, 0.0, 1.0) * 4.0;
  if (t < 1.0) { return mix(c0, c1, t); }
  if (t < 2.0) { return mix(c1, c2, t - 1.0); }
  if (t < 3.0) { return mix(c2, c3, t - 2.0); }
  return mix(c3, c4, t - 3.0);
}
fn paletteColor(x: f32) -> vec3f {
  let t = select(x, 1.0 - x, F.palette.y > 0.5);
  let p = i32(F.palette.x + 0.5);
  if (p == 1) { return ramp5(vec3f(0.27, 0.00, 0.33), vec3f(0.23, 0.32, 0.55), vec3f(0.13, 0.57, 0.55), vec3f(0.37, 0.79, 0.38), vec3f(0.99, 0.91, 0.14), t); }
  if (p == 2) { return ramp5(vec3f(0.00, 0.00, 0.02), vec3f(0.32, 0.07, 0.48), vec3f(0.72, 0.21, 0.47), vec3f(0.99, 0.53, 0.38), vec3f(0.99, 0.99, 0.75), t); }
  if (p == 3) { return ramp5(vec3f(0.13, 0.27, 0.62), vec3f(0.55, 0.72, 0.88), vec3f(0.96, 0.96, 0.94), vec3f(0.93, 0.62, 0.50), vec3f(0.70, 0.09, 0.17), t); }
  if (p == 4) { return mix(vec3f(0.08), vec3f(0.98), clamp(t, 0.0, 1.0)); }
  return ramp5(vec3f(0.10, 0.13, 0.32), vec3f(0.10, 0.45, 0.55), vec3f(0.55, 0.75, 0.40), vec3f(0.98, 0.84, 0.35), vec3f(1.00, 0.96, 0.86), t);
}
fn cellAlbedo(scalar: f32) -> vec3f {
  if (i32(F.color.x + 0.5) == 0) { return F.baseColor.rgb; }
  return paletteColor(scalar);
}
// The light a cell gives off: its own colour or the glow colour, times the strength.
fn cellEmission(albedo: vec3f, amount: f32) -> vec3f { return mix(albedo, F.matEmission.rgb, F.matEmission2.x) * (F.matEmission.a * amount); }
`;

// Reading one cell in a vertex shader.
const cellRead = /* wgsl */ `
const T = 16u;  // u32 per topology record
// Generation, curvature (mean over the ring of 2 N.(P - Q) / |P - Q|^2, per rest length:
// cell_curvature of core/kernels.hpp) and morphogen activator (as the signal min(3 B, 1)).
fn cellExtra(i: u32, p: vec3f) -> vec3f {
  var curvature = 0.0;
  if (F.style.w > 0.5) {
    let d = min(topo[i * T], 12u);
    let n = nrm[i].xyz;
    for (var q = 0u; q < d; q++) {
      let e = p - geom[topo[i * T + 4u + q]].xyz;
      let l2 = dot(e, e);
      if (l2 > 0.0) { curvature += 2.0 * dot(n, e) / l2; }
    }
    if (d > 0u) { curvature = curvature / f32(d) * F.style.z; }
  }
  var activator = 0.0;
  if (F.style.y > 0.5) { activator = min(3.0 * morph[i].y, 1.0); }
  return vec3f(f32(topo[i * T + 2u]), curvature, activator);
}
`;

const gbufferOut = /* wgsl */ `
struct GOut {
  @location(0) albedo: vec4f,    // colour, a: how much of the pixel is cell wall (membrane)
  @location(1) normal: vec4f,    // view-space normal
  @location(2) material: vec4f,  // emission, roughness, subsurface, light through
  @location(3) depth: f32,       // linear view depth (> 0; 0 is the background)
}
`;

// ---------------------------------------------------------------- cells as sphere impostors
export const cells = /* wgsl */ `
${frame}
${cellBuffers}
${palette}
${cellRead}
${gbufferOut}

struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) viewPos: vec3f,                        // on the quad
  @location(1) @interpolate(flat) centre: vec3f,      // of the sphere, view space
  @location(2) @interpolate(flat) pixelRadius: f32,
  @location(3) @interpolate(flat) scalar: f32,
  @location(4) @interpolate(flat) material: vec4f,
}

@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) cell: u32) -> VOut {
  var o: VOut;
  let corner = vec2f(f32(vi & 1u), f32((vi >> 1u) & 1u)) * 2.0 - 1.0;
  let g = geom[cell];
  let c = (F.view * vec4f(g.xyz, 1.0)).xyz;
  let r = F.sphere.x;
  let dist = max(-c.z, 1e-6);
  let pixR = r * F.proj[1][1] * 0.5 * F.viewport.y / dist;
  // The silhouette of a sphere under perspective is a little larger than a billboard through
  // its centre; and no cell falls below the smallest size (it would drop out between pixels).
  let d2 = dist * dist;
  let widen = dist / sqrt(max(d2 - r * r, 1e-4 * d2));
  var half = r * widen * 1.08;
  if (pixR < F.sphere.y) { half = r * F.sphere.y / max(pixR, 1e-6); }
  let p = c + vec3f(corner * half, 0.0);
  o.viewPos = p;
  o.centre = c;
  o.pixelRadius = pixR;
  let birth = f32(topo[cell * T + 1u]);
  let extra = cellExtra(cell, g.xyz);
  o.scalar = cellScalar(birth, g.w, extra);
  o.material = cellMaterial(birth, g.w, extra);
  o.pos = F.proj * vec4f(p, 1.0);
  return o;
}

struct FOut {
  @location(0) albedo: vec4f,
  @location(1) normal: vec4f,
  @location(2) material: vec4f,
  @location(3) depth: f32,
  @builtin(frag_depth) z: f32,
}

@fragment
fn fs(i: VOut) -> FOut {
  var hit: vec3f;
  var normal: vec3f;
  if (i.pixelRadius < 0.75) {
    // Smaller than a pixel: a point that faces the camera.
    hit = i.centre;
    normal = normalize(-i.centre);
  } else {
    let r = F.sphere.x;
    let rd = normalize(i.viewPos);
    let b = dot(rd, i.centre);
    let disc = b * b - (dot(i.centre, i.centre) - r * r);
    if (disc < 0.0) { discard; }
    let t = b - sqrt(disc);
    if (t <= 0.0) { discard; }
    hit = rd * t;
    normal = (hit - i.centre) / r;
  }
  let clip = F.proj * vec4f(hit, 1.0);
  var o: FOut;
  o.z = clip.z / clip.w;
  o.albedo = vec4f(cellAlbedo(i.scalar), 0.0);
  o.normal = vec4f(normal, 1.0);
  o.material = i.material;
  o.depth = -hit.z;
  return o;
}
`;

// ---------------------------------------------------------------- the membrane
// Per cell and neighbour slot one triangle (the cell and two consecutive neighbours), drawn
// only from its smallest cell, so every triangle of the sheet is drawn once.
const meshTriangle = /* wgsl */ `
struct Tri { ok: bool, cells: vec3u }
fn triangleOf(vi: u32, cell: u32) -> Tri {
  var t: Tri;
  t.ok = false;
  let k = vi / 3u;
  let deg = topo[cell * T];
  if (k >= deg || deg > 12u) { return t; }
  let a = topo[cell * T + 4u + k];
  let b = topo[cell * T + 4u + select(k + 1u, 0u, k + 1u == deg)];
  if (a <= cell || b <= cell) { return t; }
  t.ok = true;
  t.cells = vec3u(cell, a, b);
  return t;
}
`;

export const mesh = /* wgsl */ `
${frame}
${cellBuffers}
${palette}
${cellRead}
${gbufferOut}
${meshTriangle}

struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) viewPos: vec3f,
  @location(1) normal: vec3f,
  @location(2) scalar: f32,
  @location(3) material: vec4f,
  @location(4) @interpolate(flat) site0: vec3f,  // view-space positions of the three cells
  @location(5) @interpolate(flat) site1: vec3f,
  @location(6) @interpolate(flat) site2: vec3f,
}

@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) cell: u32) -> VOut {
  var o: VOut;
  o.pos = vec4f(2.0, 2.0, 2.0, 1.0);
  let t = triangleOf(vi, cell);
  if (!t.ok) { return o; }
  let c = t.cells[vi % 3u];
  let g = geom[c];
  let p = F.view * vec4f(g.xyz, 1.0);
  o.viewPos = p.xyz;
  o.normal = (F.view * vec4f(nrm[c].xyz, 0.0)).xyz;
  let birth = f32(topo[c * T + 1u]);
  let extra = cellExtra(c, g.xyz);
  o.scalar = cellScalar(birth, g.w, extra);
  o.material = cellMaterial(birth, g.w, extra);
  if (F.wall.a > 0.0) {
    o.site0 = (F.view * vec4f(geom[t.cells.x].xyz, 1.0)).xyz;
    o.site1 = (F.view * vec4f(geom[t.cells.y].xyz, 1.0)).xyz;
    o.site2 = (F.view * vec4f(geom[t.cells.z].xyz, 1.0)).xyz;
  }
  o.pos = F.proj * p;
  return o;
}

@fragment
fn fs(i: VOut) -> GOut {
  var n = i.normal;
  let l = length(n);
  if (l > 1e-6) { n = n / l; } else { n = vec3f(0.0, 0.0, 1.0); }
  if (dot(n, i.viewPos) > 0.0) { n = -n; }  // the inside of a fold is shaded too
  var wall = 0.0;
  if (F.wall.a > 0.0) {
    // The fragment belongs to the nearest of its three cells; its distance to the plane
    // halfway to the next one is what the walls are drawn from (shaders/mesh.frag).
    let d2 = vec3f(dot(i.viewPos - i.site0, i.viewPos - i.site0), dot(i.viewPos - i.site1, i.viewPos - i.site1),
                   dot(i.viewPos - i.site2, i.viewPos - i.site2));
    var sk = i.site0; var si = i.site1; var sj = i.site2;
    var dk = d2.x; var di = d2.y; var dj = d2.z;
    if (d2.y <= d2.x && d2.y <= d2.z) { sk = i.site1; si = i.site2; sj = i.site0; dk = d2.y; di = d2.z; dj = d2.x; }
    else if (d2.z <= d2.x && d2.z <= d2.y) { sk = i.site2; si = i.site0; sj = i.site1; dk = d2.z; di = d2.x; dj = d2.y; }
    let toWall = min((di - dk) / (2.0 * max(distance(si, sk), 1e-6)), (dj - dk) / (2.0 * max(distance(sj, sk), 1e-6)));
    let worldPerPixel = -i.viewPos.z / (0.5 * F.viewport.y * F.proj[1][1]);
    wall = clamp(F.style.x - toWall / worldPerPixel + 0.5, 0.0, 1.0);
  }
  var o: GOut;
  o.albedo = vec4f(cellAlbedo(i.scalar), wall);
  o.normal = vec4f(n, 1.0);
  o.material = i.material;
  o.depth = -i.viewPos.z;
  return o;
}
`;

// ---------------------------------------------------------------- shadow map
// The same two, seen from the key light (F.view and F.proj are those of the light here, an
// orthographic box): only the depth is kept.
export const shadowCells = /* wgsl */ `
${frame}
${cellBuffers}
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) offset: vec2f,
  @location(1) @interpolate(flat) centre: vec3f,
  @location(2) @interpolate(flat) radius: f32,
}
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) cell: u32) -> VOut {
  var o: VOut;
  let corner = vec2f(f32(vi & 1u), f32((vi >> 1u) & 1u)) * 2.0 - 1.0;
  let c = (F.view * vec4f(geom[cell].xyz, 1.0)).xyz;
  // Never smaller than about a texel of the map, so no cell is lost.
  let r = max(F.sphere.x, F.sphere.y);
  o.offset = corner;
  o.centre = c;
  o.radius = r;
  o.pos = F.proj * vec4f(c + vec3f(corner * r, 0.0), 1.0);
  return o;
}
@fragment
fn fs(i: VOut) -> @builtin(frag_depth) f32 {
  let d2 = dot(i.offset, i.offset);
  if (d2 > 1.0) { discard; }
  // The sphere bulges towards the light, in front of the quad through its centre.
  let z = i.centre.z + F.sphere.x * sqrt(1.0 - d2);
  let clip = F.proj * vec4f(i.centre.xy + i.offset * i.radius, z, 1.0);
  return clip.z / clip.w;
}
`;

export const shadowMesh = /* wgsl */ `
${frame}
${cellBuffers}
const T = 16u;
${meshTriangle}
@vertex
fn vs(@builtin(vertex_index) vi: u32, @builtin(instance_index) cell: u32) -> @builtin(position) vec4f {
  let t = triangleOf(vi, cell);
  if (!t.ok) { return vec4f(2.0, 2.0, 2.0, 1.0); }
  return F.proj * F.view * vec4f(geom[t.cells[vi % 3u]].xyz, 1.0);
}
`;

// ---------------------------------------------------------------- full-screen passes
const screen = /* wgsl */ `
struct FullOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f }
@vertex
fn vsFull(@builtin(vertex_index) vi: u32) -> FullOut {
  let p = vec2f(f32((vi << 1u) & 2u), f32(vi & 2u));
  var o: FullOut;
  o.pos = vec4f(p * 2.0 - 1.0, 0.0, 1.0);
  o.uv = vec2f(p.x, 1.0 - p.y);
  return o;
}
fn hash12(p: vec2f) -> f32 {
  var p3 = fract(vec3f(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
// Direction of the view ray through uv (0, 0 is the top left), scaled so that z is -1.
fn viewRay(uv: vec2f) -> vec3f { return vec3f((uv.x * 2.0 - 1.0) / F.proj[0][0], (1.0 - uv.y * 2.0) / F.proj[1][1], -1.0); }
fn viewPos(uv: vec2f, depth: f32) -> vec3f { return viewRay(uv) * depth; }
// Where a view-space point lands on the screen.
fn screenOf(p: vec3f) -> vec2f {
  let clip = F.proj * vec4f(p, 1.0);
  return vec2f(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
}
fn texel(uv: vec2f) -> vec2i { return clamp(vec2i(uv * F.viewport.xy), vec2i(0), vec2i(F.viewport.xy) - 1); }
`;

// Screen-space ambient occlusion at two scales (shaders/ssao.frag): a small radius for the
// contact between neighbouring cells and a large one for the insides of folds. a = how open the
// point is; rgb = the light of glowing cells that the same samples land on. The sample pattern
// repeats every 4 x 4 pixels and the next pass averages over just that.
export const ssao = /* wgsl */ `
${frame}
${palette}
${screen}
@group(1) @binding(0) var texDepth: texture_2d<f32>;
@group(1) @binding(1) var texNormal: texture_2d<f32>;
@group(1) @binding(2) var texAlbedo: texture_2d<f32>;
@group(1) @binding(3) var texMaterial: texture_2d<f32>;

const kSamples = 10;  // per scale

struct Gathered { occlusion: f32, glow: vec3f }

fn occlusionAt(p: vec3f, n: vec3f, t: vec3f, b: vec3f, radius: f32, seed: f32, gather: bool) -> Gathered {
  var res: Gathered;
  res.occlusion = 0.0;
  res.glow = vec3f(0.0);
  for (var i = 0; i < kSamples; i++) {
    let u = (f32(i) + fract(seed + f32(i) * 0.618034)) / f32(kSamples);
    let ang = 6.2831853 * fract(seed * 7.0 + f32(i) * 0.754878);
    let rr = sqrt(u);
    let dir = vec3f(rr * cos(ang), rr * sin(ang), sqrt(max(1.0 - u, 0.0)));
    let len = mix(0.15, 1.0, fract(seed * 13.0 + f32(i) * 0.3819660));
    let s = p + (t * dir.x + b * dir.y + n * dir.z) * (radius * len * len);
    let suv = screenOf(s);
    if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) { continue; }
    let at = texel(suv);
    let sceneDepth = textureLoad(texDepth, at, 0).r;
    if (sceneDepth <= 0.0) { continue; }             // background: nothing there
    let diff = -s.z - sceneDepth;                    // > 0: the scene is in front of the sample
    let range = smoothstep(0.0, 1.0, radius / max(abs(-p.z - sceneDepth), 1e-5));
    res.occlusion += select(0.0, 1.0, diff > 0.02 * radius) * range;
    if (gather) {
      // The surface on screen there, if it is within reach and in front of this one.
      let to = viewPos(suv, sceneDepth) - p;
      let d = length(to);
      let facing = dot(n, to) / max(d, 1e-6);
      if (d < 2.0 * radius && facing > 0.0) {
        res.glow += cellEmission(textureLoad(texAlbedo, at, 0).rgb, textureLoad(texMaterial, at, 0).r) * facing;
      }
    }
  }
  res.occlusion = res.occlusion / f32(kSamples);
  return res;
}

@fragment
fn fs(i: FullOut) -> @location(0) vec4f {
  let at = vec2i(i.pos.xy);
  let depth = textureLoad(texDepth, at, 0).r;
  let gather = F.matEmission2.y > 0.5 && F.matEmission.a > 0.0;
  if (depth <= 0.0 || (F.ao.w < 0.5 && !gather)) { return vec4f(0.0, 0.0, 0.0, 1.0); }
  let n = normalize(textureLoad(texNormal, at, 0).xyz);
  let p = viewPos(i.uv, depth);
  let tile = vec2f(at & vec2i(3));
  let seed = hash12(tile * 1.7 + 0.31);
  let a = 6.2831853 * hash12(tile * 2.3 + 5.17);
  let rnd = vec3f(cos(a), sin(a), 0.0);
  let t = normalize(rnd - n * dot(rnd, n) + vec3f(0.0, 0.0, 1e-4));
  let b = cross(n, t);
  let small = occlusionAt(p, n, t, b, F.ao.x, seed, gather);
  let large = occlusionAt(p, n, t, b, F.ao.y, fract(seed + 0.5), gather);
  // A surface seen almost edge-on (the rim of the form, of a fold) has its samples fall behind
  // itself, which would draw a dark line of single pixels along the rim: it counts for less.
  let rim = clamp(4.0 * dot(n, normalize(-p)), 0.0, 1.0);
  let occ = (0.5 * small.occlusion + 0.8 * large.occlusion) * rim;
  // (The samples are spread evenly, not by the cosine: twice their mean stands for the light
  // a matt surface receives.)
  let open = select(clamp(1.0 - F.ao.z * occ, 0.0, 1.0), 1.0, F.ao.w < 0.5);
  return vec4f((small.glow + large.glow) * (2.0 / f32(2 * kSamples)), open);
}
`;

// Averages the occlusion over the 4 x 4 pixels its pattern repeats in, among pixels at about
// the same depth.
export const blur = /* wgsl */ `
${frame}
${screen}
@group(1) @binding(0) var texDepth: texture_2d<f32>;
@group(1) @binding(1) var texGather: texture_2d<f32>;
@fragment
fn fs(i: FullOut) -> @location(0) vec4f {
  let at = vec2i(i.pos.xy);
  let depth = textureLoad(texDepth, at, 0).r;
  if (depth <= 0.0) { return vec4f(0.0, 0.0, 0.0, 1.0); }
  let limit = vec2i(F.viewport.xy) - 1;
  let reach = max(0.75, 0.01 * depth);
  // The slope of the surface under the pixel (the smaller of the steps to either side, so an
  // edge beside it does not count): a surface seen almost edge-on changes depth from pixel to
  // pixel, and its pixels still belong together.
  let dl = depth - textureLoad(texDepth, clamp(at - vec2i(1, 0), vec2i(0), limit), 0).r;
  let dr = textureLoad(texDepth, clamp(at + vec2i(1, 0), vec2i(0), limit), 0).r - depth;
  let du = depth - textureLoad(texDepth, clamp(at - vec2i(0, 1), vec2i(0), limit), 0).r;
  let dd = textureLoad(texDepth, clamp(at + vec2i(0, 1), vec2i(0), limit), 0).r - depth;
  let slope = vec2f(select(dr, dl, abs(dl) < abs(dr)), select(dd, du, abs(du) < abs(dd)));
  var sum = vec4f(0.0);
  var weight = 0.0;
  var plain = vec4f(0.0);
  var count = 0.0;
  for (var y = -2; y < 2; y++) {
    for (var x = -2; x < 2; x++) {
      let q = clamp(at + vec2i(x, y), vec2i(0), limit);
      let d = textureLoad(texDepth, q, 0).r;
      if (d <= 0.0) { continue; }
      let dz = (d - depth - dot(slope, vec2f(f32(x), f32(y)))) / reach;
      let w = exp(-dz * dz);
      let g = textureLoad(texGather, q, 0);
      sum += w * g;
      weight += w;
      plain += g;
      count += 1.0;
    }
  }
  if (weight < 1e-4 || count < 0.5) { return textureLoad(texGather, at, 0); }
  // A pixel that stands alone in depth (a gap one pixel wide between two folds) has nothing to
  // average with and would keep the noise of its own few samples: it takes on the occlusion
  // around it instead.
  return mix(plain / count, sum / weight, clamp((weight - 1.0) / 4.0, 0.0, 1.0));
}
`;

// How a surface reflects light (shaders/bsdf.glsl): a GGX highlight over a matt base.
const bsdf = /* wgsl */ `
const kPi = 3.14159265;
fn fresnel(f0: vec3f, cosine: f32) -> vec3f {
  let m = clamp(1.0 - cosine, 0.0, 1.0);
  let m2 = m * m;
  return f0 + (1.0 - f0) * (m2 * m2 * m);
}
fn ggxD(ndh: f32, a: f32) -> f32 {
  let a2 = a * a;
  let d = ndh * ndh * (a2 - 1.0) + 1.0;
  return a2 / (kPi * d * d);
}
fn ggxV(ndl: f32, ndv: f32, a: f32) -> f32 {
  let a2 = a * a;
  let gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
  let gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-6);
}
fn alphaOf(roughness: f32) -> f32 {
  let r = clamp(roughness, 0.03, 1.0);
  return r * r;
}
// The highlight of a light from direction l, on the scale of the matt term colour * ndl.
fn highlight(n: vec3f, v: vec3f, l: vec3f, f0: vec3f, a: f32) -> vec3f {
  let ndl = dot(n, l);
  if (ndl <= 0.0) { return vec3f(0.0); }
  let h = normalize(l + v);
  let ndv = max(dot(n, v), 1e-3);
  return fresnel(f0, max(dot(v, h), 0.0)) * (kPi * ggxD(max(dot(n, h), 0.0), a) * ggxV(ndl, ndv, a) * ndl);
}
`;

// The studio of the desktop viewer (studioEnvironment in src/viewer/environment.cpp: walls that
// brighten towards the ceiling, a darker warm floor, three soft lamps), as functions of the
// direction instead of an image: seen sharp or through a rough surface, and as the light a
// matt surface gathers from all of it.
const studio = /* wgsl */ `
const kKeyDir = vec3f(-0.5426, 0.6412, 0.5426);
const kFillDir = vec3f(0.9125, 0.1610, 0.3757);
const kRimDir = vec3f(0.2507, 0.3510, -0.9022);
fn studioRoom(d: vec3f) -> vec3f {
  if (d.y >= 0.0) { return vec3f(0.20, 0.21, 0.24) + vec3f(0.18, 0.19, 0.22) * d.y; }
  return vec3f(0.20, 0.19, 0.18) + vec3f(0.10) * d.y;
}
// A round lamp, full inside 'radius' radians and fading out over the same angle again, seen
// through a surface that blurs by 'blur' radians: wider and, by as much, fainter.
fn lamp(d: vec3f, towards: vec3f, radius: f32, blur: f32) -> f32 {
  let wide = sqrt(radius * radius + blur * blur);
  let angle = acos(clamp(dot(d, towards), -1.0, 1.0));
  let t = clamp((2.0 * wide - angle) / wide, 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t) * (radius * radius) / (wide * wide);
}
fn studioMirror(d: vec3f, roughness: f32) -> vec3f {
  let blur = 1.4 * roughness * roughness + 0.25 * roughness;
  // A rough surface sees the room blurred too: towards its mean.
  let room = mix(studioRoom(d), vec3f(0.24, 0.245, 0.26), clamp(blur, 0.0, 1.0));
  return room + 7.0 * lamp(d, kKeyDir, 0.30, blur) * vec3f(1.00, 0.97, 0.92) + 1.6 * lamp(d, kFillDir, 0.40, blur) * vec3f(0.85, 0.92, 1.00) +
         3.5 * lamp(d, kRimDir, 0.22, blur) * vec3f(1.00, 0.96, 0.90);
}
// The light a matt surface facing n gathers: the room, and each lamp by the solid angle it
// covers and how squarely it is faced.
fn studioMatt(n: vec3f) -> vec3f {
  let room = mix(vec3f(0.15, 0.14, 0.13), vec3f(0.29, 0.305, 0.35), 0.5 + 0.5 * n.y);
  let lamps = 1.39 * max(dot(n, kKeyDir), 0.0) * vec3f(1.00, 0.97, 0.92) + 0.56 * max(dot(n, kFillDir), 0.0) * vec3f(0.85, 0.92, 1.00) +
              0.38 * max(dot(n, kRimDir), 0.0) * vec3f(1.00, 0.96, 0.90);
  return room + lamps;
}
`;

// Lighting (shaders/light.frag and combine.frag), then exposure, the soft tone curve and gamma.
// What the desktop viewer does with a blur of the light image (subsurface scattering) is
// approximated here in the shading itself: a scattering material is lit past the edge of the
// light, further in the colours that travel far, and its creases and shadow edges take on
// those colours.
export const light = /* wgsl */ `
${frame}
${palette}
${screen}
${bsdf}
${studio}
@group(1) @binding(0) var texDepth: texture_2d<f32>;
@group(1) @binding(1) var texNormal: texture_2d<f32>;
@group(1) @binding(2) var texAlbedo: texture_2d<f32>;
@group(1) @binding(3) var texMaterial: texture_2d<f32>;
@group(1) @binding(4) var texGather: texture_2d<f32>;
@group(1) @binding(5) var texShadow: texture_depth_2d;

// The key light at a point, from the shadow map: x = the share that reaches it (a few rotated
// taps for a soft edge), y = how far the light travelled behind the first surface it met before
// it got here (world units): how much material it had to cross.
fn keyLight(p: vec3f, n: vec3f, pixel: vec2f) -> vec2f {
  if (F.shadow2.y < 0.5) { return vec2f(1.0, 0.0); }
  let world = (F.invView * vec4f(p, 1.0)).xyz + (F.invView * vec4f(n, 0.0)).xyz * F.shadow.x;
  let lc = F.lightViewProj * vec4f(world, 1.0);
  let uv = vec2f(lc.x * 0.5 + 0.5, 0.5 - lc.y * 0.5);
  if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || lc.z > 1.0) { return vec2f(1.0, 0.0); }
  let size = F.shadow2.z;
  let spin = 6.2831853 * hash12(pixel + 5.0 * vec2f(17.13, 3.71));
  var lit = 0.0;
  var behind = 0.0;
  const kTaps = 12;
  for (var i = 0; i < kTaps; i++) {
    let r = F.shadow.y * sqrt((f32(i) + 0.5) / f32(kTaps));
    let a = spin + f32(i) * 2.3999632;  // golden angle
    let at = clamp(vec2i((uv + r * vec2f(cos(a), sin(a)) * F.shadow.z) * size), vec2i(0), vec2i(i32(size) - 1));
    let d = textureLoad(texShadow, at, 0);
    lit += select(1.0, 0.0, lc.z - F.shadow.w > d);
    behind += max(lc.z - F.shadow.w - d, 0.0);
  }
  return vec2f(mix(1.0, lit / f32(kTaps), F.lightColor.a), behind / f32(kTaps) * F.shadow2.x);
}

// Light bounces between the walls of a crease, so a crease in a bright material is less dark
// than the occlusion alone says, and takes on the colour of the material (Jimenez et al. 2016).
fn bounced(open: f32, albedo: vec3f) -> vec3f {
  let a = 2.0404 * albedo - 0.3324;
  let b = -4.7951 * albedo + 0.6417;
  let c = 2.7552 * albedo + 0.6903;
  return max(vec3f(open), ((open * a + b) * open + c) * open);
}

fn backgroundColor(uv: vec2f) -> vec3f {
  let top = F.background.rgb;
  let c = mix(top, top * (1.0 - F.palette.z), uv.y);
  let q = uv - 0.5;
  return c * (1.0 - F.background.a * dot(q, q));
}

// Soft: unchanged up to 0.8, then eased towards 1 by the brightest channel, which keeps the hue
// of a glow; what is many times too bright turns pale, as a hot light does (shaders/screen.glsl).
fn toneSoft(c: vec3f) -> vec3f {
  let m = max(c.r, max(c.g, c.b));
  if (m <= 0.8) { return c; }
  let eased = 0.8 + 0.2 * (1.0 - exp(-(m - 0.8) / 0.2));
  return mix(c * (eased / m), vec3f(eased), 0.7 * smoothstep(1.5, 8.0, m));
}

fn toScreen(linear: vec3f) -> vec4f {
  return vec4f(pow(max(toneSoft(max(linear * F.baseColor.a, vec3f(0.0))), vec3f(0.0)), vec3f(1.0 / 2.2)), 1.0);
}

@fragment
fn fs(i: FullOut) -> @location(0) vec4f {
  let at = vec2i(i.pos.xy);
  let depth = textureLoad(texDepth, at, 0).r;
  if (depth <= 0.0) { return toScreen(backgroundColor(i.uv)); }
  let n = normalize(textureLoad(texNormal, at, 0).xyz);
  let p = viewPos(i.uv, depth);
  let v = normalize(-p);
  let albedoWall = textureLoad(texAlbedo, at, 0);
  let albedo = albedoWall.rgb;
  let material = textureLoad(texMaterial, at, 0);
  var gather = vec4f(0.0, 0.0, 0.0, 1.0);
  if (F.ao.w > 0.5 || (F.matEmission2.y > 0.5 && F.matEmission.a > 0.0)) { gather = textureLoad(texGather, at, 0); }
  let open = gather.a;
  let reachColor = F.matSssColor.rgb;
  // In a scattering material a crease is not simply dark: light gets into it under the
  // surface, the colours that travel furthest most.
  let openMatt = pow(bounced(open, albedo), vec3f(1.0) - 0.8 * material.b * reachColor);
  let ndv = max(dot(n, v), 1e-3);

  // --- the key light
  let l = normalize(F.light.xyz);
  let key = keyLight(p, n, i.pos.xy);
  // Light through the material: a share of what falls on the far side crosses the sheet, the
  // less the more material is in the way, and red gets further than blue.
  let reach = max(F.matSss.w * reachColor, vec3f(1e-4));
  let through = material.a * exp(-(0.25 * F.matSss.w + key.y) / reach);
  let facing = dot(n, l);
  // A scattering material is lit past the edge of the light, each colour as far as it travels,
  // and the edge of a shadow on it is soft in those colours.
  let spread = clamp(F.matSss.y / 2.5, 0.0, 1.5);
  let wrap = material.b * (0.5 + 0.4 * spread * reachColor);
  let ndl = max((vec3f(facing) + wrap) / (vec3f(1.0) + wrap), vec3f(0.0));
  let lit = pow(vec3f(key.x), vec3f(1.0) - 0.6 * material.b * min(spread, 1.0) * reachColor);
  let keyColor = (1.0 - F.light.w) * F.lightColor.rgb;
  let keyMatt = keyColor * (ndl * max(lit, through) + max(-facing, 0.0) * through) * mix(vec3f(1.0), openMatt, 0.6);

  // --- light from the surroundings: the sky above and the ground below, or the studio, and
  // the surroundings mirrored in the surface, sharper the smoother it is.
  let nWorld = (F.invView * vec4f(n, 0.0)).xyz;
  let mirrorWorld = (F.invView * vec4f(reflect(-v, n), 0.0)).xyz;
  var around: vec3f;
  var mirrored: vec3f;
  if (F.env.x > 0.5) {
    around = studioMatt(nWorld) * F.env.y;
    mirrored = studioMirror(mirrorWorld, material.g) * F.env.y;
  } else {
    around = mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * nWorld.y);
    mirrored = mix(F.ground.rgb, F.sky.rgb, 0.5 + 0.5 * mirrorWorld.y);
  }
  let f0 = mix(vec3f(0.08 * F.matBase.z), albedo, F.matBase.y);
  let mirrorShare = f0 + (max(vec3f(1.0 - material.g), f0) - f0) * pow(clamp(1.0 - ndv, 0.0, 1.0), 5.0);

  let mattLight = keyMatt + F.light.w * around * openMatt + gather.rgb;
  var direct = keyColor * key.x * highlight(n, v, l, f0, alphaOf(material.g)) * mix(1.0, open, 0.6);
  direct += F.light.w * mirrored * mirrorShare * open;
  direct += cellEmission(albedo, material.r);

  // The matt colour: none for a metal, plus the sheen, a pale rim at grazing angles.
  let matt = albedo * (1.0 - F.matBase.y) + F.matBase.w * pow(1.0 - clamp(ndv, 0.0, 1.0), 5.0);
  var color = matt * mattLight + direct;
  color = mix(color, F.wall.rgb, albedoWall.a * F.wall.a);
  return toScreen(color);
}
`;

// To the canvas: optionally smooths edges (FXAA, the short form of Timothy Lottes's filter),
// scales from the size rendered to the size shown, and adds a little noise against banding.
export const final = /* wgsl */ `
${frame}
${screen}
@group(1) @binding(0) var texColor: texture_2d<f32>;
@group(1) @binding(1) var smp: sampler;

fn tap(uv: vec2f) -> vec3f { return textureSampleLevel(texColor, smp, uv, 0.0).rgb; }
fn luma(c: vec3f) -> f32 { return dot(c, vec3f(0.299, 0.587, 0.114)); }

fn fxaa(uv: vec2f) -> vec3f {
  let px = F.viewport.zw;
  let m = tap(uv);
  let lNW = luma(tap(uv + vec2f(-1.0, -1.0) * px));
  let lNE = luma(tap(uv + vec2f(1.0, -1.0) * px));
  let lSW = luma(tap(uv + vec2f(-1.0, 1.0) * px));
  let lSE = luma(tap(uv + vec2f(1.0, 1.0) * px));
  let lM = luma(m);
  let lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
  let lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
  if (lMax - lMin < max(0.0312, lMax * 0.125)) { return m; }
  var dir = vec2f(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
  let reduce = max((lNW + lNE + lSW + lSE) * (0.25 / 8.0), 1.0 / 128.0);
  let rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
  dir = clamp(dir * rcp, vec2f(-8.0), vec2f(8.0)) * px;
  let a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5)) + tap(uv + dir * (2.0 / 3.0 - 0.5)));
  let b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5) + tap(uv + dir * 0.5));
  let lB = luma(b);
  if (lB < lMin || lB > lMax) { return a; }
  return b;
}

@fragment
fn fs(i: FullOut) -> @location(0) vec4f {
  var c: vec3f;
  if (F.misc.x > 0.5) { c = fxaa(i.uv); } else { c = tap(i.uv); }
  c += (hash12(i.pos.xy + 0.37) - 0.5) / 255.0;
  return vec4f(c, 1.0);
}
`;
