// The parameters of the model as the app offers them: names and defaults as in
// src/core/params.hpp (checked against it; one exception, crowdLow, is marked below), ranges as
// the search uses them (scripts/explore.py, BASE_SPACE and SOURCES), a little wider, and inside
// what the simulation accepts (validate() in src/core/params.cpp; web/sim.js validateParams).
// A group with `switch` is an optional mechanism: it is off while its switch parameter has
// its `off` value, and switching it on sets the parameter to `on`. The switch parameter is
// what the simulation itself looks at, so a group is on exactly when its mechanism runs.
//
// { name, label, min, max, value (the default), log (slider on a logarithmic scale), step, about }
//
// Lengths are in world units; the rest length of a link (linkRestLength) is 1 here, so they
// are rest lengths.

export const GROUPS = [
  {
    title: 'Sheet',
    about: 'How the cells hold together and push apart. Each of the first three moves a cell a share of the way to a target; keep their sum below 1 (above 1 the sheet overshoots, from 2 the simulation refuses).',
    params: [
      { name: 'springFactor', label: 'spring', min: 0.02, max: 0.5, value: 0.15, about: 'pull towards the rest length of the links' },
      { name: 'planarFactor', label: 'planar', min: 0, max: 0.5, value: 0.1, about: 'pull towards the middle of the neighbours: stiffness against bending' },
      { name: 'bulgeFactor', label: 'bulge', min: 0, max: 0.5, value: 0.1, about: 'push along the normal that brings the links back to their rest length: the sheet bulges out where links are short' },
      { name: 'repulsionStrength', label: 'repulsion', min: 0.01, max: 0.5, value: 0.1, log: true, about: 'push between cells that are near but not linked' },
      { name: 'radiusOfInfluence', label: 'influence radius', min: 1, max: 3.5, value: 2, about: 'how far the repulsion reaches, in rest lengths' },
      { name: 'maxDisplacement', label: 'max move', min: 0.05, max: 1, value: 0.5, about: 'cap on the move of a cell in one step, in rest lengths' },
    ],
  },
  {
    title: 'Food and division',
    about: 'Every cell gets food every step and divides when it has enough; the two cells share the food.',
    params: [
      { name: 'foodMax', label: 'food per step', min: 0.0005, max: 0.03, value: 0.002, log: true, about: 'the most food a cell gets in one step (the amount is random, from the least up to this)' },
      { name: 'foodMin', label: 'least food per step', min: 0, max: 0.01, value: 0, about: 'the least food a cell gets in one step; not above the most' },
      { name: 'foodThreshold', label: 'food threshold', min: 0.25, max: 4, value: 1, log: true, about: 'a cell divides when its food is above this' },
      { name: 'daughterOffset', label: 'split offset', min: 0.02, max: 0.35, value: 0.1, about: 'how far each of the two cells starts from where the dividing cell was, on opposite sides, in rest lengths' },
    ],
  },
  {
    title: 'Oriented division',
    about: 'A dividing cell puts its daughter along an axis where it can, so the sheet grows in that direction: long bodies with ring folds.',
    switch: { name: 'orientAmount', off: 0, on: 0.7 },
    params: [
      { name: 'orientAmount', label: 'amount', min: 0, max: 1, value: 0, about: 'above 0 the direction decides between equally good ways to divide; towards 1 it may also win over keeping the number of neighbours even' },
      { name: 'orientAxisX', label: 'axis x', min: -1, max: 1, value: 0, about: 'the axis: only its direction counts; not all three zero' },
      { name: 'orientAxisY', label: 'axis y', min: -1, max: 1, value: 1 },
      { name: 'orientAxisZ', label: 'axis z', min: -1, max: 1, value: 0 },
    ],
  },
  {
    title: 'Food diffusion',
    about: 'Food spreads between linked cells: bristly forms with much surface.',
    switch: { name: 'foodDiffusion', off: 0, on: 0.25 },
    params: [
      { name: 'foodDiffusion', label: 'diffusion', min: 0, max: 0.6, value: 0, about: 'share of the way to the neighbours\' mean food, per step' },
    ],
  },
  {
    title: 'Food decay',
    about: 'Cells lose food over time, so cells that get little never divide.',
    switch: { name: 'foodDecay', off: 0, on: 0.002 },
    params: [
      { name: 'foodDecay', label: 'decay', min: 0, max: 0.02, value: 0, about: 'share of its food a cell loses per step' },
    ],
  },
  {
    title: 'Morphogens',
    about: 'Two substances on the cells react and spread along the links, which makes spots and stripes on the sheet. The pattern does nothing to the form until one of the first four parameters says what it changes.',
    switch: { name: 'morphogens', off: 0, on: 1 },
    params: [
      { name: 'morphFood', label: 'on food', min: -1, max: 1, value: 0, about: 'cells with the activator get more (or, below 0, less) food; at 1 from none to double' },
      { name: 'morphBulge', label: 'on bulge', min: -1, max: 1, value: 0, about: '... bulge more (or less)' },
      { name: 'morphPlanar', label: 'on stiffness', min: -1, max: 1, value: 0, about: '... are stiffer (or softer)' },
      { name: 'morphRest', label: 'on link length', min: -0.5, max: 0.5, value: 0, about: '... have longer (or shorter) links' },
      { name: 'morphFeed', label: 'feed', min: 0.01, max: 0.1, value: 0.055, about: 'feed rate of the reaction; with the kill rate it picks the pattern (0.055 / 0.062: stripes that fill the sheet; 0.035 / 0.065: spots, which need a larger sheet to get going)' },
      { name: 'morphKill', label: 'kill', min: 0.04, max: 0.07, value: 0.062, about: 'kill rate of the reaction' },
      { name: 'morphDiffA', label: 'substrate spread', min: 0.05, max: 0.9, value: 0.4, about: 'how fast the substrate spreads along the links' },
      { name: 'morphDiffB', label: 'activator spread', min: 0.02, max: 0.6, value: 0.2, about: 'how fast the activator spreads; half the substrate\'s gives features a few cells across' },
      { name: 'morphIterations', label: 'iterations per step', min: 1, max: 16, value: 4, step: 1, about: 'how fast the pattern moves compared with the growth' },
      { name: 'morphSeedLevel', label: 'seed level', min: 0, max: 1, value: 0.75, about: 'where the activator starts: lower, on more of the sheet. Only used at the moment the morphogens first start in a run' },
      { name: 'morphSeedScale', label: 'seed patch size', min: 0.5, max: 10, value: 3, about: 'size of the patches the activator starts in, in rest lengths. Only used at that moment too' },
    ],
  },
  {
    title: 'Field',
    about: 'A fixed, smooth random pattern in space that the form grows through. It does nothing by itself: the two groups below use it.',
    params: [
      { name: 'fieldScale', label: 'wavelength', min: 3, max: 80, value: 20, log: true, about: 'size of the pattern\'s features, in rest lengths' },
      { name: 'fieldContrast', label: 'contrast', min: 1, max: 5, value: 1, about: 'above 1 the pattern sharpens into patches' },
      { name: 'fieldOctaves', label: 'octaves', min: 1, max: 6, value: 1, step: 1, about: 'layers of finer detail, each half the size of the one before' },
    ],
  },
  {
    title: 'Food field',
    about: 'Food depends on where a cell is in the field: irregular, one-sided forms.',
    switch: { name: 'fieldAmount', off: 0, on: 0.7 },
    params: [
      { name: 'fieldAmount', label: 'amount', min: 0, max: 1, value: 0, about: '1: from no food to double food across the field' },
    ],
  },
  {
    title: 'Bulge field',
    about: 'The bulge of a cell depends on where it is in the field, so regions of the sheet fold differently.',
    switch: { name: 'fieldBulge', off: 0, on: 0.5 },
    params: [
      { name: 'fieldBulge', label: 'amount', min: -1, max: 1, value: 0, about: '1: from no bulge to double bulge across the field; below 0 the other way round' },
    ],
  },
  {
    title: 'Light',
    about: 'Food comes from a direction: a cell gets it by how squarely it faces the light, and cells in the shadow of others or facing away get only the ambient share.',
    switch: { name: 'lightAmount', off: 0, on: 0.7 },
    params: [
      { name: 'lightAmount', label: 'amount', min: 0, max: 1, value: 0, about: 'how much of a cell\'s food depends on the light; at 1 a cell without light starves' },
      { name: 'lightAmbient', label: 'ambient', min: 0, max: 0.5, value: 0.05, about: 'share of the light that reaches every cell' },
      { name: 'lightDirX', label: 'direction x', min: -1, max: 1, value: 0, about: 'towards the light: only the direction counts; not all three zero' },
      { name: 'lightDirY', label: 'direction y', min: -1, max: 1, value: 1 },
      { name: 'lightDirZ', label: 'direction z', min: -1, max: 1, value: 0 },
    ],
  },
  {
    title: 'Crowding',
    about: 'A cell whose links are squeezed short gets less food, so growth slows where the sheet cannot spread. On while "full food at" is above "no food at".',
    switch: { name: 'crowdHigh', off: 0, on: 1.0 },
    params: [
      // The model's default for crowdLow is 0. It is 0.8 here so that switching the group on
      // (crowdHigh = 1) gives a window like the search's (crowdLow 0.70 to 0.95, crowdHigh 0.05
      // to 0.30 above it). While crowdHigh is 0 the mechanism is off whatever crowdLow is, and
      // the results are those of crowdLow = 0.
      { name: 'crowdLow', label: 'no food at', min: 0, max: 1.2, value: 0.8, about: 'mean length of a cell\'s links, in rest lengths, at or below which it gets no food' },
      { name: 'crowdHigh', label: 'full food at', min: 0, max: 1.6, value: 0, about: 'mean length of its links at or above which it gets all its food; in between the food rises evenly' },
    ],
  },
  {
    title: 'Pulsed food',
    about: 'Food comes in pulses: full for a part of every period, less for the rest.',
    switch: { name: 'pulsePeriod', off: 0, on: 300 },
    params: [
      { name: 'pulsePeriod', label: 'period', min: 0, max: 1500, value: 0, step: 1, about: 'length of a period, in steps' },
      { name: 'pulseDuty', label: 'on share', min: 0.1, max: 0.9, value: 0.5, about: 'share of every period with full food' },
      { name: 'pulseLow', label: 'off level', min: 0, max: 0.6, value: 0, about: 'share of the food a cell gets in the rest of the period' },
    ],
  },
];

// Every parameter with its default, { name: value }. The ones without a control: the rest
// length of a link, the switch of the morphogens, and two settings of the light's shadows
// (lightBias: how far behind the cell nearest the light a cell still counts as lit, in rest
// lengths; lightPixel: the size of a texel of the shadow map, in rest lengths).
export const DEFAULTS = Object.fromEntries(GROUPS.flatMap((g) => g.params.map((p) => [p.name, p.value]))
  .concat([['linkRestLength', 1], ['morphogens', 0], ['lightBias', 0.75], ['lightPixel', 1]]));

// The start shapes of web/app/starts (starts.json lists the files). The two rough spheres are
// the shapes cf_run makes for seed 1: on the PC the roughness follows the seed of the run, so
// with another seed a run from these files is a different run from the PC's.
export const START_SHAPES = [
  { shape: 'icosphere:2', label: 'sphere, 162 cells' },
  { shape: 'icosphere:1', label: 'sphere, 42 cells' },
  { shape: 'icosphere:0', label: 'sphere, 12 cells' },
  { shape: 'icosphere:3', label: 'sphere, 642 cells' },
  { shape: 'icosphere:4', label: 'sphere, 2,562 cells' },
  { shape: 'ellipsoid:2:2:1:1', label: 'ellipsoid 2 : 1 : 1' },
  { shape: 'ellipsoid:2:3:1:1', label: 'ellipsoid 3 : 1 : 1' },
  { shape: 'ellipsoid:2:1:1:0.5', label: 'ellipsoid, flattened' },
  { shape: 'noisy:2:0.3:2', label: 'rough sphere, 162 cells' },
  { shape: 'noisy:3:0.3:2', label: 'rough sphere, 642 cells' },
];
