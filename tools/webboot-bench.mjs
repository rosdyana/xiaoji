// Measures one Boot Android run in headless Chromium (docs/loading.md). Starts web/serve.mjs, opens the
// page with a fresh profile (or the profile in --profile, for a warm run), clicks Boot Android, waits
// for "Starting phase 100", stops the worker and writes the numbers as JSON.
//
//   node tools/webboot-bench.mjs --out FILE [--trace] [--query '?boot=pack'] [--profile DIR] [--port N]
//                                [--throttle-mbps N --rtt-ms N] [--timeout-ms N]
//
// --port N fixes the port: Cache Storage is per origin, so a warm run (--profile of an earlier run) needs it.
//
// --trace asks the page for the access trace (web/trace.js); it is slower, so the timings come from
// runs without it. Playwright comes from XN_PLAYWRIGHT, as in tests/web.mjs.

import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

import { startServer } from '../web/serve.mjs';

const PW =
  process.env.XN_PLAYWRIGHT ??
  '/Users/rosdyanakusuma/Ross/projects/xiaonux/bench/node_modules/playwright-core/index.mjs';

function args(argv) {
  const o = { out: null, trace: false, query: '', profile: null, timeoutMs: 900000, throttleMbps: 0, rttMs: 0, port: 0, coop: false };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--out') o.out = argv[++i];
    else if (a === '--trace') o.trace = true;
    else if (a === '--coop') o.coop = true; // COOP/COEP on the server (SharedArrayBuffer, docs/loading.md L2)
    else if (a === '--query') o.query = argv[++i];
    else if (a === '--profile') o.profile = argv[++i];
    else if (a === '--port') o.port = Number(argv[++i]);
    else if (a === '--timeout-ms') o.timeoutMs = Number(argv[++i]);
    else if (a === '--throttle-mbps') o.throttleMbps = Number(argv[++i]);
    else if (a === '--rtt-ms') o.rttMs = Number(argv[++i]);
    else throw new Error(`unknown option ${a}`);
  }
  if (!o.out) throw new Error('--out FILE is required');
  return o;
}

async function main() {
  const o = args(process.argv.slice(2));
  const { chromium } = await import(pathToFileURL(PW).href);
  // Every response the server sends is tallied from its log line (web/serve.mjs): bytes of GET bodies by
  // kind (chunks, packs, other), GET and HEAD requests. Cache Storage hits never reach the server.
  const wire = { bytes: 0, gets: 0, heads: 0, byKind: { chunks: 0, packs: 0, other: 0 } };
  const log = (line) => {
    const m = /^(\d{3}) (GET|HEAD) (\S+) (.*)$/.exec(line);
    if (!m || m[1][0] !== '2') return;
    if (m[2] === 'HEAD') {
      wire.heads++;
      return;
    }
    const n = m[1] === '206' ? Number(m[4].split('-')[1]) - Number(m[4].split('-')[0]) + 1 : Number(m[4]);
    const kind = m[3].startsWith('/xj/chunks/') ? 'chunks' : m[3].startsWith('/xj/packs/') ? 'packs' : 'other';
    if (process.env.XN_LOG_GETS) console.log(`    ${line}`);
    wire.gets++;
    wire.bytes += n;
    wire.byKind[kind] += n;
  };
  const srv = await startServer({ port: o.port, coop: o.coop, log, throttle: { mbps: o.throttleMbps, rttMs: o.rttMs } });
  const profile = o.profile ?? mkdtempSync(join(tmpdir(), 'xj-bench-'));
  const t0 = Date.now();
  // XN_CHROME: a Chromium executable to use instead of Playwright's bundled one (for a machine without the bundle).
  const launch = { headless: true };
  if (process.env.XN_CHROME) launch.executablePath = process.env.XN_CHROME;
  const browser = await chromium.launchPersistentContext(profile, launch);
  try {
    const page = await browser.newPage();
    const pageErrors = [];
    page.on('pageerror', (e) => pageErrors.push(String(e)));
    await page.goto(srv.url + (o.query || (o.trace ? '?trace' : '')), { waitUntil: 'load' });
    await page.click('#boot');
    await page.waitForFunction(
      () => window.xb && (window.xb.milestones.phase100 || ['done', 'error', 'stopped'].includes(window.xb.status)),
      null,
      { timeout: o.timeoutMs, polling: 250 },
    );
    // The stats and the trace are posted from the worker at phase 100; give them a moment to arrive.
    await page.waitForFunction((wantTrace) => (window.xb.stats !== undefined && (!wantTrace || window.xb.trace !== undefined))
      || window.xb.status === 'done', o.trace, { timeout: 60000, polling: 250 }).catch(() => {});
    const xb = await page.evaluate(() => window.xb);
    // The boot runs on after phase 100 until Stop. The numbers are taken here, at phase 100, like the stats.
    const wireAt100 = JSON.parse(JSON.stringify(wire));
    await page.click('#stop').catch(() => {});
    const result = {
      query: o.query || (o.trace ? '?trace' : ''),
      throttle: { mbps: o.throttleMbps, rttMs: o.rttMs },
      coop: o.coop,
      status: xb.status,
      pageErrors,
      harnessWallS: (Date.now() - t0) / 1000,
      milestones: xb.milestones,
      fetch: xb.fetch,
      stats: xb.stats ?? null,
      wire: wireAt100,
      trace: xb.trace ?? null,
      statsAt: xb.statsAt ?? null,
    };
    writeFileSync(o.out, JSON.stringify(result));
    const p100 = xb.milestones.phase100;
    console.log(
      `status ${xb.status}; phase100 wall ${p100 ? (p100.wall / 1000).toFixed(2) : 'n/a'} s, ` +
        `engine ${p100 && p100.stamp ? (p100.stamp.ms / 1000).toFixed(2) : 'n/a'} s; ` +
        `wire ${JSON.stringify(wireAt100)}; stats ${xb.stats ? JSON.stringify({ sync: xb.stats.sync, store: xb.stats.store }) : 'n/a'}; -> ${o.out}`,
    );
  } finally {
    await browser.close();
    await srv.close();
  }
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
