// Booted snapshots, the page's side (docs/snapshot.md), in Node (no browser, no boot):
// - the boot screen's restore phase (web/bootscreen.js): its label, its bar from the snapshot's download, the arc turning,
//   and the outro on the first frame;
// - tools/snappack.py on a small synthetic snapshot: every extent and the head come back byte for byte from the packs
//   (as web/worker.js reads them), equal chunks are stored once, and the manifest carries the build and the tree's hash.
// tools/snapshot-web.mjs (make test-snapshot-web) checks a real restore in the page.
import { execFileSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { inflateSync } from 'node:zlib';

import { newScreen, startRun, onRestore, onFrame, render, animPhase } from '../web/bootscreen.js';
import { DEFAULT_THEME } from '../web/theme.js';

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..');
let failures = 0;
function check(ok, what) {
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${what}`);
  if (!ok) failures++;
}

// ---- the boot screen ----
{
  const s = newScreen(DEFAULT_THEME);
  startRun(s);
  onRestore(s, { loaded: 0, total: 100 * 1048576 });
  let v = render(s, 1000);
  check(s.phase === 'restore' && v.label === 'Restoring Android', `screen: a restore shows "Restoring Android" (${v.label})`);
  onRestore(s, { loaded: 50 * 1048576, total: 100 * 1048576 });
  v = render(s, 2000);
  check(Math.round(v.fill) === 50 && v.count === '50%' && /50\.0 of 100\.0 MB/.test(v.detail),
    `screen: the bar is the snapshot's download (${v.fill}, ${v.count}, ${v.detail})`);
  onRestore(s, { loaded: 100 * 1048576, total: 100 * 1048576 });
  v = render(s, 3000);
  check(v.detail === 'Starting from the home screen', `screen: when the download is in, it says what comes next (${v.detail})`);
  check(animPhase(s, 60000, false) === 'loop', 'screen: the arc turns while restoring');
  onFrame(s, 61000);
  check(animPhase(s, 61000, false) === 'outro', 'screen: the first frame plays the outro');
  const t = newScreen(DEFAULT_THEME);
  onRestore(t, { loaded: 1, total: 2 });
  check(t.phase === 'idle', 'screen: a restore message before a run changes nothing');
}

// ---- tools/snappack.py ----
{
  const dir = mkdtempSync(join(tmpdir(), 'xiaoji-snappack-'));
  try {
    // A version 1 file as engine/snap.c writes it: the header, a head section (the trees), the extent table, the data.
    const hdr = Buffer.alloc(4096);
    hdr.write('XJSNAP\0\x01', 0, 'latin1');
    hdr.writeUInt32LE(1, 8);
    hdr.write('web-test-build', 16);
    hdr.writeBigUInt64LE(77n, 96); // icount
    hdr.writeBigUInt64LE(1n, 104); // nranges
    hdr.writeBigUInt64LE(4096n, 112);
    hdr.writeBigUInt64LE(4096n + 600000n, 120);
    const tree = Buffer.from('the writable trees');
    const rand = (n, seed) => {
      const b = Buffer.alloc(n);
      let x = seed;
      for (let i = 0; i < n; i++) {
        x = (x * 1103515245 + 12345) >>> 0;
        b[i] = x >>> 24;
      }
      return b;
    };
    // Two extents; the second repeats a chunk of the first (equal chunks are stored once).
    const e1 = Buffer.concat([rand(262144, 1), rand(10000, 2)]);
    const e2 = Buffer.concat([rand(262144, 1), Buffer.alloc(5000, 8)]);
    const exts = [[8192, e1], [8192 + 300000, e2]];
    const extOff = (4096 + tree.length + 15) & ~15;
    const table = Buffer.alloc(16 * exts.length);
    exts.forEach(([a, d], k) => {
      table.writeBigUInt64LE(BigInt(a), 16 * k);
      table.writeBigUInt64LE(BigInt(d.length), 16 * k + 8);
    });
    const dataOff = Math.ceil((extOff + table.length) / 4096) * 4096;
    hdr.writeBigUInt64LE(BigInt(exts.length), 240);
    hdr.writeBigUInt64LE(BigInt(extOff), 248);
    hdr.writeBigUInt64LE(BigInt(dataOff), 256);
    hdr.writeBigUInt64LE(BigInt(e1.length + e2.length), 264);
    const file = Buffer.alloc(dataOff + e1.length + e2.length);
    hdr.copy(file, 0);
    tree.copy(file, 4096);
    table.copy(file, extOff);
    e1.copy(file, dataOff);
    e2.copy(file, dataOff + e1.length);
    writeFileSync(join(dir, 'test.snap'), file);
    const treeManifest = Buffer.from('{"tree":1}');
    writeFileSync(join(dir, 'tree.json'), treeManifest);
    execFileSync('python3', ['-I', join(ROOT, 'tools', 'snappack.py'), join(dir, 'test.snap'), join(dir, 'xj'), '--tree', join(dir, 'tree.json')],
      { stdio: 'pipe' });
    const m = JSON.parse(readFileSync(join(dir, 'xj', 'snapshot.json'), 'utf8'));
    check(m.build === 'web-test-build' && m.tree === createHash('sha256').update(treeManifest).digest('hex'),
      'snappack: the manifest names the build and the tree manifest\'s SHA-256');
    const raw = new Map();
    for (const p of m.packs) {
      const buf = readFileSync(join(dir, 'xj', p.file));
      for (const [id, off, zlen, rlen] of p.chunks) {
        const c = inflateSync(buf.subarray(off, off + zlen));
        if (c.length === rlen && createHash('sha256').update(c).digest('hex').slice(0, 32) === id) raw.set(id, c);
      }
    }
    const back = (ids) => Buffer.concat(ids.map((id) => raw.get(id) || Buffer.alloc(0)));
    check(m.extents.length === 2 && back(m.extents[0][2]).equals(e1) && back(m.extents[1][2]).equals(e2) &&
      m.extents[0][0] === 8192 && m.extents[1][0] === 8192 + 300000, 'snappack: every extent comes back byte for byte at its address');
    check(back(m.head.chunks).equals(file.subarray(0, extOff)), 'snappack: the head (header and trees) comes back byte for byte');
    check(raw.size === 4 && m.extents[0][2][0] === m.extents[1][2][0], `snappack: equal chunks are stored once (${raw.size} distinct)`);
    check(m.ranges.length === 1 && m.ranges[0][0] === 4096, 'snappack: the image\'s ranges are in the manifest');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

// ---- the chunk store's synchronous inflater (web/inflate.js), which a restore's file reads go through ----
// Against Node's zlib: stored, fixed and dynamic blocks, overlapping copies, codes longer than the lookup table, and
// the real chunks of the tree's store when it is built (build/web/xj/chunks).
{
  const { inflateZlib } = await import('../web/inflate.js');
  const { deflateSync, constants } = await import('node:zlib');
  const rnd = (n, seed) => {
    const b = Buffer.alloc(n);
    let x = seed;
    for (let i = 0; i < n; i++) {
      x = (x * 1103515245 + 12345) >>> 0;
      b[i] = x >>> 24;
    }
    return b;
  };
  // Skewed bytes: a few symbols are very frequent and many are rare, so the rare ones get codes of 11 to 15 bits.
  const skew = (n, seed) => {
    const b = rnd(n, seed);
    for (let i = 0; i < n; i++) b[i] = b[i] < 250 ? b[i] & 3 : b[i];
    return b;
  };
  const cases = [
    ['empty', Buffer.alloc(0), {}],
    ['stored (level 0)', rnd(70000, 3), { level: 0 }],
    ['fixed codes (a short text)', Buffer.from('hello hello hello, xiaoji'), { strategy: constants.Z_FIXED }],
    ['dynamic codes', Buffer.from('the quick brown fox '.repeat(5000)), {}],
    ['overlapping copies (a run)', Buffer.alloc(100000, 7), {}],
    ['random bytes', rnd(131072, 5), {}],
    ['long codes (skewed bytes)', skew(131072, 9), { level: 9 }],
  ];
  let bad = [];
  for (const [what, raw, o] of cases) {
    try {
      const out = inflateZlib(new Uint8Array(deflateSync(raw, o)), raw.length);
      if (!Buffer.from(out).equals(raw)) bad.push(what);
    } catch (e) {
      bad.push(`${what}: ${e.message}`);
    }
  }
  check(!bad.length, `inflate: ${cases.length} kinds of stream come back byte for byte${bad.length ? ' (' + bad.join('; ') + ')' : ''}`);
  let threw = false;
  try {
    inflateZlib(new Uint8Array([0x78, 0x9c, 0xff, 0xff]), 10);
  } catch {
    threw = true;
  }
  check(threw, 'inflate: a broken stream is an error');
  const { readdirSync, existsSync } = await import('node:fs');
  const cdir = join(ROOT, 'build', 'web', 'xj', 'chunks');
  if (existsSync(cdir)) {
    const names = readdirSync(cdir).filter((n, i) => n.endsWith('.z') && (process.env.XIAOJI_INFLATE_ALL || i % 8 === 0)); // every 8th (XIAOJI_INFLATE_ALL=1: all)
    let n = 0, wrong = 0, bytes = 0;
    const t0 = performance.now();
    for (const name of names) {
      const z = readFileSync(join(cdir, name));
      const raw = inflateSync(z);
      const out = inflateZlib(new Uint8Array(z), raw.length);
      if (!Buffer.from(out).equals(raw)) wrong++;
      n++;
      bytes += raw.length;
    }
    const ms = performance.now() - t0;
    check(wrong === 0, `inflate: the tree's ${n} chunks (${(bytes / 1048576).toFixed(0)} MiB) come back byte for byte (${wrong} differ; ` +
      `${ms.toFixed(0)} ms with Node's own inflate of each)`);
  } else {
    console.log('note: no build/web/xj/chunks: the tree\'s chunks are not checked');
  }
}

// ---- the restore's prefetch: tools/prefetch.py and the chunk store's prefetched chunks (web/chunks.js) ----
{
  const dir = mkdtempSync(join(tmpdir(), 'xiaoji-prefetch-'));
  try {
    const { mkdirSync } = await import('node:fs');
    const { deflateSync } = await import('node:zlib');
    const { ChunkStore, layoutPacks, PACK_READY } = await import('../web/chunks.js');
    const xj = join(dir, 'xj');
    mkdirSync(join(xj, 'chunks'), { recursive: true });
    mkdirSync(join(xj, 'packs'), { recursive: true });
    const cs = 1024;
    const files = { '/a': Buffer.alloc(2500, 1), '/b': Buffer.alloc(1500, 2) };
    files['/a'].write('first', 0);
    files['/b'].write('second', 1100);
    const entries = [];
    const ids = new Map();
    for (const [path, data] of Object.entries(files)) {
      const chunks = [];
      for (let o = 0; o < data.length; o += cs) {
        const raw = data.subarray(o, o + cs);
        const id = createHash('sha256').update(raw).digest('hex').slice(0, 32);
        writeFileSync(join(xj, 'chunks', id + '.z'), deflateSync(raw));
        ids.set(id, raw);
        chunks.push(id);
      }
      entries.push({ path, type: 'file', mode: 0o644, size: data.length, chunks });
    }
    writeFileSync(join(xj, 'manifest-xiaoji-v2.json'), JSON.stringify({ chunk_size: cs, entries, packs: [] }));
    writeFileSync(join(xj, 'snapshot.json'), JSON.stringify({ packs: [] }));
    // A server log as tools/latency.mjs --fetch-log writes it: b's second chunk, then a's first, then b's again.
    const [a0] = entries[0].chunks, [, b1] = entries[1].chunks;
    writeFileSync(join(dir, 'fetch.log'), [`1 200 GET /xj/chunks/${b1}.z 100`, '2 200 GET /main.js 10',
      `3 200 GET /xj/chunks/${a0}.z 100`, `4 200 GET /xj/chunks/${b1}.z 100`].join('\n') + '\n');
    const py = (...a) => execFileSync('python3', ['-I', join(ROOT, 'tools', 'prefetch.py'), ...a], { encoding: 'utf8' });
    py('record', join(dir, 'fetch.log'), '--tree', join(xj, 'manifest-xiaoji-v2.json'), '--out', join(dir, 'order.json'));
    const order = JSON.parse(readFileSync(join(dir, 'order.json'), 'utf8'));
    check(JSON.stringify(order) === JSON.stringify([['/b', 1], ['/a', 0]]),
      `prefetch: the order is the log's first reads, as [path, chunk] (${JSON.stringify(order)})`);
    py('pack', join(dir, 'order.json'), '--xj', xj, '--pack-max', '20');
    const m = JSON.parse(readFileSync(join(xj, 'snapshot.json'), 'utf8'));
    const pf = m.prefetch;
    check(pf && pf.packs.length === 2 && pf.packs[0].chunks[0][0] === b1 && pf.packs[1].chunks[0][0] === a0 &&
      pf.raw_bytes === 476 + 1024, `prefetch: the snapshot manifest lists the packs in that order (${pf ? pf.packs.length : 0} packs)`);
    // The chunk store: a chunk of a ready pack comes from the shared buffer; one of a pack not started is read as before.
    const { total } = layoutPacks(pf.packs);
    const sab = new SharedArrayBuffer(total);
    const ctrl = new Int32Array(new SharedArrayBuffer(4 * pf.packs.length));
    const store = new ChunkStore('http://127.0.0.1:1/xj/', cs);
    store.attachPrefetch(pf.packs, sab, ctrl);
    const { where } = layoutPacks(pf.packs);
    new Uint8Array(sab).set(ids.get(b1), where.get(b1).off);
    Atomics.store(ctrl, 0, PACK_READY);
    const got = store.get(b1, 476);
    check(Buffer.from(got).equals(ids.get(b1)) && store.stats.prefetchHits === 1, 'prefetch: a chunk of a ready pack comes from the prefetch');
    let fetched = false;
    try {
      store.get(a0, 1024); // its pack is not in: the store makes its own (here failing) request
    } catch {
      fetched = true;
    }
    check(fetched && store.stats.prefetchMisses === 1, 'prefetch: a chunk of a pack not in yet is read by the store itself');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

process.exit(failures ? 1 : 0);
