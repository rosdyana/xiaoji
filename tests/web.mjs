// Headless Chromium test for the web build (M4): starts web/serve.mjs, opens the page,
// runs ART cases in the worker and checks stdout and the exit code against the oracle
// (build/oracle/<case>.{out,rc}, the native run in the builder VM).
//
//   node tests/web.mjs [case ...]        (default: hello-jit)
//
// Playwright comes from XN_PLAYWRIGHT (the playwright-core ESM entry), defaulting to the
// copy in the xiaonux bench. Each case gets a new browser process, so each run has a fresh
// profile. Not part of make test; make test-web runs it.

import { execFileSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

import { startServer } from '../web/serve.mjs';
import { CASES, GUEST_ENV } from '../web/cases.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const PW =
  process.env.XN_PLAYWRIGHT ??
  '/Users/rosdyanakusuma/Ross/projects/xiaonux/bench/node_modules/playwright-core/index.mjs';
const TIMEOUT_MS = Number(process.env.XN_WEB_TIMEOUT_MS ?? 600000);

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

  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  const pageErrors = [];
  page.on('pageerror', (e) => pageErrors.push(String(e)));
  try {
    await page.goto(baseUrl, { waitUntil: 'load' });
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

async function main() {
  const names = process.argv.slice(2).length ? process.argv.slice(2) : ['hello-jit'];
  for (const n of names) {
    if (!CASES[n]) {
      console.error(`unknown case ${n}; known: ${Object.keys(CASES).join(' ')}`);
      process.exit(2);
    }
  }
  for (const f of ['build/wasm/xiaoji.mjs', 'build/web/manifest.json', 'build/rootfs/data/hello.dex']) {
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
