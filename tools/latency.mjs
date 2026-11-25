#!/usr/bin/env node
// The demo's yardstick (docs/demo.md): what a visitor feels after pressing Boot Android, measured in headless Chromium
// (Playwright, as tools/snapshot-web.mjs) on the page's booted snapshot.
//
//   node tools/latency.mjs [--runs N] [--xj DIR] [--label NAME] [--idle S] [--calc] [--warm] [--json FILE] [--query Q]
//                          [--fetch-log FILE]
//   node tools/latency.mjs --summary A.json,B.json,... [--label NAME]     the table of runs written before (--json)
//
// Each run is a new browser (a fresh profile: nothing cached), which restores the snapshot and then:
//   press      Boot Android to the first Android frame on the page
//   idle       the next --idle seconds (default 30) untouched: the engine's load reports (?load=1000, --load-every):
//              guest instructions per second, the share of time the engine was busy, processes and threads, and the
//              processes that ran the most
//   drawer     a swipe up on the home screen to the all-apps drawer
//   settings   a tap on Settings in the drawer to Settings on screen (and when the log's START and "Displayed" lines
//              arrive; the "+T" of "Displayed" is kept in the JSON only: after a restore it can count from a
//              launch that began before the snapshot (in the first runs, the launcher's HOME start, whose transition
//              had not ended when the snapshot was taken), so it is not the tap's)
//   home       the Home button back to the home screen
//   --warm     then the drawer and Settings once more (Settings' second start)
//   --calc     then Calculator from the drawer, and 1 + 2 = (the screenshot shows the result)
// For each action: "response", from the input (the swipe's first touch, the tap's release, the button's click) to the
// first frame that differs from the frame before the input by more than 1% of the sampled pixels; and "settled", from
// the input's end to the last frame that changed the picture (more than 0.2%) before 5 s without change. The frames
// are timed where the page receives them (web/main.js, xb.onFrame), the input where the page sends it.
// --xj DIR serves another chunk store (a frozen tree and its snapshot, for before/after runs on one engine build).
// --query adds page parameters. Output: a table of medians and ranges, the runs as JSON (--json), screenshots
// build/web-shots/l1-<label>-*.png of the first run.
//
// Playwright comes from XN_PLAYWRIGHT (default: the xiaonux bench's playwright-core), XN_CHROME picks a browser.
import { execFileSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { DIRS, startServer } from '../web/serve.mjs';
import { drawerLayout } from './ui-points.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PW = process.env.XN_PLAYWRIGHT ?? '/Users/rosdyanakusuma/Ross/projects/xiaonux/bench/node_modules/playwright-core/index.mjs';
const SHOTS = join(ROOT, 'build', 'web-shots');
const VIEW = { width: 1280, height: 1000 };

const args = process.argv.slice(2);
const opt = (name, dflt) => {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : dflt;
};
const RUNS = Number(opt('--runs', 3));
const IDLE_S = Number(opt('--idle', 30));
const LABEL = opt('--label', 'run');
const XJ = opt('--xj', null);
const JSON_OUT = opt('--json', null);
const QUERY = opt('--query', '');
const WARM = args.includes('--warm');
const CALC = args.includes('--calc');
// --summary A.json,B.json: no runs; the table of the runs these files hold (several --runs 1 invocations, one table).
const SUMMARY = opt('--summary', null);
// --fetch-log FILE: every request the server answers, with its time (which chunks of which files a step reads).
const FETCH_LOG = opt('--fetch-log', null);
if (XJ) DIRS.xj = resolve(XJ);

// Calculator's keys (ExactCalculator, portrait 540x960).
const CALC_KEYS = { 1: [70, 715], 2: [189, 715], plus: [440, 865], eq: [307, 858] };

const median = (xs) => {
  const a = xs.filter((x) => x != null && !Number.isNaN(x)).sort((p, q) => p - q);
  if (!a.length) return null;
  return a.length % 2 ? a[(a.length - 1) / 2] : (a[a.length / 2 - 1] + a[a.length / 2]) / 2;
};

function power() {
  const sh = (cmd, a) => {
    try {
      return execFileSync(cmd, a, { encoding: 'utf8' });
    } catch {
      return '';
    }
  };
  const pm = sh('pmset', ['-g']);
  const batt = sh('pmset', ['-g', 'batt']);
  const swap = sh('sysctl', ['-n', 'vm.swapusage']);
  return {
    lowPower: (/lowpowermode\s+(\d)/.exec(pm) || [])[1] ?? '?',
    source: /AC Power/.test(batt) ? 'AC' : /Battery Power/.test(batt) ? 'battery' : '?',
    swapUsed: (/used = ([\d.]+M)/.exec(swap) || [])[1] ?? '?',
  };
}

// In the page: frame signatures (a 15-bit colour every 6th pixel of every 6th row), the input's times, the log lines'.
function installHooks() {
  const L = (window.__lat = { frames: [], input: [], lines: [], boot: 0, prev: null, ref: null });
  document.getElementById('boot').addEventListener('click', () => (L.boot = performance.now()), true);
  const scr = document.getElementById('android-screen');
  for (const type of ['pointerdown', 'pointerup']) scr.addEventListener(type, () => L.input.push({ type, t: performance.now() }), true);
  for (const id of ['btn-home', 'btn-back']) {
    document.getElementById(id).addEventListener('click', () => L.input.push({ type: id, t: performance.now() }), true);
  }
  const diff = (a, b) => {
    if (!a || !b || a.length !== b.length) return 1;
    let d = 0;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) d++;
    return d / a.length;
  };
  window.xbHooks = window.xbHooks || {};
  window.xbHooks.onFrame = (m, now) => {
    const px = new Uint8Array(m.px);
    const w = m.w, h = m.h, cols = Math.floor(w / 6), rows = Math.floor(h / 6);
    const sig = new Uint16Array(cols * rows);
    let k = 0;
    for (let y = 0; y < rows; y++) {
      for (let x = 0; x < cols; x++) {
        const o = (y * 6 * w + x * 6) * 4;
        sig[k++] = ((px[o] >> 3) << 10) | ((px[o + 1] >> 3) << 5) | (px[o + 2] >> 3);
      }
    }
    const seen = new Set();
    for (let i = 0; i < sig.length && seen.size < 255; i += 7) seen.add(sig[i]);
    L.frames.push({ t: now, diff: diff(sig, L.prev), diffRef: L.ref ? diff(sig, L.ref) : null, colors: seen.size });
    L.prev = sig;
  };
  window.xbHooks.onLine = (text, now) => {
    if (/^xiaoji: load: |ActivityTaskManager\(\d+\): (START u0|Displayed)|ActivityManager\(\d+\): (Start proc|Killing)|ANR in|FATAL EXCEPTION|Fatal signal/.test(text)) {
      L.lines.push({ t: now, text });
    }
  };
}

async function gesture(page, points, stepMs) {
  const box = await page.evaluate(() => {
    const r = document.getElementById('android-screen').getBoundingClientRect();
    const s = Math.min(r.width / 540, r.height / 960);
    return { left: r.left + (r.width - 540 * s) / 2, top: r.top + (r.height - 960 * s) / 2, s };
  });
  const at = ([x, y]) => [box.left + x * box.s, box.top + y * box.s];
  const [x0, y0] = at(points[0]);
  await page.mouse.move(x0, y0);
  await page.mouse.down();
  for (const p of points.slice(1)) {
    const [x, y] = at(p);
    await page.mouse.move(x, y);
    await page.waitForTimeout(stepMs);
  }
  await page.mouse.up();
}
const tap = (page, p) => gesture(page, [p, p], 80);
// An app's start can leave the screen unchanged for many seconds (its splash screen while the app's process works),
// so an app launch is settled only after this long without a change.
const APP_QUIET_MS = Number(process.env.XN_APP_QUIET_MS ?? 30000);
const SWIPE = Array.from({ length: 15 }, (_, k) => [270, 860 - k * 45]);

// One action: mark, act, wait until the picture has settled (and the log line, if one is expected: an app's START,
// which tells a tap that missed from a slow start; "Displayed" is not waited for, an app's first start after a restore
// can log none), then measure.
async function action(page, name, act, { log = null, timeoutMs = 120000, quietMs = 5000 } = {}) {
  const mark = await page.evaluate(() => {
    const L = window.__lat;
    L.ref = L.prev;
    return { frames: L.frames.length, input: L.input.length, lines: L.lines.length };
  });
  await act();
  const t0 = Date.now();
  let r;
  for (;;) {
    await page.waitForTimeout(250);
    r = await page.evaluate((mk) => {
      const L = window.__lat;
      return { frames: L.frames.slice(mk.frames), input: L.input.slice(mk.input), lines: L.lines.slice(mk.lines), now: performance.now() };
    }, mark);
    const sig = r.frames.filter((f) => f.diff > 0.002);
    const last = sig.length ? sig[sig.length - 1].t : null;
    const logOk = !log || r.lines.some((l) => log.test(l.text));
    if (logOk && last != null && r.now - last >= quietMs) break;
    if (Date.now() - t0 > timeoutMs) break;
  }
  const inputs = r.input;
  const down = inputs.find((e) => e.type === 'pointerdown' || e.type.startsWith('btn-'));
  const up = [...inputs].reverse().find((e) => e.type === 'pointerup' || e.type.startsWith('btn-'));
  const from = name === 'drawer' ? down : up; // a swipe responds while it moves; a tap or a button on release
  const first = r.frames.find((f) => f.diffRef != null && f.diffRef > 0.01 && from && f.t >= from.t);
  const sig = r.frames.filter((f) => f.diff > 0.002 && up && f.t >= up.t);
  const lastF = r.frames.length ? r.frames[r.frames.length - 1] : null;
  const out = {
    name,
    responseMs: first && from ? Math.round(first.t - from.t) : null,
    settledMs: sig.length && up ? Math.round(sig[sig.length - 1].t - up.t) : null,
    frames: r.frames.length,
    changed: lastF && lastF.diffRef != null ? Number(lastF.diffRef.toFixed(3)) : null, // how much of the picture changed
    timedOut: Date.now() - t0 > timeoutMs,
  };
  const start = r.lines.find((l) => /START u0 /.test(l.text));
  if (start && up) out.startMs = Math.round(start.t - up.t);
  const disp = r.lines.find((l) => /Displayed /.test(l.text));
  if (disp) {
    const m = /Displayed (\S+): \+(?:(\d+)m)?(?:(\d+)s)?(?:(\d+)ms)?/.exec(disp.text);
    if (m) {
      out.displayed = m[1];
      out.displayedMs = (Number(m[2] || 0) * 60 + Number(m[3] || 0)) * 1000 + Number(m[4] || 0);
    }
    if (up) out.displayedAtMs = Math.round(disp.t - up.t);
  }
  // The frames after the input: ms from the input's end, the change from the frame before, the colours (a splash
  // screen has few, a screen of text many).
  out.timeline = r.frames.filter((f) => up && f.t >= up.t).slice(0, 300)
    .map((f) => [Math.round(f.t - up.t), Number(f.diff.toFixed(3)), f.colors]);
  out.procStarts = r.lines.filter((l) => /Start proc/.test(l.text)).map((l) => l.text.replace(/^.*Start proc \d+:/, '').slice(0, 80));
  return out;
}

// The drawer must open (most of the picture changes); a swipe that did not open it (on a loaded machine it can read as
// a long press, whose menu Home closes) is counted and tried again, up to three times.
async function openDrawer(page) {
  let drawer, tries = 0;
  do {
    if (tries) {
      await page.click('#btn-home');
      await page.waitForTimeout(5000);
    }
    tries++;
    drawer = await action(page, 'drawer', () => gesture(page, SWIPE, 20), { timeoutMs: 90000 });
  } while ((drawer.changed == null || drawer.changed < 0.5) && tries < 3);
  return { ...drawer, tries };
}

// The load reports of an interval: the means, the last counts, the processes by instructions.
const LOAD = /^xiaoji: load: (\d+) ms, ([\d.]+) M insn \(([\d.]+) M\/s\), busy (\d+)%, (\d+) processes, (\d+) threads \((\d+) runnable\); top:(.*); threads:(.*)$/;
function loadSummary(lines) {
  const reps = lines.map((l) => LOAD.exec(l.text)).filter(Boolean);
  if (!reps.length) return null;
  let ms = 0, insn = 0, busyMs = 0;
  const byProc = new Map();
  for (const m of reps) {
    const d = Number(m[1]), n = Number(m[2]);
    ms += d;
    insn += n;
    busyMs += (d * Number(m[4])) / 100;
    for (const part of m[8].split(',')) {
      const p = /^\s*(.+)\/(\d+) (\d+)%$/.exec(part);
      if (p) byProc.set(p[1], (byProc.get(p[1]) || 0) + (n * Number(p[3])) / 100);
    }
  }
  const last = reps[reps.length - 1];
  return {
    seconds: Number((ms / 1000).toFixed(1)),
    mInsnPerS: Number((insn / (ms / 1000)).toFixed(1)),
    busyPct: Math.round((100 * busyMs) / ms),
    processes: Number(last[5]),
    threads: Number(last[6]),
    top: [...byProc].sort((a, b) => b[1] - a[1]).slice(0, 6).map(([k, v]) => `${k} ${Math.round((100 * v) / insn)}%`),
  };
}

async function oneRun(chromium, srv, k, ui) {
  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  const shots = k === 1;
  const shot = async (page, what) => {
    if (shots) await page.locator('#phone').screenshot({ path: join(SHOTS, `l1-${LABEL}-${what}.png`) });
  };
  try {
    const ctx = await browser.newContext({ viewport: VIEW });
    const page = await ctx.newPage();
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await page.goto(srv.url + '?keeplog=1&load=1000' + QUERY, { waitUntil: 'load' });
    await page.evaluate(installHooks);
    await page.click('#boot');
    await page.waitForFunction(() => window.xb.restore && /loaded|refused/.test(window.xb.restore.state) || window.xb.error, null,
      { timeout: 600000, polling: 100 });
    const st = await page.evaluate(() => ({ restore: window.xb.restore, error: window.xb.error }));
    if (!st.restore || st.restore.state !== 'loaded') throw new Error(`no restore: ${JSON.stringify(st.restore)} ${st.error || ''}`);
    await page.waitForFunction(() => window.__lat.frames.length > 0, null, { timeout: 300000, polling: 50 });
    const r0 = await page.evaluate(() => ({ boot: window.__lat.boot, first: window.__lat.frames[0].t, restore: window.xb.restore }));
    const run = {
      run: k,
      pressToFrameMs: Math.round(r0.first - r0.boot),
      downloadMiB: Number((r0.restore.bytes / 1048576).toFixed(1)),
      loadMs: Math.round(r0.restore.loadMs),
    };
    // The idle window: from the first frame, IDLE_S untouched.
    const idleFrom = r0.first;
    await page.waitForTimeout(IDLE_S * 1000);
    await shot(page, 'home');
    const lines = await page.evaluate(() => window.__lat.lines);
    run.idle = loadSummary(lines.filter((l) => l.t >= idleFrom));
    run.actions = [];
    run.actions.push(await openDrawer(page));
    await shot(page, 'drawer');
    run.actions.push(await action(page, 'settings', () => tap(page, ui.settings),
      { log: /START u0 .*cmp=com\.android\.settings\//, timeoutMs: 300000, quietMs: APP_QUIET_MS }));
    await shot(page, 'settings');
    run.actions.push(await action(page, 'home', () => page.click('#btn-home'), { timeoutMs: 90000 }));
    await shot(page, 'home-after');
    if (WARM) {
      run.actions.push({ ...(await openDrawer(page)), name: 'drawer2' });
      const s = await action(page, 'settings', () => tap(page, ui.settings), { log: /START u0 .*cmp=com\.android\.settings\//, timeoutMs: 300000, quietMs: APP_QUIET_MS });
      run.actions.push({ ...s, name: 'settings2' });
      run.actions.push({ ...(await action(page, 'home', () => page.click('#btn-home'), { timeoutMs: 90000 })), name: 'home2' });
    }
    if (CALC && ui.calculator) {
      await openDrawer(page);
      const c = await action(page, 'calculator', () => tap(page, ui.calculator), { log: /START u0 .*cmp=com\.android\.calculator2\//, timeoutMs: 300000, quietMs: APP_QUIET_MS });
      run.actions.push(c);
      await shot(page, 'calculator');
      for (const key of ['1', 'plus', '2', 'eq']) {
        run.actions.push({ ...(await action(page, 'key', () => tap(page, CALC_KEYS[key]), { timeoutMs: 60000, quietMs: 3000 })), name: `key ${key}` });
      }
      await shot(page, 'calc-result');
    }
    const all = await page.evaluate(() => window.__lat.lines.map((l) => l.text));
    run.bad = all.filter((l) => /ANR in|FATAL EXCEPTION|Fatal signal/.test(l)).map((l) => l.slice(0, 160));
    run.pageErrors = pageErrors;
    if (shots) writeFileSync(join(SHOTS, `l1-${LABEL}.log`), (await page.evaluate(() => (window.xb.all || []).join('\n'))) + '\n');
    return run;
  } finally {
    await browser.close();
  }
}

const fmt = (xs, unit = 's', div = 1000) => {
  const v = xs.filter((x) => x != null);
  if (!v.length) return '-';
  const m = median(v), lo = Math.min(...v), hi = Math.max(...v);
  const f = (x) => (div === 1 ? String(x) : (x / div).toFixed(1));
  return v.length > 1 ? `${f(m)} ${unit} (${f(lo)} to ${f(hi)})` : `${f(m)} ${unit}`;
};


// The table: medians and ranges over the runs. A swipe that did not open the drawer and a tap that started nothing
// (no START line) are left out and counted; the first Settings start is the first tap that started it (the --warm
// one when the first tap missed), and the second start is the --warm one after a first that worked.
function table(runs, label, idleS) {
  const ok = runs.filter((r) => !r.error);
  const acts = (r, n) => r.actions.filter((a) => a.name === n);
  const firstSettings = (r) => {
    for (const n of ['settings', 'settings2']) for (const a of acts(r, n)) if (a.startMs != null) return [a, n];
    return [null, null];
  };
  const secondSettings = (r) => {
    const [, n] = firstSettings(r);
    return n === 'settings' ? acts(r, 'settings2').find((a) => a.startMs != null) || null : null;
  };
  const drawers = ok.map((r) => acts(r, 'drawer')[0]).filter(Boolean);
  const opened = drawers.filter((a) => a.changed != null && a.changed >= 0.5);
  const missedSwipes = drawers.reduce((n, a) => n + ((a.tries || 1) - 1) + (a.changed != null && a.changed >= 0.5 ? 0 : 1), 0);
  const fs = ok.map((r) => firstSettings(r)[0]).filter(Boolean);
  const missedTaps = ok.filter((r) => firstSettings(r)[1] !== 'settings').length;
  const ws = ok.map(secondSettings).filter(Boolean);
  const homes = ok.flatMap((r) => acts(r, 'home')).filter((a) => a.responseMs != null);
  const calcs = ok.flatMap((r) => acts(r, 'calculator')).filter((a) => a.startMs != null);
  const keys = ok.flatMap((r) => r.actions.filter((a) => a.name.startsWith('key') && a.responseMs != null));
  const pair = (xs, k1, k2) => `${fmt(xs.map((a) => a[k1]))} / ${fmt(xs.map((a) => a[k2]))}`;
  const rows = [
    ['press to first frame', fmt(ok.map((r) => r.pressToFrameMs))],
    ['snapshot download', `${ok.length ? ok[0].downloadMiB : '-'} MiB`],
    [`idle ${idleS} s: engine busy`, fmt(ok.map((r) => r.idle && r.idle.busyPct), '%', 1)],
    [`idle ${idleS} s: guest instructions`, fmt(ok.map((r) => r.idle && r.idle.mInsnPerS), 'M/s', 1)],
    ['processes / threads', [...new Set(ok.filter((r) => r.idle).map((r) => `${r.idle.processes} / ${r.idle.threads}`))].join(', ') || '-'],
    ['drawer: response / settled', pair(opened, 'responseMs', 'settledMs')],
    ['first Settings start: response / settled', pair(fs, 'responseMs', 'settledMs')],
    ['first Settings start: START / Displayed in the log', `${fmt(fs.map((a) => a.startMs))} / ${fmt(fs.map((a) => a.displayedAtMs))}`],
    ['second Settings start: response / settled', ws.length ? pair(ws, 'responseMs', 'settledMs') : '-'],
    ['home: response / settled', pair(homes, 'responseMs', 'settledMs')],
  ];
  if (calcs.length) rows.push(['Calculator start: response / settled', pair(calcs, 'responseMs', 'settledMs')]);
  if (keys.length) rows.push(['Calculator key: response', fmt(keys.map((a) => a.responseMs))]);
  rows.push(['missed: swipes / Settings taps', `${missedSwipes} / ${missedTaps}`]);
  console.log(`\n| ${label} (${ok.length} of ${runs.length} runs; median, range) | |\n|---|---|`);
  for (const [k, v] of rows) console.log(`| ${k} | ${v} |`);
  if (ok.length && ok[0].idle) console.log(`\nidle, busiest processes (first run): ${ok[0].idle.top.join(', ')}`);
  const bad = [...new Set(ok.flatMap((r) => r.bad.map((b) => b.replace(/^\[[^\]]*\] /, '').slice(0, 80))))];
  if (bad.length) console.log(`in the log: ${bad.join('; ')}`);
  return ok.length === runs.length;
}

if (SUMMARY) {
  const runs = [];
  let idleS = IDLE_S;
  for (const f of SUMMARY.split(',')) {
    const d = JSON.parse(readFileSync(f, 'utf8'));
    console.log(`${f}: ${d.label}, power ${d.env.source}, low power mode ${d.env.lowPower}, swap used ${d.env.swapUsed}`);
    runs.push(...d.runs);
  }
  table(runs, LABEL, idleS);
  process.exit(0);
}

mkdirSync(SHOTS, { recursive: true });
const { chromium } = await import(pathToFileURL(PW).href);
const ui = drawerLayout(DIRS.xj);
const fetchLines = [];
const srv = await startServer({ port: 0, ...(FETCH_LOG ? { log: (l) => fetchLines.push(`${Date.now()} ${l}`) } : {}) });
const env = power();
console.log(`latency: ${LABEL}, ${RUNS} runs, chunk store ${DIRS.xj} (${ui.name} drawer), idle ${IDLE_S} s; ` +
  `power ${env.source}, low power mode ${env.lowPower}, swap used ${env.swapUsed}`);
const runs = [];
try {
  for (let k = 1; k <= RUNS; k++) {
    try {
      const r = await oneRun(chromium, srv, k, ui);
      runs.push(r);
      const a = Object.fromEntries(r.actions.map((x) => [x.name, x]));
      console.log(`  run ${k}: press->frame ${(r.pressToFrameMs / 1000).toFixed(1)} s; idle busy ${r.idle ? r.idle.busyPct : '-'}%, ` +
        `${r.idle ? r.idle.mInsnPerS : '-'} M insn/s; drawer ${a.drawer && a.drawer.responseMs}/${a.drawer && a.drawer.settledMs} ms; ` +
        `settings ${a.settings && a.settings.responseMs}/${a.settings && a.settings.settledMs} ms (START ${a.settings && a.settings.startMs} ms); ` +
        `home ${a.home && a.home.responseMs}/${a.home && a.home.settledMs} ms${r.bad.length ? '; ' + r.bad[0] : ''}`);
    } catch (e) {
      console.log(`  run ${k}: FAILED ${String(e).split('\n')[0]}`);
      runs.push({ run: k, error: String(e).split('\n')[0] });
    }
  }
} finally {
  await srv.close();
}
if (FETCH_LOG) writeFileSync(FETCH_LOG, fetchLines.join('\n') + '\n');
if (JSON_OUT) writeFileSync(JSON_OUT, JSON.stringify({ label: LABEL, env, ui, runs }, null, 1) + '\n');
process.exit(table(runs, LABEL, IDLE_S) ? 0 : 1);
