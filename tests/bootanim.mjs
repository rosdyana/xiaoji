// The boot animation (W3): web/bootscreen.js's animation parts and the theme's animation knobs (node, no browser),
// then the page in headless Chromium with a stand-in engine worker (no Android boot): the intro, the loop, the
// outro into a first frame, the calm error state, reduced motion and a theme with the animation off.
//
//   node tests/bootanim.mjs            the node checks and the page checks
//   node tests/bootanim.mjs --node     the node checks alone
//
// Screenshots go to build/web-shots/w3-*.png. The page's clock is Playwright's (page.clock), and the CSS animations
// are paused and seeked for each shot, so the pictures do not depend on how fast the machine is. Playwright comes
// from XN_PLAYWRIGHT, as in tests/web.mjs.

import { mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import {
  newScreen, startRun, onBootTree, onError, onRestart, onStop, onEnd,
  onShown, onAnimRun, onFrame, animPhase, animTimings, animNextMs, animMotion, ANIM_DEFAULT, REDUCED_OUTRO_MS,
} from '../web/bootscreen.js';
import { DEFAULT_THEME, validateTheme } from '../web/theme.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PW =
  process.env.XN_PLAYWRIGHT ??
  '/Users/rosdyanakusuma/Ross/projects/xiaonux/bench/node_modules/playwright-core/index.mjs';
const SHOTS = join(ROOT, 'build', 'web-shots');
const VIEW = { width: 1100, height: 860 };

let failures = 0;
function check(ok, what) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures++;
}

function throws(f) {
  try {
    f();
    return null;
  } catch (e) {
    return e.message;
  }
}

// ---- node: bootscreen.js and theme.js ----

function nodeChecks() {
  const T = DEFAULT_THEME;
  const I = ANIM_DEFAULT.introMs;
  const O = ANIM_DEFAULT.outroMs;

  // Intro once, then rest (idle) or loop (working).
  let s = newScreen(T);
  check(animPhase(s, 0, false) === 'rest', 'anim: before the screen is shown there is no intro');
  onShown(s, 1000);
  check(animPhase(s, 1000, false) === 'intro', 'anim: the intro starts when the screen is shown');
  check(animNextMs(s, 1500, false) === I - 500, 'anim: the intro ends introMs after it started');
  check(animPhase(s, 1000 + I, false) === 'rest', 'anim: after the intro, idle rests');
  onShown(s, 9000);
  check(animPhase(s, 9000, false) === 'rest', 'anim: the intro plays once');
  startRun(s);
  onAnimRun(s);
  check(animPhase(s, 1000 + I, false) === 'loop', 'anim: the download loops');
  check(animNextMs(s, 1000 + I, false) === null, 'anim: the loop has no end of its own');
  onBootTree(s, 5000);
  check(animPhase(s, 6000, false) === 'loop', 'anim: the boot loops');
  const w = newScreen(T);
  onShown(w, 0);
  w.phase = 'some-new-phase';
  check(animPhase(w, I, false) === 'loop', 'anim: a working phase added later also loops');
  // A run that starts during the intro finishes the intro first.
  const e = newScreen(T);
  onShown(e, 0);
  startRun(e);
  check(animPhase(e, 100, false) === 'intro' && animPhase(e, I, false) === 'loop', 'anim: a run started in the intro loops after it');

  // The first frame: outro, then off; later frames do not restart it; a new run brings the screen back.
  onFrame(s, 10000);
  onFrame(s, 10300);
  check(animPhase(s, 10000, false) === 'outro', 'anim: the first frame starts the outro');
  check(animNextMs(s, 10200, false) === O - 200, 'anim: the outro lasts outroMs from the first frame');
  check(animPhase(s, 10000 + O, false) === 'off', 'anim: after the outro the boot screen is off');
  startRun(s);
  onAnimRun(s);
  check(animPhase(s, 20000, false) === 'loop', 'anim: a new run shows the boot screen again (loop)');

  // Errors calm the loop; a restart too; Stop and the end rest.
  const f = newScreen(T);
  onShown(f, 0);
  startRun(f);
  onBootTree(f, I);
  onError(f);
  check(animPhase(f, I + 100, false) === 'calm', 'anim: an error calms the animation');
  const r = newScreen(T);
  onShown(r, 0);
  startRun(r);
  onBootTree(r, I);
  onRestart(r);
  check(animPhase(r, I + 100, false) === 'calm', 'anim: a restart of the framework calms the animation');
  const st = newScreen(T);
  onShown(st, 0);
  startRun(st);
  onStop(st);
  check(animPhase(st, I + 1, false) === 'rest', 'anim: Stop rests');
  const d = newScreen(T);
  onShown(d, 0);
  startRun(d);
  onBootTree(d, 1);
  onEnd(d, 0, 2);
  check(animPhase(d, I + 1, false) === 'rest', 'anim: the end of the run rests');
  // An error after the first frame leaves the picture (the boot screen stays off).
  onFrame(f, I + 200);
  check(animPhase(f, I + 200 + O, false) === 'off', 'anim: frames win over the calm state');

  // Reduced motion: no intro, a short outro; the motion says the CSS must not animate.
  const m = newScreen(T);
  onShown(m, 0);
  check(animMotion(T, true) === 'reduced' && animTimings(T, true).motion === 'reduced', 'anim, reduced motion: motion is "reduced"');
  check(animPhase(m, 0, true) === 'rest' && animTimings(T, true).introMs === 0, 'anim, reduced motion: no intro');
  startRun(m);
  check(animPhase(m, 0, true) === 'loop', 'anim, reduced motion: the part is still loop (the CSS keeps it still)');
  onFrame(m, 100);
  check(animTimings(T, true).outroMs === Math.min(O, REDUCED_OUTRO_MS) && animPhase(m, 100 + REDUCED_OUTRO_MS, true) === 'off',
    `anim, reduced motion: the outro is a short fade (${REDUCED_OUTRO_MS} ms)`);

  // Theme animation "none": nothing moves, the outro is a cut.
  const none = { ...T, animation: { ...ANIM_DEFAULT, style: 'none' } };
  const n = newScreen(none);
  onShown(n, 0);
  check(animMotion(none, false) === 'none' && animPhase(n, 0, false) === 'rest', 'anim, style "none": no intro, motion "none"');
  startRun(n);
  onFrame(n, 50);
  check(animPhase(n, 50, false) === 'off', 'anim, style "none": the first frame cuts to the picture');

  // The theme's knobs.
  const base = {
    name: 'Xiao', accentPart: 'Ji', subtitle: 'Android 13', logo: { text: '小机' }, colors: T.colors,
    stages: T.stages.map((x) => ({ ...x })),
  };
  check(JSON.stringify(validateTheme(base, 'http://x/').animation) === JSON.stringify(ANIM_DEFAULT),
    'theme: no animation key takes the defaults');
  const v = validateTheme({ ...base, animation: { introMs: 1200, glow: false } }, 'http://x/').animation;
  check(v.introMs === 1200 && v.glow === false && v.outroMs === ANIM_DEFAULT.outroMs && v.style === 'default',
    'theme: animation fields left out take their defaults');
  check(/animation.style/.test(throws(() => validateTheme({ ...base, animation: { style: 'fancy' } }, 'http://x/')) || ''),
    'theme: an unknown animation style is refused');
  check(/animation.introMs/.test(throws(() => validateTheme({ ...base, animation: { introMs: 60000 } }, 'http://x/')) || ''),
    'theme: an intro longer than 5 s is refused');
  check(/animation.spinMs/.test(throws(() => validateTheme({ ...base, animation: { spinMs: '1s' } }, 'http://x/')) || ''),
    'theme: a duration that is not a number is refused');
  check(/animation.glow/.test(throws(() => validateTheme({ ...base, animation: { glow: 1 } }, 'http://x/')) || ''),
    'theme: glow must be true or false');
}

// ---- the page, with a stand-in for the engine's worker ----

// Replaces Worker before the page's scripts run: the boot's worker records what it is sent, and the test feeds it
// the worker's messages (window.__fw.emit).
function fakeWorker() {
  class FakeWorker {
    constructor(url) {
      this.url = String(url);
      this.onmessage = null;
      this.onerror = null;
      this.sent = [];
      window.__fw = this;
    }
    postMessage(m) {
      this.sent.push(m && m.type);
    }
    terminate() {
      this.terminated = true;
    }
    emit(data) {
      if (this.onmessage) this.onmessage({ data });
    }
  }
  window.Worker = FakeWorker;
}

// A stand-in first frame: 540 x 960 (the display), a dark blue screen with a lighter band and a dock.
function emitFrame() {
  const w = 540;
  const h = 960;
  const px = new Uint8ClampedArray(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const o = (y * w + x) * 4;
      const band = y > 300 && y < 420 && x > 60 && x < 480;
      const dock = y > 860 && y < 920 && x > 40 && x < 500;
      px[o] = band || dock ? 70 : 20;
      px[o + 1] = band || dock ? 110 : 40 + (y >> 4);
      px[o + 2] = band || dock ? 170 : 90 + (y >> 3);
      px[o + 3] = 255;
    }
  }
  window.__fw.emit({ type: 'frame', w, h, px: px.buffer, seq: 1 });
}

// Pauses every animation and transition on the page at t ms (from its start, delays included). The loop's two
// animations are left alone in the intro (only: 'intro'): once a script pauses or plays a CSS animation, the
// stylesheet's animation-play-state no longer moves it, and the loop checks read that.
const seek = (page, t, only = null) => page.evaluate(([t, only]) => {
  const loop = ['xj-spin', 'xj-breathe'];
  for (const a of document.getAnimations()) {
    if (only === 'intro' && loop.includes(a.animationName)) continue;
    a.pause();
    a.currentTime = t;
  }
}, [t, only]);

const resumeAll = (page) => page.evaluate(() => {
  for (const a of document.getAnimations()) {
    if (a instanceof CSSTransition) a.finish();
    else a.play();
  }
});

const anim = (page) => page.evaluate(() => {
  const p = document.getElementById('phone');
  return { anim: p.dataset.anim, motion: p.dataset.motion, screen: window.xb.screen };
});

async function shot(page, name) {
  const path = join(SHOTS, `w3-${name}.png`);
  await page.locator('#phone').screenshot({ path });
  console.log(`    screenshot: ${path}`);
}

async function newPage(browser, { reduced = false, theme = null } = {}) {
  const ctx = await browser.newContext({ viewport: VIEW, reducedMotion: reduced ? 'reduce' : 'no-preference' });
  const page = await ctx.newPage();
  page.errors = [];
  page.on('pageerror', (e) => page.errors.push(String(e)));
  await page.addInitScript(fakeWorker);
  if (theme) {
    await page.route('**/theme-w3test.json', (route) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(theme) }));
  }
  await page.clock.install();
  return page;
}

async function open(page, baseUrl, query = '') {
  await page.goto(baseUrl + '?boot=manifest' + query, { waitUntil: 'load' });
  await page.waitForFunction(() => window.xb && window.xb.screen && window.xb.screen.anim, null, { timeout: 30000 });
}

async function boot(page) {
  await page.click('#boot');
  await page.waitForFunction(() => window.__fw && window.__fw.onmessage, null, { timeout: 30000 });
  await page.evaluate(() => {
    window.__fw.emit({ type: 'boot-tree' });
    window.__fw.emit({ type: 'milestone', id: 'art', label: 'ART up', wall: 3000, stamp: null, line: '' });
  });
}

async function pageChecks(chromium, baseUrl) {
  mkdirSync(SHOTS, { recursive: true });
  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  try {
    // Intro, loop, outro.
    const page = await newPage(browser);
    await open(page, baseUrl);
    let a = await anim(page);
    check(a.anim === 'intro' && a.motion === 'full', `page: the intro plays when the page opens (${a.anim}, ${a.motion})`);
    for (const [i, t] of [[1, 250], [2, 800], [3, 1300], [4, 1900]]) {
      await seek(page, t, 'intro');
      await shot(page, `intro-${i}`);
    }
    await page.clock.runFor(ANIM_DEFAULT.introMs + 100);
    a = await anim(page);
    check(a.anim === 'rest', `page: after the intro the idle screen rests (${a.anim})`);
    await boot(page);
    a = await anim(page);
    check(a.anim === 'loop' && a.screen.label === 'Loading framework', `page: the boot loops (${a.anim}, "${a.screen.label}")`);
    const loop = await page.evaluate(() => {
      // The boot animation's own keyframes (the progress bar's width transition is the W2 bar, not part of it).
      const running = document.getAnimations().filter((x) => x.playState === 'running' && x.animationName);
      const props = new Set();
      for (const x of running) for (const k of x.effect.getKeyframes()) for (const p of Object.keys(k)) props.add(p);
      ['offset', 'computedOffset', 'easing', 'composite'].forEach((p) => props.delete(p));
      return {
        names: running.map((x) => x.animationName).sort(),
        props: [...props].sort(),
        spin: getComputedStyle(document.querySelector('.l-spin')).animationPlayState,
        canvas: getComputedStyle(document.getElementById('android-screen')).opacity,
      };
    });
    check(loop.names.includes('xj-spin') && loop.names.includes('xj-breathe'), `page, loop: the arc spins and the glow breathes (${loop.names})`);
    check(loop.props.every((p) => p === 'transform' || p === 'opacity'), `page, loop: only transform and opacity animate (${loop.props})`);
    check(loop.canvas === '0', 'page, loop: the Android canvas is see-through until the first frame');
    // A hidden tab pauses the loop.
    const hidden = await page.evaluate(() => {
      Object.defineProperty(document, 'hidden', { configurable: true, get: () => true });
      document.dispatchEvent(new Event('visibilitychange'));
      const st = getComputedStyle(document.querySelector('.l-spin')).animationPlayState;
      const anyRunning = document.getAnimations().some((x) => x.playState === 'running' && x.animationName);
      delete document.hidden;
      document.dispatchEvent(new Event('visibilitychange'));
      return { st, anyRunning, back: getComputedStyle(document.querySelector('.l-spin')).animationPlayState };
    });
    check(hidden.st === 'paused' && !hidden.anyRunning, 'page: a hidden tab pauses the loop');
    check(hidden.back === 'running', 'page: the loop runs again when the tab is visible');
    for (const [i, t] of [[1, 400], [2, 1100]]) {
      await seek(page, t);
      await shot(page, `loop-${i}`);
    }
    await resumeAll(page);
    // The first frame: the outro.
    await page.evaluate(emitFrame);
    a = await anim(page);
    check(a.anim === 'outro', `page: the first frame starts the outro (${a.anim})`);
    await seek(page, Math.round(ANIM_DEFAULT.outroMs * 0.45));
    await shot(page, 'outro-1');
    await page.clock.runFor(ANIM_DEFAULT.outroMs + 100);
    await resumeAll(page);
    a = await anim(page);
    const end = await page.evaluate(() => ({
      logo: getComputedStyle(document.querySelector('.logo')).visibility,
      stage: getComputedStyle(document.querySelector('.stage')).visibility,
      canvas: getComputedStyle(document.getElementById('android-screen')).opacity,
      spin: getComputedStyle(document.querySelector('.l-spin')).animationPlayState,
      stopOnTop: (() => {
        const stop = document.getElementById('stop');
        const r = stop.getBoundingClientRect();
        const el = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2);
        return !!el && (el === stop || stop.contains(el));
      })(),
    }));
    check(a.anim === 'off', `page: after the outro the boot screen is off (${a.anim})`);
    check(end.logo === 'hidden' && end.stage === 'hidden' && end.canvas === '1', 'page: the picture shows and the boot screen is hidden (takes no taps)');
    check(end.spin === 'paused', 'page: the hidden loop is paused');
    check(end.stopOnTop, 'page: Stop is still over the picture');
    await shot(page, 'outro-2');
    check(page.errors.length === 0, `page: no page errors${page.errors.length ? ' (' + page.errors[0] + ')' : ''}`);
    await page.context().close();

    // Real time (no page.clock, no seeking): the intro ends by itself, the render ticks do not restart the loop, and
    // the outro ends by itself.
    const rt = await browser.newContext({ viewport: VIEW });
    const live = await rt.newPage();
    live.errors = [];
    live.on('pageerror', (e) => live.errors.push(String(e)));
    await live.addInitScript(fakeWorker);
    await open(live, baseUrl);
    await live.waitForFunction(() => document.getElementById('phone').dataset.anim === 'rest', null, { timeout: 30000 });
    await boot(live);
    const t1 = await live.evaluate(() => {
      window.__spin = document.getAnimations().find((x) => x.animationName === 'xj-spin');
      return window.__spin.currentTime;
    });
    await live.waitForFunction((t1) => window.__spin.currentTime >= t1 + 600, t1, { timeout: 30000 });
    const same = await live.evaluate(() => document.getAnimations().includes(window.__spin) && window.__spin.playState === 'running');
    check(same, 'page, real time: the loop keeps running through the render ticks (the same animation, not restarted)');
    await live.evaluate(emitFrame);
    await live.waitForFunction(() => document.getElementById('phone').dataset.anim === 'off' &&
      getComputedStyle(document.querySelector('.logo')).visibility === 'hidden', null, { timeout: 30000 });
    check(true, 'page, real time: the intro and the outro end by themselves');
    check(live.errors.length === 0, `page, real time: no page errors${live.errors.length ? ' (' + live.errors[0] + ')' : ''}`);
    await rt.close();

    // An error: the animation calms.
    const err = await newPage(browser);
    await open(err, baseUrl);
    await err.clock.runFor(ANIM_DEFAULT.introMs + 100);
    await boot(err);
    await seek(err, 700);
    await err.evaluate(() => window.__fw.emit({ type: 'error', text: 'test: the engine stopped' }));
    a = await anim(err);
    const calm = await err.evaluate(() => ({
      spin: getComputedStyle(document.querySelector('.l-spin')).animationPlayState,
      arc: getComputedStyle(document.querySelector('.l-arc')).opacity,
      halo: getComputedStyle(document.querySelector('.halo')).opacity,
    }));
    check(a.anim === 'calm' && a.screen.label === 'The engine stopped' && a.screen.count === '', `page, error: calm, says what failed, no progress count ("${a.screen.label}", "${a.screen.count}")`);
    check(calm.spin === 'paused', 'page, error: the arc stops');
    await err.clock.runFor(1000);
    await resumeAll(err);
    await shot(err, 'calm');
    check(err.errors.length === 0, `page, error: no page errors${err.errors.length ? ' (' + err.errors[0] + ')' : ''}`);
    await err.context().close();

    // Reduced motion: no intro and nothing runs.
    const rm = await newPage(browser, { reduced: true });
    await open(rm, baseUrl);
    a = await anim(rm);
    check(a.anim === 'rest' && a.motion === 'reduced', `page, reduced motion: no intro (${a.anim}, ${a.motion})`);
    await boot(rm);
    a = await anim(rm);
    const still = await rm.evaluate(() => ({
      running: document.getAnimations().filter((x) => x.playState === 'running').map((x) => x.animationName || x.transitionProperty),
      spin: getComputedStyle(document.querySelector('.l-spin')).animationName,
    }));
    check(a.anim === 'loop' && still.running.length === 0, `page, reduced motion: nothing animates while booting (${still.running})`);
    check(still.spin === 'none', 'page, reduced motion: the arc has no animation');
    await shot(rm, 'reduced');
    await rm.evaluate(emitFrame);
    const fade = await rm.evaluate(() => getComputedStyle(document.querySelector('.logo')).transitionDuration);
    check(fade.split(',')[0].trim() === `${REDUCED_OUTRO_MS / 1000}s`, `page, reduced motion: the outro is a ${REDUCED_OUTRO_MS} ms fade (${fade})`);
    check(rm.errors.length === 0, `page, reduced motion: no page errors${rm.errors.length ? ' (' + rm.errors[0] + ')' : ''}`);
    await rm.context().close();

    // A theme with the animation off.
    const th = JSON.parse(JSON.stringify(DEFAULT_THEME));
    th.animation = { style: 'none' };
    const off = await newPage(browser, { theme: th });
    await open(off, baseUrl, '&theme=w3test');
    a = await anim(off);
    const note = await off.evaluate(() => document.getElementById('themenote').textContent);
    check(note === '' && a.motion === 'none' && a.anim === 'rest', `page, theme animation "none": nothing moves (${a.anim}, ${a.motion}${note ? ', ' + note : ''})`);
    check(off.errors.length === 0, `page, theme: no page errors${off.errors.length ? ' (' + off.errors[0] + ')' : ''}`);
    await off.context().close();
  } finally {
    await browser.close();
  }
}

async function main() {
  nodeChecks();
  if (!process.argv.includes('--node')) {
    const { startServer } = await import('../web/serve.mjs');
    const { chromium } = await import(pathToFileURL(PW).href);
    const srv = await startServer({ port: 0, log: () => {} });
    try {
      await pageChecks(chromium, srv.url);
    } finally {
      await srv.close();
    }
  }
  console.log(failures ? `\n${failures} check(s) failed` : '\nall boot animation checks passed');
  process.exit(failures ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
