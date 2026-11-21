// The gallery: forms found by the search, each with the picture of its tile in a contact sheet
// (gallery/gallery.json and the sheets beside it). Choosing one hands its settings to the viewer.
import { GROUPS } from './params.js';
import { el, formatCount, formatNumber } from './ui.js';

const base = new URL('gallery/', import.meta.url);

// The names the gallery uses for the optional mechanisms, and the group of params.js each is.
const SOURCE_SWITCH = { light: 'lightAmount', field: 'fieldAmount', diffusion: 'foodDiffusion', orient: 'orientAmount', morph: 'morphogens', crowd: 'crowdHigh', pulse: 'pulsePeriod' };
export function sourceTitle(source) {
  const group = GROUPS.find((g) => g.switch && g.switch.name === SOURCE_SWITCH[source]);
  return group ? group.title.toLowerCase() : source;
}
export const sourceGroup = (source) => GROUPS.find((g) => g.switch && g.switch.name === SOURCE_SWITCH[source]) ?? null;

let loading = null;
// { tile, columns, sheets, kinds, metrics, items, rows: [rows of each sheet] }
export function loadGallery() {
  loading ??= (async () => {
    const r = await fetch(new URL('gallery.json', base));
    if (!r.ok) throw new Error(`the gallery could not be loaded (${r.status})`);
    const data = await r.json();
    // Rows per sheet, from the tiles that are on it (the pictures are placed by percentages,
    // which need the size of the sheet in tiles).
    data.rows = data.sheets.map((_, s) => 1 + Math.max(0, ...data.items.filter((i) => i.tile[0] === s).map((i) => i.tile[2])));
    data.byId = new Map(data.items.map((i) => [i.id, i]));
    data.kindName = new Map(data.kinds.map((k) => [k.id, k.name]));
    return data;
  })();
  loading.catch(() => { loading = null; });
  return loading;
}

// CSS that shows the tile of an item in a box of any size with the tile's proportions.
export function tileStyle(data, item) {
  const [sheet, column, row] = item.tile;
  const cols = data.columns, rows = data.rows[sheet];
  const x = cols > 1 ? (column / (cols - 1)) * 100 : 0, y = rows > 1 ? (row / (rows - 1)) * 100 : 0;
  return {
    backgroundImage: `url("${new URL(data.sheets[sheet], base).href}")`,
    backgroundSize: `${cols * 100}% ${rows * 100}%`,
    backgroundPosition: `${x}% ${y}%`,
  };
}

const SORTS = [
  ['', 'as found'],
  ['structure', 'most structure'],
  ['area_ratio', 'most surface'],
  ['reach', 'reaching out furthest'],
  ['relief_meso', 'most folds'],
  ['growth_unevenness', 'most uneven growth'],
];

// Builds the gallery into `root`. onChoose(item) is called when a form is picked;
// missing(item) returns the mechanisms the item needs that the simulation here lacks.
export function buildGallery(root, data, { onChoose, missing }) {
  const state = { kind: '', sort: '', runnable: false, current: null };
  const count = el('span', { class: 'count' });
  const grid = el('div', { class: 'gallery-grid' });

  const kindSelect = el('select', { id: 'gallery-kind', onchange: () => { state.kind = kindSelect.value; fill(); } },
    el('option', { value: '', text: `all kinds (${data.items.length})` }),
    data.kinds.map((k) => el('option', { value: String(k.id), text: `${k.name} (${data.items.filter((i) => i.kind === k.id).length})` })));
  const sortSelect = el('select', { id: 'gallery-sort', onchange: () => { state.sort = sortSelect.value; fill(); } },
    SORTS.map(([v, text]) => el('option', { value: v, text })));
  const runnable = el('input', { type: 'checkbox', id: 'gallery-runnable', onchange: () => { state.runnable = runnable.checked; fill(); } });
  const runnableLabel = el('label', { htmlFor: 'gallery-runnable' }, runnable, 'only forms that can be grown here');

  function tile(item) {
    const pic = el('span', { class: 'pic', role: 'img', 'aria-label': `picture of form ${item.id}` });
    Object.assign(pic.style, tileStyle(data, item));
    const lacks = missing(item);
    const m = item.metrics ?? {};
    const numbers = [`${formatCount(item.cells)} cells`, `step ${formatCount(item.step)}`];
    if (m.structure !== undefined) numbers.push(`structure ${formatNumber(m.structure)}`);
    if (m.area_ratio !== undefined) numbers.push(`surface ${formatNumber(m.area_ratio)} x`);
    const title = Object.entries(m).map(([k, v]) => `${k.replace(/_/g, ' ')} ${formatNumber(v)}: ${data.metrics[k] ?? ''}`).join('\n');
    return el('button', {
      type: 'button', class: 'tile' + (item.id === state.current ? ' current' : ''), title, 'data-id': item.id,
      onclick: () => onChoose(item),
    }, pic, el('span', { class: 'text' },
      el('span', { class: 'name', text: `Form ${item.id}` }),
      el('span', { class: 'kind', text: data.kindName.get(item.kind) ?? '' }),
      el('span', { class: 'numbers', text: numbers.join(' · ') }),
      item.sources.length ? el('span', { class: 'chips' }, item.sources.map((s) => el('span', {
        class: 'chip' + (lacks.includes(s) ? ' missing' : ''), text: sourceTitle(s), title: lacks.includes(s) ? 'not in the simulation of this page yet' : undefined,
      }))) : null));
  }

  function fill() {
    let items = data.items.filter((i) => (!state.kind || String(i.kind) === state.kind) && (!state.runnable || missing(i).length === 0));
    if (state.sort) items = [...items].sort((a, b) => (b.metrics?.[state.sort] ?? -Infinity) - (a.metrics?.[state.sort] ?? -Infinity));
    count.textContent = `${items.length} of ${data.items.length} forms`;
    grid.replaceChildren(...items.map(tile));
    if (!items.length) grid.append(el('p', { class: 'gallery-intro', text: 'No form matches.' }));
  }

  root.replaceChildren(
    el('p', { class: 'gallery-intro', text: 'Forms found by a search over the parameters of the model, pictured at about 31,600 cells. ' +
      'Choose one to grow it in the viewer with its start shape, seed and parameters.' }),
    el('div', { class: 'gallery-tools' },
      el('label', { htmlFor: 'gallery-kind' }, 'kind', kindSelect),
      el('label', { htmlFor: 'gallery-sort' }, 'order', sortSelect),
      runnableLabel, count),
    grid);
  fill();

  return {
    refresh: fill,
    setCurrent: (id) => {
      state.current = id;
      for (const t of grid.querySelectorAll('.tile')) t.classList.toggle('current', Number(t.dataset.id) === id);
    },
    // Hides the filter that only makes sense while some mechanisms are missing.
    setRunnableFilter: (visible) => {
      runnableLabel.hidden = !visible;
      if (!visible && state.runnable) {
        state.runnable = false;
        runnable.checked = false;
        fill();
      }
    },
  };
}
