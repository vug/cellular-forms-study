// Small pieces the panel is built from: sections that fold, and rows with a slider and its
// number, a switch, a choice or a colour. Every row returns { root, set(value) }.

let nextId = 0;
const uid = (prefix) => `${prefix}-${++nextId}`;

export function el(tag, props = {}, ...children) {
  const node = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') node.className = v;
    else if (k === 'text') node.textContent = v;
    else if (k.startsWith('on')) node.addEventListener(k.slice(2), v);
    else if (k in node && k !== 'list') node[k] = v;
    else node.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children.flat()) if (c !== null && c !== undefined && c !== false) node.append(c);
  return node;
}

// A number as the panel shows it: four significant digits, no exponent.
export function formatNumber(v, integer = false) {
  if (!Number.isFinite(v)) return '';
  if (integer) return String(Math.round(v));
  const a = Math.abs(v);
  if (a !== 0 && a < 1e-4) return v.toFixed(7).replace(/0+$/, '');
  return String(+v.toPrecision(4));
}
export const formatCount = (n) => Math.round(n).toLocaleString('en-US');

export function section(title, { open = false, tag = '' } = {}) {
  const tagEl = el('span', { class: 'tag', text: tag });
  const body = el('div', { class: 'sec-body' });
  const root = el('details', { class: 'sec', open }, el('summary', {}, el('span', { text: title }), tagEl), body);
  return { root, body, setTag: (text) => { tagEl.textContent = text; } };
}

const hint = (about) => (about ? el('div', { class: 'hint', text: about }) : null);

// A focused number field changes its value when the wheel turns over it; here the wheel
// is for scrolling the panel.
const noWheel = (input) => input.addEventListener('wheel', () => input.blur(), { passive: true });

// A slider with its number. `log`: the slider moves on a logarithmic scale. A typed number is
// kept inside the slider's range; set() takes any value (a gallery form may bring one from
// outside) and shows it as it is.
export function sliderRow({ label, about, min, max, step, log = false, value, onInput }) {
  const id = uid('s');
  const integer = step === 1 && Number.isInteger(min);
  const useLog = log && min > 0;
  const STEPS = 400;
  const toSlider = (v) => {
    const c = Math.min(Math.max(v, min), max);
    return useLog ? (Math.log(c / min) / Math.log(max / min)) * STEPS : c;
  };
  const fromSlider = (t) => {
    if (!useLog) return integer ? Math.round(t) : +t.toPrecision(5);
    return +(min * Math.pow(max / min, t / STEPS)).toPrecision(4);
  };
  const range = el('input', {
    type: 'range', id, min: useLog ? 0 : min, max: useLog ? STEPS : max,
    step: useLog ? 1 : step ?? (max - min) / STEPS, 'aria-describedby': about ? id + '-h' : undefined,
  });
  const number = el('input', { type: 'number', step: 'any', inputMode: 'decimal', 'aria-label': label + ', number' });
  let current = value;
  const show = (v, fromRange) => {
    current = v;
    if (!fromRange) range.value = String(toSlider(v));
    number.value = formatNumber(v, integer);
  };
  range.addEventListener('input', () => {
    const v = fromSlider(Number(range.value));
    show(v, true);
    onInput(v);
  });
  number.addEventListener('change', () => {
    let v = Number(number.value);
    if (number.value.trim() === '' || !Number.isFinite(v)) return show(current);
    v = Math.min(Math.max(v, min), max);
    if (integer) v = Math.round(v);
    show(v);
    onInput(v);
  });
  noWheel(number);
  const h = hint(about);
  if (h) h.id = id + '-h';
  const root = el('div', { class: 'row', title: about }, el('label', { htmlFor: id, text: label }), range, number, h);
  show(value);
  return { root, set: (v) => show(v) };
}

export function checkRow({ label, about, value, onChange }) {
  const id = uid('c');
  const box = el('input', { type: 'checkbox', id, checked: !!value, onchange: () => onChange(box.checked) });
  const root = el('div', { class: 'row check', title: about }, box, el('label', { htmlFor: id, text: label }), hint(about));
  return { root, set: (v) => { box.checked = !!v; } };
}

// options: [[value, text], ...]
export function selectRow({ label, about, options, value, onChange }) {
  const id = uid('o');
  const select = el('select', { id, onchange: () => onChange(select.value) }, options.map(([v, text]) => el('option', { value: String(v), text })));
  select.value = String(value);
  const root = el('div', { class: 'row wide', title: about }, el('label', { htmlFor: id, text: label }), select, hint(about));
  return {
    root, select,
    set: (v) => { select.value = String(v); },
    setOptions: (list, v) => {
      select.replaceChildren(...list.map(([val, text]) => el('option', { value: String(val), text })));
      select.value = String(v);
    },
  };
}

// value: '#rrggbb'
export function colorRow({ label, about, value, onInput }) {
  const id = uid('k');
  const input = el('input', { type: 'color', id, value, oninput: () => onInput(input.value) });
  const root = el('div', { class: 'row wide', title: about }, el('label', { htmlFor: id, text: label }), el('div', {}, input), hint(about));
  return { root, set: (v) => { input.value = v; } };
}

export function numberRow({ label, about, value, min, max, onChange }) {
  const id = uid('n');
  const input = el('input', { type: 'number', id, min, max, step: 1, inputMode: 'numeric', value: String(value) });
  input.addEventListener('change', () => {
    const v = Math.round(Number(input.value));
    if (input.value.trim() === '' || !Number.isFinite(v)) return;
    onChange(Math.min(Math.max(v, min), max));
  });
  noWheel(input);
  const root = el('div', { class: 'row wide', title: about }, el('label', { htmlFor: id, text: label }), input, hint(about));
  input.style.textAlign = 'left';
  return { root, set: (v) => { input.value = String(v); } };
}

export function buttons(...list) {
  return el('div', { class: 'buttons' }, list);
}
export function button(text, onclick, props = {}) {
  return el('button', { type: 'button', text, onclick, ...props });
}
