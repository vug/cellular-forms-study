// The material library of the desktop viewer (src/viewer/materials.cpp), with the same values.
// A material sets the material part of the look; `environment` turns the studio surroundings
// on (a metal or a glaze looks like little without something to mirror). `drive` says which
// cell property drives a part of the material: [part, property, low, high, from, to] with
// part 0 emission, 1 roughness, 2 subsurface, 3 light through, and the property as in
// PROPERTIES of look.js (for age, low is the oldest cell and high the youngest).

const base = {
  specular: 0.5, sheen: 0, subsurface: 0, sssRadius: 1.5, sssColor: [1, 0.35, 0.2], translucency: 0, transDepth: 3,
  emission: 0, emissionColor: [1, 0.75, 0.35], emissionTint: 0, environment: false, drive: [],
};

export const MATERIALS = [
  { name: 'clay', note: 'matt, nothing scatters: the plain look the viewer starts with', color: [0.80, 0.74, 0.66], roughness: 0.6, metal: 0 },
  { name: 'skin', note: 'soft highlights, red light under the surface, thin parts glow against the light', color: [0.80, 0.52, 0.42], roughness: 0.42, metal: 0,
    sheen: 0.25, subsurface: 0.9, sssRadius: 2.6, sssColor: [1, 0.36, 0.18], translucency: 0.15, transDepth: 4 },
  { name: 'tissue', note: 'a thin wet membrane: much of the light goes through', color: [0.90, 0.72, 0.68], roughness: 0.3, metal: 0, specular: 0.6,
    sheen: 0.3, subsurface: 0.6, sssRadius: 1.6, sssColor: [1, 0.5, 0.4], translucency: 0.6, transDepth: 7 },
  { name: 'wax', note: 'light travels far inside and comes out warm', color: [0.93, 0.86, 0.70], roughness: 0.35, metal: 0,
    subsurface: 1, sssRadius: 3.2, sssColor: [1, 0.72, 0.42], translucency: 0.4, transDepth: 6 },
  { name: 'jade', note: 'polished stone that lets green light through', color: [0.36, 0.68, 0.46], roughness: 0.14, metal: 0, specular: 0.65,
    subsurface: 0.9, sssRadius: 2.6, sssColor: [0.45, 1, 0.6], translucency: 0.35, transDepth: 5, environment: true },
  { name: 'porcelain', note: 'glazed and white, a little light under the glaze', color: [0.95, 0.94, 0.91], roughness: 0.08, metal: 0, specular: 0.65,
    subsurface: 0.5, sssRadius: 0.8, sssColor: [1, 0.9, 0.8], environment: true },
  { name: 'pearl', note: 'smooth with a pale rim at grazing angles', color: [0.92, 0.88, 0.86], roughness: 0.2, metal: 0, specular: 0.8, sheen: 0.6,
    subsurface: 0.4, sssRadius: 1.0, sssColor: [1, 0.85, 0.8], environment: true },
  { name: 'rubber', note: 'dark and matt with a broad highlight', color: [0.07, 0.07, 0.08], roughness: 0.65, metal: 0 },
  { name: 'gold', note: 'a metal: it mirrors its surroundings in its own colour', color: [1.0, 0.77, 0.34], roughness: 0.25, metal: 1, environment: true },
  { name: 'chrome', note: 'a mirror', color: [0.95, 0.95, 0.96], roughness: 0.06, metal: 1, environment: true },
  { name: 'copper patina', note: 'metal that is rougher where the surface curves in', color: [0.85, 0.47, 0.34], roughness: 0.3, metal: 1, environment: true,
    drive: [[1, 'curvature', 0.75, 0.12, 0, 1]] },
  { name: 'bioluminescent', note: 'a dark translucent body; cells glow by how much food they hold', color: [0.05, 0.09, 0.13], roughness: 0.4, metal: 0,
    sheen: 0.2, subsurface: 0.7, sssRadius: 2.0, sssColor: [0.5, 0.9, 1.0], translucency: 0.5, transDepth: 6,
    emission: 3, emissionColor: [0.15, 0.85, 1.0], emissionTint: 1, drive: [[0, 'food', 0, 1, 0.55, 1]] },
  { name: 'ember', note: 'the youngest cells glow like coals', color: [0.10, 0.04, 0.03], roughness: 0.7, metal: 0, specular: 0.3,
    subsurface: 0.5, sssRadius: 1.5, sssColor: [1, 0.4, 0.2], translucency: 0.3, transDepth: 4,
    emission: 5, emissionColor: [1.0, 0.38, 0.08], emissionTint: 1, drive: [[0, 'age', 0, 1, 0.7, 1]] },
  { name: 'growth light', note: 'skin that glows where the morphogen activator is high', color: [0.80, 0.60, 0.55], roughness: 0.45, metal: 0,
    sheen: 0.2, subsurface: 0.8, sssRadius: 2.0, sssColor: [1, 0.4, 0.25], translucency: 0.3, transDepth: 4,
    emission: 3, emissionColor: [1.0, 0.8, 0.4], emissionTint: 1, drive: [[0, 'activator', 0, 1, 0.3, 1]] },
].map((m) => ({ ...base, ...m }));

export const findMaterial = (name) => MATERIALS.find((m) => m.name === name) ?? null;

// The values of the look that a material sets (the keys are those of LOOK in look.js).
export function materialLook(m, withColor) {
  const out = {
    roughness: m.roughness, metal: m.metal, specular: m.specular, sheen: m.sheen,
    subsurface: m.subsurface, sssRadius: m.sssRadius, sssColor: [...m.sssColor],
    translucency: m.translucency, transDepth: m.transDepth,
    emission: m.emission, emissionColor: [...m.emissionColor], emissionTint: m.emissionTint,
    // The values a part takes when it is driven, as the library has them where it drives nothing.
    drive0: 'none', drive1: 'none', drive2: 'none', drive3: 'none',
    lo0: 0, lo1: 0.2, lo2: 0, lo3: 0, hi0: 1, hi1: 0.9, hi2: 1, hi3: 1,
    from0: 0, from1: 0, from2: 0, from3: 0, to0: 1, to1: 1, to2: 1, to3: 1,
  };
  for (const [part, property, lo, hi, from, to] of m.drive) {
    out['drive' + part] = property;
    out['lo' + part] = lo;
    out['hi' + part] = hi;
    out['from' + part] = from;
    out['to' + part] = to;
  }
  if (m.environment) out.env = true;
  if (withColor) {
    out.colorBy = 'material';
    out.baseColor = [...m.color];
  }
  return out;
}
