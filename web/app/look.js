// The look: everything that decides how the form is drawn, apart from the camera. One table
// drives the defaults, the controls of the panel and the short names used in a shared link.
// Defaults follow RenderSettings of the desktop viewer (src/viewer/renderer.hpp) where the
// setting exists there. Colours are linear red, green, blue as the shaders use them.
//
// { id, k (short name in a link), def, and for the panel: label, section, then
//   min / max / step / log (a slider), options (a choice), or neither (a switch or a colour),
//   about (a line of help), when (shown only while this holds) }

import { MATERIALS, materialLook } from './materials.js';

// The cell properties a colour or a part of the material can follow, with their number in the shaders.
export const PROPERTIES = { none: 0, age: 1, food: 2, generation: 4, curvature: 5, activator: 6 };
const propertyOptions = [['none', 'nothing'], ['age', 'age'], ['food', 'food'], ['generation', 'generation'], ['curvature', 'curvature'], ['activator', 'morphogen activator']];
const PARTS = ['glow', 'roughness', 'subsurface', 'light through'];

const driven = (part) => (l) => l['drive' + part] !== 'none';
const driveRows = PARTS.flatMap((name, part) => [
  { id: 'drive' + part, k: 'v' + part, def: 'none', section: 'material', group: part === 0 ? 'Driven by the cells' : undefined, label: name + ' follows', options: propertyOptions,
    about: 'This part of the material follows a property of each cell' },
  { id: 'lo' + part, k: 'vl' + part, def: [0, 0.2, 0, 0][part], section: 'material', label: 'at the lowest', min: 0, max: 1, when: driven(part),
    about: 'Its value where the property is lowest (for age: in the oldest cells)' },
  { id: 'hi' + part, k: 'vh' + part, def: [1, 0.9, 1, 1][part], section: 'material', label: 'at the highest', min: 0, max: 1, when: driven(part),
    about: 'Its value where the property is highest (for age: in the youngest cells)' },
  { id: 'from' + part, k: 'vf' + part, def: 0, section: 'material', label: 'range from', min: 0, max: 1, when: driven(part),
    about: 'The part of the range of the property over which it changes: start' },
  { id: 'to' + part, k: 'vt' + part, def: 1, section: 'material', label: 'range to', min: 0, max: 1, when: driven(part), about: '... and end' },
]);

export const LOOK = [
  // --- draw
  { id: 'draw', k: 'd', def: 'cells', section: 'draw', label: 'draw as', options: [['cells', 'cells (spheres)'], ['membrane', 'membrane']],
    about: 'Every cell as a sphere, or the sheet between the cells' },
  { id: 'radius', k: 'r', def: 0.55, section: 'draw', label: 'cell radius', min: 0.1, max: 1.5, when: (l) => l.draw === 'cells',
    about: 'In link lengths: cells sit about one apart' },
  { id: 'walls', k: 'w', def: false, section: 'draw', label: 'cell walls', when: (l) => l.draw === 'membrane', about: 'Lines on the membrane where one cell ends and the next begins' },
  { id: 'wallWidth', k: 'ww', def: 1, section: 'draw', label: 'wall width', min: 0.5, max: 4, when: (l) => l.draw === 'membrane' && l.walls, about: 'In pixels' },
  { id: 'wallStrength', k: 'ws', def: 0.8, section: 'draw', label: 'wall strength', min: 0, max: 1, when: (l) => l.draw === 'membrane' && l.walls },
  { id: 'wallColor', k: 'wc', def: [0.05, 0.05, 0.07], section: 'draw', label: 'wall colour', color: true, when: (l) => l.draw === 'membrane' && l.walls },

  // --- colour
  { id: 'colorBy', k: 'cb', def: 'age', section: 'colour', label: 'colour by',
    options: [['material', 'the material\'s colour'], ['age', 'age'], ['generation', 'generation'], ['food', 'food'], ['curvature', 'curvature'], ['activator', 'morphogen activator']],
    about: 'One colour for all cells, or a palette over a property of each cell' },
  { id: 'palette', k: 'p', def: 'ocean', section: 'colour', label: 'palette', when: (l) => l.colorBy !== 'material',
    options: [['ocean', 'blue to yellow'], ['viridis', 'purple to yellow'], ['magma', 'black to cream'], ['diverging', 'blue, white, red'], ['grey', 'grey']] },
  { id: 'paletteReverse', k: 'pr', def: false, section: 'colour', label: 'reverse the palette', when: (l) => l.colorBy !== 'material' },

  // --- material
  { id: 'baseColor', k: 'bc', def: [0.80, 0.74, 0.66], section: 'material', label: 'base colour', color: true, about: 'The colour of the cells when they are coloured by the material' },
  { id: 'roughness', k: 'ro', def: 0.6, section: 'material', group: 'Surface', label: 'roughness', min: 0.03, max: 1, about: 'From mirror-smooth to matt' },
  { id: 'metal', k: 'me', def: 0, section: 'material', label: 'metal', min: 0, max: 1, about: 'A metal mirrors in its own colour and has no matt part' },
  { id: 'specular', k: 'sp', def: 0.5, section: 'material', label: 'highlight', min: 0, max: 1, about: 'Strength of the highlight of a non-metal' },
  { id: 'sheen', k: 'sh', def: 0, section: 'material', label: 'sheen', min: 0, max: 1, about: 'A pale rim at grazing angles' },
  { id: 'subsurface', k: 'ss', def: 0, section: 'material', group: 'Light under the surface', label: 'subsurface', min: 0, max: 1,
    about: 'Light that scatters under the surface: it reaches past the edge of the light and warms the creases (an approximation; the desktop viewer blurs the light)' },
  { id: 'sssRadius', k: 'sr', def: 1.5, section: 'material', label: 'scatter radius', min: 0, max: 8, when: (l) => l.subsurface > 0, about: 'How far it travels, in link lengths' },
  { id: 'sssColor', k: 'sc', def: [1, 0.35, 0.2], section: 'material', label: 'reach per colour', color: true, when: (l) => l.subsurface > 0 || l.translucency > 0,
    about: 'How far red, green and blue get: red travels furthest in skin' },
  { id: 'translucency', k: 'tl', def: 0, section: 'material', label: 'light through', min: 0, max: 0.95, about: 'Light that crosses thin parts from behind (uses the shadow map for the thickness)' },
  { id: 'transDepth', k: 'td', def: 3, section: 'material', label: 'its depth', min: 0.2, max: 20, log: true, when: (l) => l.translucency > 0, about: 'How much material light gets through, in link lengths' },
  { id: 'emission', k: 'em', def: 0, section: 'material', group: 'Glow', label: 'emission', min: 0, max: 12, about: 'Light the cells give off' },
  { id: 'emissionColor', k: 'ec', def: [1, 0.75, 0.35], section: 'material', label: 'glow colour', color: true, when: (l) => l.emission > 0 },
  { id: 'emissionTint', k: 'et', def: 0, section: 'material', label: 'glow colour share', min: 0, max: 1, when: (l) => l.emission > 0, about: '0: cells glow in their own colour; 1: in the glow colour' },
  { id: 'emissionLights', k: 'el', def: true, section: 'material', label: 'glow lights the neighbours', when: (l) => l.emission > 0, about: 'Gathered in screen space: only what is in view lights anything' },
  ...driveRows,

  // --- light
  { id: 'lightFollow', k: 'lf', def: true, section: 'light', label: 'key light follows the camera', about: 'Otherwise it stays where it is, like a sun' },
  { id: 'lightAz', k: 'la', def: 25, section: 'light', label: 'azimuth', min: -180, max: 180, step: 1, about: 'Degrees to the side: of the view, or around the vertical when the light is fixed' },
  { id: 'lightEl', k: 'le', def: 34, section: 'light', label: 'elevation', min: -30, max: 90, step: 1, about: 'Degrees above the view axis, or above the horizon when the light is fixed' },
  { id: 'lightIntensity', k: 'li', def: 1, section: 'light', label: 'intensity', min: 0, max: 4 },
  { id: 'lightColor', k: 'lc', def: [1, 1, 1], section: 'light', label: 'light colour', color: true },
  { id: 'ambient', k: 'am', def: 0.55, section: 'light', group: 'Surroundings', label: 'share from around', min: 0, max: 1, about: 'How much of the light comes from the surroundings instead of the key light' },
  { id: 'skyColor', k: 'sk', def: [1, 1, 1], section: 'light', label: 'sky light', color: true, when: (l) => !l.env, about: 'On surfaces that face up' },
  { id: 'groundColor', k: 'gr', def: [0.5, 0.5, 0.5], section: 'light', label: 'ground light', color: true, when: (l) => !l.env, about: 'On surfaces that face down' },
  { id: 'env', k: 'en', def: false, section: 'light', label: 'studio surroundings', about: 'A room with three soft lamps in place of the sky and ground light: something for smooth and metal surfaces to mirror' },
  { id: 'envIntensity', k: 'ei', def: 1, section: 'light', label: 'studio intensity', min: 0, max: 4, when: (l) => l.env },

  // --- shadows and occlusion
  { id: 'shadows', k: 'sd', def: true, section: 'shade', label: 'shadow of the key light' },
  { id: 'shadowStrength', k: 'sds', def: 0.85, section: 'shade', label: 'shadow strength', min: 0, max: 1, when: (l) => l.shadows },
  { id: 'shadowSoftness', k: 'sdo', def: 1.5, section: 'shade', label: 'soft edge', min: 0, max: 6, when: (l) => l.shadows, about: 'In texels of the shadow map' },
  { id: 'shadowSize', k: 'sdz', def: '2048', section: 'shade', label: 'shadow map', options: [['1024', '1024'], ['2048', '2048'], ['4096', '4096']], when: (l) => l.shadows || l.translucency > 0 },
  { id: 'ao', k: 'ao', def: true, section: 'shade', group: 'Ambient occlusion', label: 'ambient occlusion', about: 'Darkens creases and the contact between cells (screen space)' },
  { id: 'aoIntensity', k: 'aoi', def: 1, section: 'shade', label: 'occlusion strength', min: 0, max: 2, when: (l) => l.ao },
  { id: 'aoSmall', k: 'aos', def: 1.5, section: 'shade', label: 'small radius', min: 0.2, max: 6, when: (l) => l.ao, about: 'Contact between neighbouring cells, in link lengths' },
  { id: 'aoLarge', k: 'aol', def: 8, section: 'shade', label: 'large radius', min: 1, max: 60, log: true, when: (l) => l.ao, about: 'The insides of folds' },

  // --- picture
  { id: 'exposure', k: 'ex', def: 1, section: 'picture', label: 'exposure', min: 0.2, max: 3 },
  { id: 'background', k: 'bg', def: [0.13, 0.14, 0.17], section: 'picture', label: 'background', color: true },
  { id: 'gradient', k: 'bgg', def: 0.45, section: 'picture', label: 'gradient', min: 0, max: 1, about: 'How much darker the bottom of the background is' },
  { id: 'vignette', k: 'vg', def: 0.6, section: 'picture', label: 'vignette', min: 0, max: 2, about: 'Darker corners' },
  { id: 'scale', k: 'rs', def: 1, section: 'picture', label: 'render scale', min: 0.5, max: 2, step: 0.25,
    about: 'Pixels drawn per pixel shown, along each axis. Above 1 smooths every edge (2 costs four times the work); below 1 is faster' },
  { id: 'fxaa', k: 'fx', def: true, section: 'picture', label: 'smooth edges', about: 'One cheap pass over the finished picture (FXAA)' },
];

export const LOOK_DEFAULTS = Object.fromEntries(LOOK.map((s) => [s.id, Array.isArray(s.def) ? [...s.def] : s.def]));
export const DEFAULT_MATERIAL = 'clay';

// The look a material starts from: the defaults with the material applied (colour included).
// A link stores the name of the material and what differs from this.
export function baselineLook(materialName) {
  const m = MATERIALS.find((x) => x.name === materialName);
  const look = structuredClone(LOOK_DEFAULTS);
  if (m && m.name !== DEFAULT_MATERIAL) Object.assign(look, materialLook(m, true));
  return look;
}

// Linear colour <-> the #rrggbb a colour field shows (what the colour looks like on screen:
// the picture is written with gamma 2.2).
export function toHex(rgb) {
  return '#' + rgb.map((c) => Math.round(255 * Math.pow(Math.min(Math.max(c, 0), 1), 1 / 2.2)).toString(16).padStart(2, '0')).join('');
}
export function fromHex(hex) {
  const m = /^#?([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex);
  if (!m) return null;
  return [1, 2, 3].map((i) => +Math.pow(parseInt(m[i], 16) / 255, 2.2).toFixed(4));
}

export function sameValue(a, b) {
  if (Array.isArray(a) && Array.isArray(b)) return toHex(a) === toHex(b);
  if (typeof a === 'number' && typeof b === 'number') return Math.abs(a - b) <= 1e-6 * Math.max(1, Math.abs(a), Math.abs(b));
  return a === b;
}
