// Theme of the Boot Android screen (W2): the name, subtitle, logo, colours, stage labels and the boot
// animation (W3). The default is web/theme.json; ?theme=<name> loads web/theme-<name>.json. A theme that does
// not validate is replaced by the built-in default (the same values as theme.json), and loadTheme says why in
// `note`.
//
// The stage mapping: stages[k].from is the milestone (an id of BOOT.milestones) that starts stage k+1 of
// the screen. "boot" starts the first stage when the engine starts; null is a stage the boot does not reach
// yet. The ids must come in the boot's order, so the screen only moves forward.
import { BOOT } from './cases.js';
import { ANIM_DEFAULT } from './bootscreen.js';

export const DEFAULT_THEME = {
  name: 'Xiao',
  accentPart: 'Ji',
  subtitle: 'Android 13',
  logo: { text: '小机' },
  colors: { bg1: '#0b0f14', bg2: '#121a24', ink: '#e8eef5', dim: '#7d8b9b', accent: '#3ddc84' },
  stages: [
    { label: 'Starting runtime', from: 'boot' },
    { label: 'Loading framework', from: 'art' },
    { label: 'Starting system server', from: 'forked' },
    { label: 'Starting services', from: 'phase100' },
    { label: 'Starting home', from: 'completed' },
  ],
  animation: { ...ANIM_DEFAULT },
};

const COLOURS = ['bg1', 'bg2', 'ink', 'dim', 'accent'];
const HEX = /^#[0-9a-fA-F]{6}$/;
const STAGES = 5;

function text(v, min, max, what) {
  if (typeof v !== 'string') throw new Error(`${what}: not a string`);
  const n = [...v].length;
  if (n < min || n > max) throw new Error(`${what}: ${n} characters, want ${min} to ${max}`);
  if (/[\u0000-\u001f\u007f]/.test(v)) throw new Error(`${what}: control characters`);
  return v;
}

// Returns the theme with every field checked; throws an Error naming the first field that is wrong.
// base: the URL that a relative logo.image is resolved against (it must be http or https).
export function validateTheme(raw, base) {
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new Error('theme: not an object');
  const name = text(raw.name, 1, 24, 'name');
  const accentPart = raw.accentPart === undefined ? '' : text(raw.accentPart, 0, 24, 'accentPart');
  const subtitle = text(raw.subtitle, 0, 40, 'subtitle');

  let logo;
  if (raw.logo && typeof raw.logo === 'object' && 'text' in raw.logo) {
    logo = { text: text(raw.logo.text, 1, 4, 'logo.text') };
  } else if (raw.logo && typeof raw.logo === 'object' && 'image' in raw.logo) {
    const src = text(raw.logo.image, 1, 2000, 'logo.image');
    let proto;
    try {
      proto = new URL(src, base).protocol;
    } catch {
      throw new Error('logo.image: not a URL');
    }
    if (proto !== 'http:' && proto !== 'https:') throw new Error('logo.image: want an http or https URL');
    logo = { image: src };
  } else {
    throw new Error('logo: want { text } or { image }');
  }

  if (!raw.colors || typeof raw.colors !== 'object') throw new Error('colors: not an object');
  const colors = {};
  for (const k of COLOURS) {
    const v = raw.colors[k];
    if (typeof v !== 'string' || !HEX.test(v)) throw new Error(`colors.${k}: want #rrggbb`);
    colors[k] = v;
  }

  if (!Array.isArray(raw.stages) || raw.stages.length !== STAGES) throw new Error(`stages: want ${STAGES}`);
  const order = ['boot', ...BOOT.milestones.map((m) => m.id)];
  let last = -1;
  const stages = raw.stages.map((st, i) => {
    if (!st || typeof st !== 'object') throw new Error(`stages[${i}]: not an object`);
    const label = text(st.label, 1, 40, `stages[${i}].label`);
    if (st.from === null) return { label, from: null };
    if (typeof st.from !== 'string' || !order.includes(st.from)) {
      throw new Error(`stages[${i}].from: unknown milestone ${JSON.stringify(st.from)}`);
    }
    const at = order.indexOf(st.from);
    if (at <= last) throw new Error(`stages[${i}].from: ${st.from} is not after the previous stage`);
    last = at;
    return { label, from: st.from };
  });
  if (stages[0].from !== 'boot') throw new Error('stages[0].from: want "boot"');
  return { name, accentPart, subtitle, logo, colors, stages, animation: validateAnimation(raw.animation) };
}

// The boot animation (W3, web/bootscreen.js): optional; each field that is left out takes its default.
const ANIM_MS = { introMs: [0, 5000], outroMs: [0, 2000], spinMs: [400, 10000], breatheMs: [1000, 20000] };

function validateAnimation(raw) {
  const a = { ...ANIM_DEFAULT };
  if (raw === undefined) return a;
  if (!raw || typeof raw !== 'object' || Array.isArray(raw)) throw new Error('animation: not an object');
  if (raw.style !== undefined) {
    if (raw.style !== 'default' && raw.style !== 'none') throw new Error('animation.style: want "default" or "none"');
    a.style = raw.style;
  }
  for (const [k, [lo, hi]] of Object.entries(ANIM_MS)) {
    if (raw[k] === undefined) continue;
    if (!Number.isInteger(raw[k]) || raw[k] < lo || raw[k] > hi) throw new Error(`animation.${k}: want an integer from ${lo} to ${hi}`);
    a[k] = raw[k];
  }
  if (raw.glow !== undefined) {
    if (typeof raw.glow !== 'boolean') throw new Error('animation.glow: want true or false');
    a.glow = raw.glow;
  }
  return a;
}

// Loads the theme for ?theme=<name> (name null: the default). Returns { theme, source, note }: note is null
// when the file is used, else the reason the default is used instead.
export async function loadTheme(name) {
  if (name !== null && !/^[a-z0-9][a-z0-9-]{0,31}$/.test(name)) {
    return { theme: DEFAULT_THEME, source: 'default', note: `theme "${name}": use lowercase letters, digits and dashes` };
  }
  const file = name === null ? 'theme.json' : `theme-${name}.json`;
  const source = name === null ? 'default' : name;
  try {
    const res = await fetch(new URL(file, location.href), { cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    const theme = validateTheme(await res.json(), location.href);
    return { theme, source, note: null };
  } catch (e) {
    return { theme: DEFAULT_THEME, source: 'default', note: `theme ${source}: ${e.message}; using the default` };
  }
}
