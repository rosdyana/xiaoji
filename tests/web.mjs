// Headless Chromium test for the web build (M4): starts web/serve.mjs, opens the page,
// runs ART cases in the worker and checks stdout and the exit code against the oracle
// (build/oracle/<case>.{out,rc}, the native run in the builder VM).
//
//   node tests/web.mjs [case ...]        (default: hello-jit)
//   node tests/web.mjs --boot-only       the Boot Android check alone (W1)
//   XIAOJI_WEB_BOOT=1 node tests/web.mjs ... also runs the Boot Android check
//
// The Boot Android check (W1) clicks "Boot Android" on the page and waits for the milestone
// "Starting phase 100" (the furthest point the native boot reaches reliably at commit 054b086). It
// then checks that every earlier milestone was reached, and that the log was still streaming (the
// worker was still running when the milestone arrived). It stops the boot with the page's Stop
// button. XN_BOOT_TIMEOUT_MS bounds the wait (default 900000).
//
// Playwright comes from XN_PLAYWRIGHT (the playwright-core ESM entry), defaulting to the
// copy in the xiaonux bench. Each case gets a new browser process, so each run has a fresh
// profile. Not part of make test; make test-web runs it.

import { execFileSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { startServer } from '../web/serve.mjs';
import { CASES, GUEST_ENV } from '../web/cases.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PW =
  process.env.XN_PLAYWRIGHT ??
  '/Users/rosdyanakusuma/Ross/projects/xiaonux/bench/node_modules/playwright-core/index.mjs';
const TIMEOUT_MS = Number(process.env.XN_WEB_TIMEOUT_MS ?? 600000);
const BOOT_TIMEOUT_MS = Number(process.env.XN_BOOT_TIMEOUT_MS ?? 900000);
// The milestones that come before phase 100 in the native boot (any order).
const BOOT_REQUIRED = ['art', 'preload', 'sensors', 'health', 'sf', 'forked', 'entry', 'phase100'];

let failures = 0;
function check(ok, what) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures++;
}

// The page's guest environment must match tools/guest-env.sh, the one the native runs use.
function checkEnvMatchesScript() {
  const vars = Object.keys(GUEST_ENV);
  const out = execFileSync(
    'bash',
    ['-c', '. "$1"; for v in ' + vars.join(' ') + '; do printf "%s=%s\\n" "$v" "${!v}"; done', '_', join(ROOT, 'tools/guest-env.sh')],
    { encoding: 'utf8' },
  );
  const want = out.split('\n').filter(Boolean);
  const have = vars.map((k) => `${k}=${GUEST_ENV[k]}`);
  check(JSON.stringify(want) === JSON.stringify(have), 'guest env matches tools/guest-env.sh');
}

async function runCase(chromium, baseUrl, name) {
  const expectOut = readFileSync(join(ROOT, CASES[name].expect), 'utf8');
  const expectRc = Number(readFileSync(join(ROOT, CASES[name].expect.replace(/\.out$/, '.rc')), 'utf8').trim());

  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  try {
    await page.goto(baseUrl + '?cases=1', { waitUntil: 'load' });
    await page.selectOption('#case', name);
    await page.click('#run');
    await page.waitForFunction(() => window.xn && window.xn.status !== 'running', null, {
      timeout: TIMEOUT_MS,
      polling: 250,
    });
    const xn = await page.evaluate(() => window.xn);
    const stdout = xn.stdout.map((l) => l + '\n').join('');
    console.log(`--- ${name}: status "${xn.status}", exit ${xn.done ? xn.done.exitCode : 'n/a'}`);
    if (xn.error) console.log(`    error: ${xn.error.split('\n')[0]}`);
    for (const l of xn.stderr.slice(0, Number(process.env.XN_SHOW ?? 8))) console.log(`    stderr: ${l}`);
    const stats = xn.stderr.filter((l) => /^xiaoji: \d+ instructions in /.test(l));
    if (stats.length) console.log(`    ${stats[stats.length - 1]}`);
    if (xn.done) {
      console.log(`    timing: ${JSON.stringify(xn.done.timing)}`);
      console.log(`    fetch: ${JSON.stringify(xn.done.fetch)}`);
    }
    check(xn.status === `exit ${expectRc}`, `${name}: worker finished with exit ${expectRc}`);
    check(stdout === expectOut, `${name}: stdout matches build/oracle/${name}.out`);
    check(pageErrors.length === 0, `${name}: no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
    return xn;
  } finally {
    await browser.close();
  }
}

// Screenshots of the boot screen (W2) go here. Not committed (build/ is ignored).
const SHOTS = join(ROOT, 'build', 'web-shots');
const VIEW = { width: 1100, height: 860 };

// The packs come over a slow link (40 Mbit/s). The engine starts after the first pack (docs/loading.md, L2), and the
// rest keep downloading during the boot: the boot screen's detail line shows them. The run is stopped while packs
// are still arriving. Also checks that a reduced-motion user gets no spin.
async function runDownloadShot(chromium) {
  const slow = await startServer({ port: 0, log: () => {}, throttle: { mbps: 40, rttMs: 0 } });
  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  const page = await browser.newPage({ viewport: VIEW });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  try {
    await page.emulateMedia({ reducedMotion: 'reduce' });
    await page.goto(slow.url + '?boot=pack', { waitUntil: 'load' });
    await page.click('#boot');
    const spin = await page.evaluate(() => getComputedStyle(document.querySelector('.l-spin')).animationName);
    check(spin === 'none', 'boot screen: the arc does not spin with prefers-reduced-motion');
    await page.emulateMedia({ reducedMotion: 'no-preference' });
    await page.waitForFunction(
      () => window.xb.screen && /packs [\d.]+ of [\d.]+ MB/.test(window.xb.screen.detail || ''),
      null,
      { timeout: 120000, polling: 100 },
    );
    const s = await page.evaluate(() => window.xb.screen);
    check(/packs [\d.]+ of [\d.]+ MB/.test(s.detail), `boot screen, packs still downloading: "${s.label}", "${s.detail}", ${s.count}`);
    await page.screenshot({ path: join(SHOTS, 'boot-download.png') });
    console.log(`    screenshot: ${join(SHOTS, 'boot-download.png')}`);
    await page.click('#stop');
    await page.waitForFunction(() => window.xb.status === 'stopped', null, { timeout: 30000, polling: 100 });
    check(pageErrors.length === 0, `boot screen, download: no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
  } finally {
    await browser.close();
    await slow.close();
  }
}

// The Boot Android check (W1). Returns nothing; counts failures like the other checks.
async function runBoot(chromium, baseUrl) {
  mkdirSync(SHOTS, { recursive: true });
  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  const page = await browser.newPage({ viewport: VIEW });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  try {
    // The chunk store and boot packs are the page's default (docs/loading.md); ?boot=pack says so explicitly.
    await page.goto(baseUrl + '?boot=pack', { waitUntil: 'load' });
    check(await page.isHidden('#devlog'), 'boot screen: the log is hidden by default');
    const t0 = Date.now();
    await page.click('#boot');
    await page.waitForFunction(
      () => window.xb && (window.xb.milestones.phase100 || ['done', 'error', 'stopped'].includes(window.xb.status)),
      null,
      { timeout: BOOT_TIMEOUT_MS, polling: 500 },
    );
    // The stats follow the milestone; wait for them, then read the state.
    await page.waitForFunction(() => window.xb.stats !== undefined || window.xb.status === 'done', null, {
      timeout: 60000,
      polling: 100,
    }).catch(() => {});
    // The worker is still running here if the log streamed: the milestone arrived before the engine stopped.
    const streaming = await page.evaluate(() => window.xb.status === 'running' || window.xb.status === 'loading');
    const xb = await page.evaluate(() => window.xb);
    const reached = Object.keys(xb.milestones);
    console.log(`--- boot: status "${xb.status}", ${xb.lines} log lines, ${Math.round((Date.now() - t0) / 1000)} s wall`);
    if (xb.error) console.log(`    error: ${xb.error.split('\n')[0]}`);
    for (const ms of Object.values(xb.milestones)) {
      const s = ms.stamp ? `engine ${ms.stamp.ms} ms, ${ms.stamp.insn} insn` : 'no stamp';
      console.log(`    milestone ${ms.label}: wall ${(ms.wall / 1000).toFixed(2)} s, ${s}`);
    }
    if (xb.fetch) console.log(`    fetch: ${JSON.stringify(xb.fetch)}`);
    const st = xb.stats ? xb.stats.store : null;
    if (st) console.log(`    packs: ${st.packs} (${st.packDownloads} downloaded, ${st.packChunks} chunks), sync fetches: ${st.syncFetches}`);
    check(xb.milestones.phase100 !== undefined, 'boot: reaches "Starting phase 100"');
    check(st !== null && st.packChunks > 0, 'boot: the chunk store served the boot from its boot packs');
    for (const id of BOOT_REQUIRED) {
      check(reached.includes(id), `boot: milestone ${id} reached`);
    }
    check(streaming, 'boot: logcat streams while the engine runs (milestone arrived before the engine stopped)');
    // The boot screen (W2): stage 4 ("Starting services", 4 / 5) is the one phase 100 starts.
    const scr = xb.screen;
    console.log(`    screen: ${JSON.stringify(scr && { state: scr.state, label: scr.label, count: scr.count, detail: scr.detail, fill: scr.fill })}`);
    check(scr && scr.stage === 3 && scr.label === 'Starting services' && scr.count === '4 / 5',
      'boot screen: reaches stage 4, "Starting services", 4 / 5');
    check(scr && scr.stageLog.map((e) => e.index).join(',') === '0,1,2,3', 'boot screen: stages entered in order (1 to 4)');
    check(scr && scr.fill >= 20 && scr.fill < 90, `boot screen: the bar is partway through the boot at stage 4 (${scr && scr.fill}%)`);
    const bar = await page.evaluate(() => ({
      role: document.getElementById('bar').getAttribute('role'),
      now: Number(document.getElementById('bar').getAttribute('aria-valuenow')),
      label: document.getElementById('stage-label').textContent,
    }));
    check(bar.role === 'progressbar' && Number.isFinite(bar.now), `boot screen: progressbar with aria-valuenow (${bar.now})`);
    check(bar.label === 'Starting services', `boot screen: the live stage label reads "${bar.label}"`);
    await page.screenshot({ path: join(SHOTS, 'boot-stage4.png') });
    console.log(`    screenshot: ${join(SHOTS, 'boot-stage4.png')}`);
    check(pageErrors.length === 0, `boot: no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
    // Stop the worker (the page's Stop button), so the run does not hold the browser.
    await page.click('#stop');
    await page.waitForFunction(() => window.xb.status !== 'running' && window.xb.status !== 'loading', null, {
      timeout: 30000,
      polling: 100,
    });
    // A warm visit (the same profile): the packs are in the chunk cache, so the download reads "Loading from cache"
    // and no pack is downloaded.
    await page.reload({ waitUntil: 'load' });
    await page.click('#boot');
    await page.waitForFunction(() => window.xb.screen && window.xb.screen.label === 'Loading from cache', null, {
      timeout: 120000,
      polling: 50,
    }).catch(() => {});
    const warm = await page.evaluate(() => ({ screen: window.xb.screen, fetch: window.xb.fetch }));
    check(warm.screen !== null && warm.screen.label === 'Loading from cache' && warm.screen.fromCache === true,
      'boot screen, warm visit: "Loading from cache"');
    check(!warm.fetch || warm.fetch.requests === 0, 'boot screen, warm visit: no pack is downloaded');
    await page.click('#stop');
    await page.waitForFunction(() => window.xb.status === 'stopped', null, { timeout: 30000, polling: 100 });
  } finally {
    await browser.close();
  }
}

// The frame path without a guest (G2, opt-in: XIAOJI_WEB_FRAMES=1): a worker runs xj_gl_selftest (engine/gl_webgl.c),
// which draws on the WebGL2 color buffer (a clear, a scissored clear, a quad through the wire path, and an alias) and
// posts the frame. The page draws it on the boot page's own #android-screen canvas, over the phone frame, and reads the pixels
// back: the clear colour at (10, 10), the scissored colour inside its box and outside the quad (115, 800), the quad's
// green at the centre, and the alias result. Stop must still be the element at its centre, over the canvas.
async function runGlSelfTest(chromium, baseUrl) {
  mkdirSync(SHOTS, { recursive: true });
  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  const page = await browser.newPage({ viewport: VIEW });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  try {
    await page.goto(baseUrl, { waitUntil: 'load' });
    const got = await page.evaluate(async () => {
      const base = new URL('./', location.href).href;
      const { newFrameView } = await import(base + 'frame.js');
      const canvas = document.getElementById('android-screen');
      // As main.js does on the first frame: the canvas is see-through until then (the boot animation's outro, W3).
      document.getElementById('phone').dataset.frame = '1';
      const sab = new SharedArrayBuffer(8);
      const view = newFrameView(canvas, new Int32Array(sab));
      return await new Promise((resolve) => {
        const w = new Worker(base + 'worker.js', { type: 'module' });
        const timer = setTimeout(() => resolve({ error: 'no frame and no done within 120 s' }), 120000);
        const px = (x, y) => Array.from(canvas.getContext('2d').getImageData(x, y, 1, 1).data.slice(0, 3));
        // The frame is posted before 'done' (the worker's selftest returns after it), so the result needs both.
        const got = {};
        const finish = () => {
          if (got.state === undefined || got.done === undefined) return;
          clearTimeout(timer);
          w.terminate();
          resolve(got);
        };
        w.onmessage = (ev) => {
          const m = ev.data;
          if (m.type === 'frame') {
            got.state = view.show(m, 0);
            got.clear = px(10, 10);
            got.boxed = px(115, 800);
            got.centre = px(270, 480);
            got.canvasVisible = !canvas.hidden;
            finish();
          } else if (m.type === 'done') {
            got.done = m.exitCode;
            got.alias = m.alias;
            got.quad = m.quad;
            got.life = m.life;
            got.ctxState = m.state;
            got.compose = m.compose;
            got.vao = m.vao;
            got.respec = m.respec;
            got.wirefloat = m.wirefloat;
            finish();
          } else if (m.type === 'error') {
            clearTimeout(timer);
            w.terminate();
            resolve({ error: m.text });
          }
        };
        w.postMessage({ type: 'glselftest', base, frameSab: sab });
      });
    });
    if (got.error) console.log(`    selftest: ${got.error.split('\n')[0]}`);
    // Stop is above the canvas: its centre is the button (the button is hidden outside a boot, so it is shown for the test).
    const stopOnTop = await page.evaluate(() => {
      const stop = document.getElementById('stop');
      const was = stop.hidden;
      stop.hidden = false;
      const r = stop.getBoundingClientRect();
      const el = document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2);
      stop.hidden = was;
      return !!el && (el === stop || stop.contains(el));
    });
    const st = got.state;
    check(got.done === 1, `selftest: the engine reports one frame posted (${got.done})`);
    check(st && st.frames === 1, 'selftest: one frame arrives from rcFBPost (WebGL2 backend, no guest)');
    check(st && st.w === 540 && st.h === 960, `selftest: the frame is the display size (${st ? st.w + 'x' + st.h : '-'})`);
    check(got.canvasVisible === true, 'selftest: the frame is shown on the boot page\'s #android-screen canvas');
    const near = (a, b) => a && a.every((v, i) => Math.abs(v - b[i]) <= 2);
    check(near(got.clear, [26, 128, 230]), `selftest: the clear colour is at (10, 10) of the top-first frame (${got.clear})`);
    check(near(got.boxed, [255, 51, 51]), `selftest: the scissored colour is inside the box, outside the quad, read top-first (${got.boxed})`);
    check(near(got.centre, [0, 255, 0]), `selftest: the quad (shaders, client array, matrix uniform, draw) is green at the centre (${got.centre})`);
    const unpack = (v) => (v === undefined || v === null ? null : [(v >> 16) & 255, (v >> 8) & 255, v & 255]);
    check(near(unpack(got.quad), [0, 255, 0]), `selftest: the quad's pixel read back from the surface through the wire path (${unpack(got.quad)})`);
    check(near(unpack(got.alias), [230, 230, 26]), `selftest: a texture bound by name and attached to a guest FBO through rcBindTexture draws into the color buffer (${unpack(got.alias)})`);
    check(got.life === 1, 'selftest: color buffer references: open and close count, and the last close deletes');
    check(got.compose === 0x3f, `selftest: host composition (rcCompose) draws a CPU-written buffer top row first, a solid color blended over it, a flushed GL window surface reads top row first, and the GLSL version and EGL vendor are advertised (${got.compose})`);
    check(got.ctxState === 0x7ff, `selftest: each context's enables, blend func, unpack alignment, array buffer and renderbuffer binding are switched with it; a multisampled R8 and a stencil renderbuffer get storage, also under a name the guest made with glGenFramebuffers (${got.ctxState})`);
    check(got.vao === 0xf, `selftest: each context has its own vertex array object 0: a draw keeps its attributes after another context sets its own (also to no buffer), the guest reads binding 0, and the enables stay per context (${got.vao})`);
    check(got.respec === 0xf, `selftest: re-specifying a texture that stands for a colour buffer (glTexImage2D after rcBindTexture) gives the name its own storage and leaves the colour buffer's size and picture; a glTexImage2D without data reads as zeros (${got.respec})`);
    check(near(unpack(got.wirefloat), [64, 128, 191]), `selftest: float arguments decoded from the wire keep their values (glClearColor 0.25, 0.5, 0.75 sent as a packet clears to ${unpack(got.wirefloat)})`);
    check(stopOnTop, 'selftest: Stop is still the element at its centre, over the Android canvas');
    check(pageErrors.length === 0, `selftest: no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
    await page.locator('#phone').screenshot({ path: join(SHOTS, 'glselftest.png') });
    console.log(`    screenshot: ${join(SHOTS, 'glselftest.png')}`);
  } finally {
    await browser.close();
  }
}

// The Android screen (G2, opt-in: XIAOJI_WEB_FRAMES=1): the boot runs with the WebGL2 backend (?gl=webgl, the page's
// default) and the first frame SurfaceFlinger composes must arrive (rcFBPost, engine/gl_webgl.c) as a picture that is
// not one colour. It waits up to XN_FRAME_TIMEOUT_MS (default 900000) for the frame, then saves the screen and stops.
async function runFrames(chromium, baseUrl) {
  mkdirSync(SHOTS, { recursive: true });
  const browser = await chromium.launch({ headless: true, ...(process.env.XN_CHROME ? { executablePath: process.env.XN_CHROME } : {}) });
  const page = await browser.newPage({ viewport: VIEW });
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  const FRAME_TIMEOUT_MS = Number(process.env.XN_FRAME_TIMEOUT_MS ?? 900000);
  try {
    await page.goto(baseUrl + '?boot=pack&gl=webgl', { waitUntil: 'load' });
    await page.click('#boot');
    const t0 = Date.now();
    await page.waitForFunction(() => window.xb.frames && window.xb.frames.frames > 0, null, {
      timeout: FRAME_TIMEOUT_MS,
      polling: 500,
    }).catch(() => {});
    // The first composed frames can be one colour (B18: with no first-boot dialog, the screen before the launcher draws
    // is a solid layer), so wait, within the same limit, for a frame that is not.
    const left = Math.max(0, FRAME_TIMEOUT_MS - (Date.now() - t0));
    await page.waitForFunction(() => window.xb.frames && window.xb.frames.distinct > 1, null, {
      timeout: left || 1,
      polling: 500,
    }).catch(() => {});
    const xb = await page.evaluate(() => ({ frames: window.xb.frames, status: window.xb.status, error: window.xb.error }));
    const f = xb.frames;
    console.log(`--- frames: ${f ? f.frames : 0} frame(s), first at ${f && f.firstMs !== null ? (f.firstMs / 1000).toFixed(1) : '-'} s after the press, ` +
      `${Math.round((Date.now() - t0) / 1000)} s wait, status "${xb.status}"`);
    if (xb.error) console.log(`    error: ${xb.error.split('\n')[0]}`);
    check(f && f.frames > 0, 'frames: at least one frame arrives (rcFBPost, WebGL2 backend)');
    check(f && f.distinct > 1, `frames: a frame that is not one colour arrives (${f ? f.distinct : 0} distinct colours in a sample)`);
    check(f && f.w === 540 && f.h === 960, `frames: the frame is the display size (${f ? f.w + 'x' + f.h : '-'})`);
    check(pageErrors.length === 0, `frames: no page errors${pageErrors.length ? ' (' + pageErrors[0] + ')' : ''}`);
    await page.waitForTimeout(500);
    await page.locator('#phone').screenshot({ path: join(SHOTS, 'android-frame.png') });
    console.log(`    screenshot: ${join(SHOTS, 'android-frame.png')}`);
    await page.click('#stop').catch(() => {});
    await page.waitForFunction(() => window.xb.status !== 'running' && window.xb.status !== 'loading', null, {
      timeout: 60000,
      polling: 200,
    }).catch(() => {});
  } finally {
    await browser.close();
  }
}

async function main() {
  const argv = process.argv.slice(2);
  const bootOnly = argv.includes('--boot-only');
  const bootToo = process.env.XIAOJI_WEB_BOOT === '1';
  const names = argv.filter((a) => !a.startsWith('--')).length
    ? argv.filter((a) => !a.startsWith('--'))
    : bootOnly ? [] : ['hello-jit'];
  for (const n of names) {
    if (!CASES[n]) {
      console.error(`unknown case ${n}; known: ${Object.keys(CASES).join(' ')}`);
      process.exit(2);
    }
  }
  const needed = ['build/wasm/xiaoji.mjs'];
  if (names.length) needed.push('build/web/manifest.json', 'build/rootfs/data/hello.dex');
  const frames = process.env.XIAOJI_WEB_FRAMES === '1';
  if (bootOnly || bootToo || frames) needed.push('build/web/manifest-xiaoji.json', 'build/web/xj/manifest-xiaoji-v2.json', 'build/rootfs-xiaoji/system/bin/init');
  for (const f of needed) {
    if (!existsSync(join(ROOT, f))) {
      console.error(`missing ${f}; run make test-web (it builds the prerequisites)`);
      process.exit(2);
    }
  }
  checkEnvMatchesScript();

  const { chromium } = await import(pathToFileURL(PW).href);
  const srv = await startServer({ port: 0, log: () => {} });
  try {
    for (const n of names) {
      await runCase(chromium, srv.url, n);
    }
    if (bootOnly || bootToo) {
      await runDownloadShot(chromium);
      await runBoot(chromium, srv.url);
    }
    if (process.env.XIAOJI_WEB_FRAMES === '1') {
      await runGlSelfTest(chromium, srv.url);
      await runFrames(chromium, srv.url);
    }
  } finally {
    await srv.close();
  }
  console.log(failures ? `\n${failures} check(s) failed` : '\nall web checks passed');
  process.exit(failures ? 1 : 0);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
