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
    const spin = await page.evaluate(() => getComputedStyle(document.querySelector('.arc')).animationName);
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
  if (bootOnly || bootToo) needed.push('build/web/manifest-xiaoji.json', 'build/web/xj/manifest-xiaoji-v2.json', 'build/rootfs-xiaoji/system/bin/init');
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
