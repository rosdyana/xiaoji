#!/usr/bin/env node
// The booted snapshot for the page (docs/snapshot.md, S3 and S4), in headless Chromium (Playwright, as tests/web.mjs).
//
//   node tools/snapshot-web.mjs make      boots the product tree in the page with ?snapshot=make: the web engine writes
//                                         a snapshot at the home screen (the launcher's HOME start + 480 s and a quiet
//                                         window, below) to the dev server (build/snap/web.snap), then
//                                         tools/snappack.py packs it into the chunk store (build/web/xj/snapshot.json)
//   node tools/snapshot-web.mjs record    restores the snapshot with every lazy page on demand (?snaprecord=1) and
//                                         records the first touch of each page while the phone idles and the demo
//                                         apps open (build/snap/touch.json), for tools/snappack.py --order
//                                         (docs/snapshot.md, S5); make runs it and packs again
//   node tools/snapshot-web.mjs fallback  the page boots instead when the snapshot is another build's, or missing
//   node tools/snapshot-web.mjs restore   opens the page, which restores the snapshot, and checks it: the first frame
//                                         after the restore is the launcher (not one colour), a swipe up and a tap on
//                                         Settings open Settings (its START in the log); it prints the timings and the
//                                         download, and saves build/web-shots/s1-*.png
//
// Playwright comes from XN_PLAYWRIGHT (default: the xiaonux bench's playwright-core), XN_CHROME picks a browser.
import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { DIRS, startServer } from '../web/serve.mjs';
import { drawerLayout } from './ui-points.mjs';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let BROWSER_EXE = null; // set once Playwright is loaded
const PW = process.env.XN_PLAYWRIGHT ?? '/Users/rosdyanakusuma/Ross/projects/xiaonux/bench/node_modules/playwright-core/index.mjs';
const SHOTS = join(ROOT, 'build', 'web-shots');
const VIEW = { width: 1280, height: 1000 };
const MAKE_TIMEOUT_MS = Number(process.env.XN_SNAP_TIMEOUT_MS ?? 1800000);
const SETTINGS = /START u0 .*cmp=com\.android\.settings\/\.Settings/;

let failures = 0;
function check(ok, what) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures++;
}

// A crash of the page or the browser says so, with the browser's processes' resident sets (MiB): the restore of a
// large snapshot used to end with only "Target page, context or browser has been closed".
function browserRss() {
  try {
    const out = execFileSync('ps', ['-A', '-ww', '-o', 'rss=,command='], { encoding: 'utf8' });
    return out.split('\n').filter((l) => BROWSER_EXE && l.includes(BROWSER_EXE))
      .map((l) => Math.round(Number(l.trim().split(/\s+/)[0]) / 1024)).filter((n) => n > 0).sort((a, b) => b - a);
  } catch {
    return [];
  }
}
function watch(page, browser, what) {
  page.on('crash', () => console.log(`    ${what}: the page crashed (browser RSS MiB before: ${watch.last.join(', ')})`));
  browser.on('disconnected', () => console.log(`    ${what}: the browser disconnected (RSS MiB before: ${watch.last.join(', ')})`));
  const t = setInterval(() => {
    watch.last = browserRss();
    watch.peak = Math.max(watch.peak, watch.last.reduce((a, b) => a + b, 0));
  }, 2000);
  t.unref();
}
watch.last = [];
watch.peak = 0;

async function launch(chromium) {
  return chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
}

// The demo apps a snapshot is pre-warmed with (docs/demo.md), in order: the drawer's icon (tools/ui-points.mjs) and the
// component of its START line. Settings is last: it gets the longest warm time before the framework's cached-app freezer
// (which the engine does not support) kills cached processes, about 10 guest minutes after they leave the screen.
const PREWARM = [
  { key: 'files', component: 'com.android.documentsui/' },
  { key: 'calculator', component: 'com.android.calculator2/' },
  { key: 'settings', component: 'com.android.settings/.Settings' },
];
const logHas = (page, re, from) => page.evaluate(([src, k]) => (window.xb.all || []).slice(k).some((l) => new RegExp(src).test(l)),
  [re.source, from]);
const logLen = (page) => page.evaluate(() => (window.xb.all || []).length);
async function waitLog(page, re, from, ms) {
  const t = Date.now();
  while (Date.now() - t < ms) {
    if (await logHas(page, re, from)) return true;
    if (await page.evaluate(() => window.xb.status === 'done' || !!window.xb.error)) return false;
    await page.waitForTimeout(2000);
  }
  return false;
}
const esc = (t) => t.replace(/[./[\]]/g, (c) => '\\' + c);
// Each app: Home, a swipe up, the icon; its START line (else Home and again, up to three tries); then its Displayed
// line or 60 s, a screenshot (build/web-shots/s1-prewarm-<app>.png), 10 s more, and the next. An app that does not
// start fails the make. The last app's START line is the snapshot's condition (+ XN_SNAP_AFTER, default 120 s, and
// the quiet window).
async function prewarm(page, ui, apps) {
  const delay = Number(process.env.XN_PREWARM_DELAY ?? 240) * 1000;
  if (!(await waitLog(page, /START u0 .*HOME.*cmp=com\.android\.launcher3\//, 0, MAKE_TIMEOUT_MS))) {
    throw new Error('pre-warm: the launcher never became home');
  }
  console.log(`    pre-warm: the launcher is home; ${delay / 1000} s, then ${apps.map((a) => a.key).join(', ')}`);
  await page.waitForTimeout(delay);
  const swipe = [];
  for (let k = 0; k <= 14; k++) swipe.push([270, 860 - k * 45]);
  mkdirSync(SHOTS, { recursive: true });
  for (const app of apps) {
    let started = false, tries = 0;
    while (!started && tries < 3) {
      tries++;
      await page.click('#btn-home').catch(() => {});
      await page.waitForTimeout(5000);
      const k = await logLen(page);
      await gesture(page, swipe, 20);
      await page.waitForTimeout(15000);
      await gesture(page, [ui[app.key], ui[app.key]], 80);
      started = await waitLog(page, new RegExp(`START u0 .*cmp=${esc(app.component)}`), k, 40000);
      if (started) {
        // The app must be drawn (its "Displayed" line), or the snapshot holds a half-started activity and the
        // visitor's first open is a cold one (P1: one make's Settings was started but not drawn in 60 s).
        const shown = await waitLog(page, new RegExp(`Displayed ${esc(app.component)}`), k, 150000);
        if (!shown) {
          console.log(`    pre-warm: ${app.key} started (try ${tries}) but not drawn in 150 s; again`);
          started = false;
          continue;
        }
        await page.waitForTimeout(10000);
        await page.locator('#phone').screenshot({ path: join(SHOTS, `s1-prewarm-${app.key}.png`) });
        console.log(`    pre-warm: ${app.key} started (try ${tries})${shown ? ', displayed' : ''}`);
      }
    }
    if (!started) throw new Error(`pre-warm: ${app.key} did not start in 3 tries`);
  }
  await page.click('#btn-home').catch(() => {});
}

// XN_XJ=DIR: another chunk store (a frozen tree, docs/demo.md): the page boots its tree, and its snapshot is packed there.
if (process.env.XN_XJ) DIRS.xj = resolve(process.env.XN_XJ);

async function make(chromium) {
  await makeSnapshot(chromium);
  const snap = join(ROOT, 'build', 'snap', 'web.snap');
  if (!failures && existsSync(snap)) await packSnapshot(chromium, snap);
}

async function makeSnapshot(chromium) {
  const out = join(ROOT, 'build', 'snap');
  const srv = await startServer({ port: 0, snapshotUpload: out });
  const browser = await launch(chromium);
  try {
    const page = await browser.newPage({ viewport: VIEW });
    watch(page, browser, 'make');
    // The snapshot point (docs/demo.md): the launcher's HOME start, 480 s more (the BOOT_COMPLETED receivers start
    // their processes in that time, and the remote provisioner's, which waits for a network, ends in its ANR about
    // 340 s after HOME), then the first 10 s in which the engine was busy 15% of the time or less (at most 360 s more).
    // XN_SNAP_AFTER and XN_SNAP_QUIET change them (XN_SNAP_QUIET=off: no quiet wait).
    const after = Number(process.env.XN_SNAP_AFTER ?? 480);
    // The whole log goes to build/snap/web-make-full.log, with the engine's load report every 5 s (docs/demo.md: when
    // the system is quiet after the launcher's start). XN_SNAP_QUERY adds page parameters (?prop=K=V for A/B runs).
    // XN_SNAP_QUIET=PCT:WINDOW:MAX: then a quiet window (--snapshot-quiet, docs/demo.md).
    const quietArg = process.env.XN_SNAP_QUIET ?? '15:10:360';
    const quiet = quietArg !== 'off' ? `&snapquiet=${quietArg}` : '';
    // Pre-warm (XN_SNAP_PREWARM=on, docs/demo.md; off by default): before the snapshot, each demo app is opened once
    // from the drawer and left with Home, so the snapshot holds their processes, started and drawn. The condition
    // becomes the last app's START line, + 120 s (its start, Home, then the quiet window); XN_PREWARM_DELAY is the wait
    // after the launcher's HOME start before the first app (default 240 s, the BOOT_COMPLETED receivers' time). It is
    // off because its snapshot (997 MiB of memory, 177 MB of WebGL objects: the apps' windows and task snapshots) did
    // not restore in headless Chromium: the page closed before the first frame.
    const ui = drawerLayout(DIRS.xj);
    const apps = process.env.XN_SNAP_PREWARM === 'off' ? [] : PREWARM.filter((a) => ui[a.key]); // on by default (P1)
    // XN_SNAP_AT=log:TEXT: the condition (default: the launcher's HOME start, or the pre-warm's last app).
    const atCond = process.env.XN_SNAP_AT || (apps.length ? `log:cmp=${apps[apps.length - 1].component}` : '');
    const at = atCond ? `&snapat=${encodeURIComponent(atCond)}` : '';
    const afterArg = apps.length && !process.env.XN_SNAP_AFTER ? 120 : after;
    await page.goto(srv.url + `?snapshot=make&snapafter=${afterArg}&timeout=1700&keeplog=1&load=5000${quiet}${at}` +
      (process.env.XN_SNAP_QUERY || ''), { waitUntil: 'load' });
    await page.click('#boot');
    const t0 = Date.now();
    if (apps.length) {
      await prewarm(page, ui, apps).catch((e) => {
        console.log('    ' + String(e));
        failures++;
      });
    }
    if (failures) {
      // the log of the failed make, for its cause (build/snap/web-make-full.log, as a finished make writes it)
      writeFileSync(join(out, 'web-make-full.log'), (await page.evaluate(() => (window.xb.all || []).join('\n'))) + '\n');
      throw new Error('make: the pre-warm failed');
    }
    await page.waitForFunction(() => window.xb.status === 'done' || window.xb.status === 'error' || window.xb.error, null,
      { timeout: MAKE_TIMEOUT_MS, polling: 1000 });
    const top = await page.evaluate(() => (window.xb.all || []).filter((l) => /snapshot: range \d+:/.test(l)).pop() || '');
    if (/0x[0-9a-f]+\.\.0x([0-9a-f]+)/.test(top)) console.log(`    image top: ${top.replace(/^.*\.\./, '')} (2 GB is 0x80000000)`);
    const r = await page.evaluate(() => ({ done: window.xb.done, error: window.xb.error, ms: window.xb.milestones,
      log: (window.xb.all || window.xb.notes || []).filter((l) => /^xiaoji: (snapshot|gl\(webgl\): snapshot)|HOME.*launcher3/.test(l)) }));
    console.log(`--- make: ${Math.round((Date.now() - t0) / 1000)} s, exit ${r.done ? r.done.exitCode : '-'}`);
    for (const l of r.log) console.log('    ' + (/gl\(webgl\)/.test(l) ? l : l.slice(0, 200)));
    if (r.error) console.log('    error: ' + r.error.split('\n')[0]);
    const full = await page.evaluate(() => (window.xb.all || []).join('\n'));
    writeFileSync(join(out, 'web-make-full.log'), full + '\n');
    // The cached-app freezer (P1: off, the engine's binder says it cannot freeze) and what was killed before the snapshot.
    for (const l of full.split('\n').filter((x) => /[Ff]reezer|ActivityManager\(\d+\): Killing/.test(x)).slice(0, 12)) {
      console.log('    ' + l.slice(0, 200));
    }
    console.log(`    browser RSS peak during the make: ${watch.peak} MiB (all its processes)`);
    check(r.done && r.done.exitCode === 0, 'make: the web engine wrote its snapshot and stopped (exit 0)');
    // Health: the last system_server started SystemUI once (a crash loop, as in a trial tree, docs/demo.md, would be in
    // the snapshot; a first boot can restart system_server once, which starts SystemUI again).
    const ams = [...full.matchAll(/ActivityManager\((\d+)\): Start proc \d+:com\.android\.systemui\//g)].map((m) => m[1]);
    const lastSs = ams.length ? ams[ams.length - 1] : null;
    const sysui = ams.filter((p) => p === lastSs).length;
    // A SystemUI that ActivityManager killed (an ANR on a loaded machine, closed by the pre-warm's tap) and started
    // again is not the start-order race this check is for.
    const killed = lastSs ? [...full.matchAll(new RegExp(`ActivityManager\\(${lastSs}\\): Killing \\d+:com\\.android\\.systemui/`, 'g'))].length : 0;
    check(sysui === 1 || (killed > 0 && sysui <= 1 + killed),
      `make: SystemUI started once by the last system_server (${sysui}; ${ams.length} in the boot; killed by it ${killed})`);
  } finally {
    await browser.close();
    await srv.close();
  }
}

// tools/snappack.py, the touch order and the tree's prefetch (docs/snapshot.md, S4 and S5; docs/demo.md, P1).
async function packSnapshot(chromium, snap) {
  {
    console.log(`    build/snap/web.snap: ${(statSync(snap).size / 1048576).toFixed(1)} MiB`);
    const pack = (...extra) => execFileSync('python3', ['-I', join(ROOT, 'tools', 'snappack.py'), snap, DIRS.xj,
      '--tree', join(DIRS.xj, 'manifest-xiaoji-v2.json'), ...extra], { stdio: 'inherit' });
    pack();
    // Lazy pages (docs/snapshot.md, S5): a restore records which pages the first frame, the idle phone and the demo
    // apps touch, and the snapshot is packed again with those hot (up to the first frame) and the rest in that order.
    let order = join(ROOT, 'image', 'restore-order.json');
    if (process.env.XN_SNAP_RECORD !== 'off') {
      const rec = await record(chromium);
      if (rec) {
        pack('--order', rec.touch);
        // The recording's file reads are this snapshot's own prefetch order: its clean pages are the ones its make
        // left out. First, then the committed order's other chunks (a session the recording did not make).
        execFileSync('python3', ['-I', join(ROOT, 'tools', 'prefetch.py'), 'record', rec.fetchLog, '--tree',
          join(DIRS.xj, 'manifest-xiaoji-v2.json'), '--out', join(ROOT, 'build', 'snap', 'restore-order.json')], { stdio: 'inherit' });
        const mine = JSON.parse(readFileSync(join(ROOT, 'build', 'snap', 'restore-order.json'), 'utf8'));
        const theirs = existsSync(order) ? JSON.parse(readFileSync(order, 'utf8')) : [];
        const seen = new Set(mine.map((x) => JSON.stringify(x)));
        const both = [...mine, ...theirs.filter((x) => !seen.has(JSON.stringify(x)))];
        order = join(ROOT, 'build', 'snap', 'prefetch-order.json');
        writeFileSync(order, JSON.stringify(both) + '\n');
        console.log(`    prefetch order: ${mine.length} chunks from the recording, ${both.length - mine.length} more from image/restore-order.json`);
      }
    }
    // The restore's prefetch (docs/demo.md, P1): the chunks of the recorded first minute, packed for the page.
    if (existsSync(order)) {
      execFileSync('python3', ['-I', join(ROOT, 'tools', 'prefetch.py'), 'pack', order, '--xj', DIRS.xj], { stdio: 'inherit' });
    }
  }
}

// A pointer gesture on the Android canvas, in display pixels (540x960), through the letterboxed box (web/input.js).
async function gesture(page, points, stepMs) {
  const box = await page.evaluate(() => {
    const el = document.getElementById('android-screen');
    const r = el.getBoundingClientRect();
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

// The touch order of a restore (docs/snapshot.md, S5): the page restores with ?snaprecord=1, so every lazy page is
// read on demand, one at a time, and its first touch is kept with its time since main. The tree's prefetch is off
// (?prefetch=0), so the server's log has every file chunk the session reads (build/snap/fetch.log, for
// tools/prefetch.py record). The phone idles
// (XN_RECORD_IDLE, default 20 s), then the demo apps open from the drawer in the order tools/latency.mjs opens them
// (Settings, Calculator, Files), each with Home after it. Writes build/snap/touch.json: {pages: [[address, ms], ...],
// first_frame_ms, marks}; returns {touch, fetchLog}, or null when the snapshot has no lazy pages.
async function record(chromium) {
  const fetchLines = [];
  const srv = await startServer({ port: 0, log: (l) => fetchLines.push(`${Date.now()} ${l}`) });
  const browser = await launch(chromium);
  const out = join(ROOT, 'build', 'snap', 'touch.json');
  const ui = drawerLayout(DIRS.xj);
  try {
    const page = await browser.newPage({ viewport: VIEW });
    watch(page, browser, 'record');
    await page.goto(srv.url + '?keeplog=1&snaprecord=1&prefetch=0&load=5000', { waitUntil: 'load' });
    await page.click('#boot');
    await page.waitForFunction(() => window.xb.restore && /loaded|refused/.test(window.xb.restore.state) || window.xb.error,
      null, { timeout: 600000, polling: 200 });
    const st = await page.evaluate(() => ({ restore: window.xb.restore, error: window.xb.error }));
    if (!st.restore || st.restore.state !== 'loaded' || !st.restore.lazy) {
      console.log(`--- record: no lazy restore (${st.error ? st.error.split('\n')[0] : JSON.stringify(st.restore && st.restore.state)})`);
      return null;
    }
    await page.waitForFunction(() => window.xb.frames && window.xb.frames.frames > 0, null, { timeout: 600000, polling: 200 });
    const marks = [];
    const mark = async (what) => marks.push([what, await page.evaluate(() => performance.timeOrigin + performance.now())]);
    await mark('first frame');
    await page.waitForTimeout(Number(process.env.XN_RECORD_IDLE ?? 20) * 1000);
    const swipe = [];
    for (let k = 0; k <= 14; k++) swipe.push([270, 860 - k * 45]);
    // Every page is read on demand here, so the phone is slower than a visitor's: the drawer gets 20 s to open, and a
    // tap that started nothing (a swipe read as something else) is tried again, up to three times.
    for (const app of [PREWARM[2], PREWARM[1], PREWARM[0]]) {
      if (!ui[app.key]) continue;
      let started = false;
      for (let tries = 1; tries <= 3 && !started; tries++) {
        await page.click('#btn-home').catch(() => {});
        await page.waitForTimeout(5000);
        await mark(`drawer for ${app.key}`);
        await gesture(page, swipe, 20);
        await page.waitForTimeout(20000);
        const k = await logLen(page);
        await mark(app.key);
        await gesture(page, [ui[app.key], ui[app.key]], 80);
        started = await waitLog(page, new RegExp(`START u0 .*cmp=${esc(app.component)}`), k, 60000);
        console.log(`    record: ${app.key} ${started ? 'started' : 'did not start'} (try ${tries})`);
      }
      // A recording without an app's first open would leave its pages out of the order: the make fails (run
      // `node tools/snapshot-web.mjs record` again; the snapshot is packed with what was recorded).
      check(started, `record: ${app.key} started from the drawer`);
      await page.waitForTimeout(20000);
    }
    await page.click('#btn-home').catch(() => {});
    await page.waitForTimeout(10000);
    await mark('end');
    await page.waitForTimeout(1500); // the last batch of touches (the worker posts them every 0.5 s)
    const r = await page.evaluate(() => ({ touches: window.xb.lazyTouches || [], restore: window.xb.restore, lazy: window.xb.lazy }));
    const mainAt = r.restore.mainAt;
    const rec = {
      pages: r.touches,
      first_frame_ms: Math.round(r.restore.firstFrameAt - mainAt),
      marks: marks.map(([w, t]) => [w, Math.round(t - mainAt)]),
    };
    writeFileSync(out, JSON.stringify(rec) + '\n');
    const before = rec.pages.filter(([, t]) => t <= rec.first_frame_ms).length;
    console.log(`--- record: ${rec.pages.length} pages touched (${before} before the first frame, at ${rec.first_frame_ms} ms ` +
      `after main), of ${r.restore.lazy.pages} lazy; ${r.lazy ? r.lazy.syncFetches : '-'} chunks fetched -> ${out}`);
    for (const [w, t] of rec.marks) console.log(`    ${w}: ${t} ms, ${rec.pages.filter(([, x]) => x <= t).length} pages by then`);
    const fetchLog = join(ROOT, 'build', 'snap', 'fetch.log');
    writeFileSync(fetchLog, fetchLines.join('\n') + '\n');
    return { touch: out, fetchLog };
  } finally {
    await browser.close();
    await srv.close();
  }
}

async function restore(chromium) {
  mkdirSync(SHOTS, { recursive: true });
  const srv = await startServer({ port: 0 });
  const browser = await launch(chromium);
  try {
    const page = await browser.newPage({ viewport: VIEW });
    watch(page, browser, 'restore');
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await page.goto(srv.url + '?keeplog=1' + (process.env.XN_SNAP_QUERY || ''), { waitUntil: 'load' });
    const tPress = Date.now();
    await page.click('#boot');
    // The boot screen while the snapshot downloads: the restore stage.
    const restoring = await page.waitForFunction(() => window.xb.screen && window.xb.screen.label === 'Restoring Android', null,
      { timeout: 60000, polling: 50 }).then(() => true).catch(() => false);
    if (restoring) await page.locator('#phone').screenshot({ path: join(SHOTS, 's1-restoring.png') });
    const scr = await page.evaluate(() => window.xb.screen);
    check(restoring, `restore: the boot screen shows the restore stage ("Restoring Android": ${scr ? scr.detail : '-'})`);
    await page.waitForFunction(() => window.xb.restore && window.xb.restore.state === 'loaded', null, { timeout: 600000, polling: 100 })
      .catch(() => {});
    await page.waitForFunction(() => window.xb.frames && window.xb.frames.frames > 0, null, { timeout: 300000, polling: 100 })
      .catch(() => {});
    const tFrame = Date.now();
    await page.waitForTimeout(1500); // the boot screen's outro
    const r1 = await page.evaluate(() => ({ restore: window.xb.restore, frames: window.xb.frames, error: window.xb.error }));
    await page.locator('#phone').screenshot({ path: join(SHOTS, 's1-restore-first-frame.png') });
    const rs = r1.restore || {};
    console.log(`--- restore: ${rs.state}; snapshot ${((rs.bytes || 0) / 1048576).toFixed(1)} MiB downloaded and written in ` +
      `${((rs.loadMs || 0) / 1000).toFixed(2)} s; press to first frame ${((tFrame - tPress) / 1000).toFixed(2)} s ` +
      `(engine start to first frame ${rs.firstFrameWall && rs.loadedWall ? ((rs.firstFrameWall - rs.loadedWall) / 1000).toFixed(2) : '-'} s)`);
    if (r1.error) console.log('    error: ' + r1.error.split('\n')[0]);
    check(rs.state === 'loaded', 'restore: the page restored the snapshot (not a boot)');
    check(r1.frames && r1.frames.frames > 0 && r1.frames.distinct > 1, `restore: the first frame after the restore is a picture (${r1.frames ? r1.frames.distinct : 0} colours)`);
    // The launcher's all-apps drawer (a swipe up), then a tap on Settings (tools/ui-points.mjs).
    // Let the restored launcher settle, then: Home (it closes a menu that a slow first touch opened as a long press),
    // a swipe up, the drawer's 20 s to open, a tap on Settings. On a loaded machine the engine can be slow enough for a
    // swipe to read as a long press, so up to three tries.
    await page.waitForTimeout(Number(process.env.XN_SNAP_SETTLE_MS ?? 5000));
    const swipe = [];
    for (let k = 0; k <= 14; k++) swipe.push([270, 860 - k * 45]);
    let started = false, tries = 0, tTap = 0;
    while (!started && tries < 3) {
      tries++;
      await page.click('#btn-home').catch(() => {});
      await page.waitForTimeout(3000);
      await gesture(page, swipe, 20);
      await page.waitForTimeout(Number(process.env.XN_SNAP_TAP_WAIT_MS ?? 20000));
      await page.locator('#phone').screenshot({ path: join(SHOTS, 's1-restore-allapps.png') });
      const at = drawerLayout(DIRS.xj).settings; // Settings' place in the drawer depends on the tree's apps
      await gesture(page, [at, at], 80);
      tTap = Date.now();
      started = await page.waitForFunction((re) => (window.xb.notes || []).some((l) => new RegExp(re).test(l)), SETTINGS.source,
        { timeout: 40000, polling: 500 }).then(() => true).catch(() => false);
    }
    await page.waitForTimeout(Number(process.env.XN_SNAP_SETTINGS_WAIT_MS ?? 20000));
    await page.locator('#phone').screenshot({ path: join(SHOTS, 's1-restore-settings.png') });
    // What the apps draw after the restore (P1: a snapshot whose memory above 2 GB was written from the wrong place
    // restored, showed its first frame, and then drew blank screens without text): Settings' screen has text and
    // icons, so many colours.
    const colours = await page.evaluate(() => (window.xb.frames ? window.xb.frames.distinct : 0));
    check(colours >= 20, `restore: Settings is drawn after the restore, not blank (${colours} colours in the last frame)`);
    const log = await page.evaluate(() => (window.xb.all || []).join('\n'));
    writeFileSync(join(SHOTS, 's1-restore.log'), log + '\n');
    const bad = log.split('\n').filter((l) => /ANR in|FATAL EXCEPTION|Fatal signal/.test(l));
    console.log(`    tap to Settings START: ${started ? 'seen, try ' + tries : 'not seen in ' + tries + ' tries'}`);
    check(started, 'restore: a tap on Settings in the drawer starts Settings (ActivityTaskManager START ... com.android.settings/.Settings)');
    check(bad.length === 0, `restore: no ANR, fatal exception or fatal signal in the log${bad.length ? ' (' + bad[0].slice(0, 120) + ')' : ''}`);
    check(pageErrors.length === 0, `restore: no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
    console.log(`    screenshots: ${SHOTS}/s1-restoring.png, s1-restore-{first-frame,allapps,settings}.png; ` +
      `browser RSS peak ${watch.peak} MiB (all its processes)`);
  } finally {
    await browser.close();
    await srv.close();
  }
}

// The page boots when the snapshot does not fit (another engine build: its manifest's build is altered here) and when
// there is none (a 404): the boot starts (the tree is built and the engine runs), then it is stopped.
async function fallback(chromium) {
  const srv = await startServer({ port: 0 });
  const browser = await launch(chromium);
  try {
    for (const kind of ['other build', 'no snapshot']) {
      const page = await browser.newPage({ viewport: VIEW });
      const pageErrors = [];
      page.on('pageerror', (e) => pageErrors.push(String(e)));
      await page.route('**/xj/snapshot.json', async (route) => {
        if (kind === 'no snapshot') return route.fulfill({ status: 404, body: 'not found\n' });
        const res = await route.fetch();
        const m = await res.json();
        m.build = 'web-0000000000000000-0';
        return route.fulfill({ response: res, body: JSON.stringify(m) });
      });
      await page.goto(srv.url, { waitUntil: 'load' });
      await page.click('#boot');
      const ok = await page.waitForFunction(() => window.xb.status === 'running', null, { timeout: 300000, polling: 250 })
        .then(() => true).catch(() => false);
      const r = await page.evaluate(() => ({ restore: window.xb.restore, status: window.xb.status }));
      if (kind === 'other build') {
        check(ok && r.restore && r.restore.state === 'refused' && /another engine/.test(r.restore.text),
          `fallback: a snapshot of another build is refused and the page boots (${r.restore ? r.restore.text : '-'}; ${r.status})`);
      } else {
        check(ok && r.restore === null, `fallback: with no snapshot the page boots (${r.status})`);
      }
      check(pageErrors.length === 0, `fallback (${kind}): no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
      await page.click('#stop').catch(() => {});
      await page.close();
    }
  } finally {
    await browser.close();
    await srv.close();
  }
}

const mode = process.argv[2];
const modes = {
  make,
  restore,
  fallback,
  // record, then pack again with the order (an existing build/snap/web.snap; no new boot)
  record: async (chromium) => {
    const snap = join(ROOT, 'build', 'snap', 'web.snap');
    await packSnapshot(chromium, snap);
  },
  // the packing only, eager (XN_SNAP_RECORD=off) or with an existing order (XN_SNAP_ORDER=FILE)
  pack: async () => {
    const snap = join(ROOT, 'build', 'snap', 'web.snap');
    const extra = process.env.XN_SNAP_ORDER ? ['--order', process.env.XN_SNAP_ORDER] : [];
    if (process.env.XN_SNAP_HOT_MS) extra.push('--hot-ms', process.env.XN_SNAP_HOT_MS);
    execFileSync('python3', ['-I', join(ROOT, 'tools', 'snappack.py'), snap, DIRS.xj, '--tree', join(DIRS.xj, 'manifest-xiaoji-v2.json'),
      ...extra], { stdio: 'inherit' });
    // the prefetch order of the last recording (build/snap/prefetch-order.json), else the committed one
    const mine = join(ROOT, 'build', 'snap', 'prefetch-order.json');
    const order = existsSync(mine) && process.env.XN_SNAP_ORDER ? mine : join(ROOT, 'image', 'restore-order.json');
    if (existsSync(order)) execFileSync('python3', ['-I', join(ROOT, 'tools', 'prefetch.py'), 'pack', order, '--xj', DIRS.xj], { stdio: 'inherit' });
  },
};
if (!modes[mode]) {
  console.error('usage: node tools/snapshot-web.mjs make|record|pack|restore|fallback');
  process.exit(2);
}
const { chromium } = await import(pathToFileURL(PW).href);
BROWSER_EXE = process.env.XN_CHROME || chromium.executablePath().replace(/\/chromium-\d+\/.*$/, "/chromium_headless_shell-"); // this tool's browsers only (another suite may run its own)
await modes[mode](chromium);
process.exit(failures ? 1 : 0);
