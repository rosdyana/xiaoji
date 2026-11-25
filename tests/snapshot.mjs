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

// ---- lazy snapshot pages (docs/snapshot.md, S5): tools/snappack.py's split, web/lazypages.js, web/snaploader.js ----
{
  const dir = mkdtempSync(join(tmpdir(), 'xiaoji-lazy-'));
  const { mkdirSync } = await import('node:fs');
  try {
    const PG = 4096;
    const rnd = (n, seed) => {
      const b = Buffer.alloc(n);
      let x = seed;
      for (let i = 0; i < n; i++) {
        x = (x * 1103515245 + 12345) >>> 0;
        b[i] = x >>> 24;
      }
      return b;
    };
    // A version 2 file: the header, the trees, the extent table, the region table (a pool slab, the WebGL blob), data.
    const POOL = 0x100000, BLOB = 0x40000, ENG = 0x10000;
    const e1 = rnd(3 * PG, 11); // engine memory
    const e2 = Buffer.concat(Array.from({ length: 40 }, (_, i) => rnd(PG, 100 + i))); // 40 pool pages
    const e3 = rnd(2 * PG, 12); // the blob
    const exts = [[ENG, e1], [BLOB, e3], [POOL, e2]].sort((a, b) => a[0] - b[0]);
    const regions = [[POOL, 64 * PG, 1], [BLOB, 2 * PG, 2]];
    const hdr = Buffer.alloc(4096);
    hdr.write('XJSNAP\0\x01', 0, 'latin1');
    hdr.writeUInt32LE(2, 8);
    hdr.write('web-test-build', 16);
    hdr.writeBigUInt64LE(1n, 104);
    hdr.writeBigUInt64LE(4096n, 112);
    hdr.writeBigUInt64LE(BigInt(POOL + 64 * PG), 120);
    const tree = Buffer.from('trees');
    const extOff = (4096 + tree.length + 15) & ~15;
    const regOff = extOff + 16 * exts.length;
    const dataOff = Math.ceil((regOff + 24 * regions.length) / 4096) * 4096;
    const table = Buffer.alloc(16 * exts.length);
    exts.forEach(([a, d], k) => {
      table.writeBigUInt64LE(BigInt(a), 16 * k);
      table.writeBigUInt64LE(BigInt(d.length), 16 * k + 8);
    });
    const rt = Buffer.alloc(24 * regions.length);
    regions.forEach(([a, l, kind], k) => {
      rt.writeBigUInt64LE(BigInt(a), 24 * k);
      rt.writeBigUInt64LE(BigInt(l), 24 * k + 8);
      rt.writeBigUInt64LE(BigInt(kind), 24 * k + 16);
    });
    const total = exts.reduce((n, [, d]) => n + d.length, 0);
    hdr.writeBigUInt64LE(BigInt(exts.length), 240);
    hdr.writeBigUInt64LE(BigInt(extOff), 248);
    hdr.writeBigUInt64LE(BigInt(dataOff), 256);
    hdr.writeBigUInt64LE(BigInt(total), 264);
    hdr.writeBigUInt64LE(BigInt(regions.length), 1384);
    hdr.writeBigUInt64LE(BigInt(regOff), 1392);
    const parts = [hdr, Buffer.alloc(extOff - 4096)];
    tree.copy(parts[1], 0);
    const file = Buffer.alloc(dataOff + total);
    Buffer.concat(parts).copy(file, 0);
    table.copy(file, extOff);
    rt.copy(file, regOff);
    let at = dataOff;
    for (const [, d] of exts) {
      d.copy(file, at);
      at += d.length;
    }
    writeFileSync(join(dir, 'test.snap'), file);
    writeFileSync(join(dir, 'tree.json'), '{"tree":2}');
    // The recorded order: page 5 before the first frame (hot), then pages 7 and 3 (first in the lazy order).
    const pg = (i) => POOL + i * PG;
    writeFileSync(join(dir, 'touch.json'), JSON.stringify({ first_frame_ms: 20, pages: [[pg(5), 10], [pg(7), 50], [pg(3), 60], [pg(7), 70]] }));
    const xj = join(dir, 'xj');
    execFileSync('python3', ['-I', join(ROOT, 'tools', 'snappack.py'), join(dir, 'test.snap'), xj, '--tree', join(dir, 'tree.json'),
      '--order', join(dir, 'touch.json'), '--lazy-pages', '4', '--lazy-pack-max', '20000'], { stdio: 'pipe' });
    const m = JSON.parse(readFileSync(join(xj, 'snapshot.json'), 'utf8'));
    const lz = m.lazy;
    check(m.version === 2 && lz && lz.pages === 39 && lz.touched === 2 && lz.packs.length > 1,
      `snappack: the pool pages are lazy but the hot one (${lz ? lz.pages : 0} lazy, ${lz ? lz.touched : 0} touched, ${lz ? lz.packs.length : 0} packs)`);
    const pages = new Uint32Array(new Uint8Array(inflateSync(readFileSync(join(xj, lz.map.file)))).buffer);
    const expectOrder = [7, 3, ...Array.from({ length: 40 }, (_, i) => i).filter((i) => ![3, 5, 7].includes(i))].map((i) => pg(i) / PG);
    check(pages.length === 39 && pages.every((p, i) => p === expectOrder[i]),
      'snappack: the lazy order is the recorded first touches, then the rest in address order');
    const raw = new Map();
    const hotBytes = [];
    for (const p of m.packs) {
      const buf = readFileSync(join(xj, p.file));
      for (const [id, off, zlen] of p.chunks) raw.set(id, inflateSync(buf.subarray(off, off + zlen)));
    }
    const mem = new Map();
    for (const [a, len, ids] of m.extents) {
      const b = Buffer.concat(ids.map((id) => raw.get(id)));
      hotBytes.push([a, len]);
      for (let o = 0; o < len; o++) mem.set(a + o, b[o]);
    }
    const hotHas = (a, d) => [...d].every((v, o) => mem.get(a + o) === v);
    check(hotHas(ENG, e1) && hotHas(BLOB, e3) && hotHas(pg(5), e2.subarray(5 * PG, 6 * PG)) &&
      !mem.has(pg(3)) && !mem.has(pg(7)) && !mem.has(pg(39)),
      'snappack: the hot extents hold the engine memory, the blob and the hot pool page, and no lazy page');
    check(m.classes.pool.raw === 40 * PG && m.classes.gl_blob.raw === 2 * PG && m.classes.engine.raw === 3 * PG,
      `snappack: the census by kind (${JSON.stringify(m.classes)})`);

    // The engine's side (web/lazypages.js), with a fake engine: a fault copies the page's chunk in.
    const { LazyPages, lazyBuffers, lazyChunks, HEAD, TAIL, SLOT0, DELIVERED } = await import('../web/lazypages.js');
    const fileOf = (url) => join(xj, url.slice('xj:/'.length));
    let failNext = 0, gets = 0;
    const get = (url, s, e) => {
      gets++;
      if (failNext > 0) {
        failNext--;
        throw new Error('HTTP 503');
      }
      return new Uint8Array(readFileSync(fileOf(url)).subarray(s, e));
    };
    const page = (i) => e2.subarray(i * PG, (i + 1) * PG);
    const BITS = 0x1f0000;
    const mk = (opts = {}) => {
      const Module = { HEAPU8: new Uint8Array(0x200000) };
      const lp = new LazyPages({ Module, lazy: lz, pages, base: 'xj:/', bufs: lazyBuffers(lz), get, retryMs: [1, 1, 1], ...opts });
      return { Module, lp };
    };
    {
      const { Module, lp } = mk();
      check(lp.lo() === pg(0) && lp.hi() === pg(40) && lp.mark(BITS) === 39, 'lazy: the range and the marked pages');
      const has = (i) => Buffer.from(Module.HEAPU8.subarray(pg(i), pg(i) + PG)).equals(page(i));
      const n = lp.fault(pg(3) + 100);
      check(n === 4 && has(7) && has(3) && has(0) && has(1) && !lp.pending(pg(3) / PG) && lp.pending(pg(2) / PG),
        `lazy: a fault on a page not yet loaded gets the right bytes, with its chunk's other pages (${n} written)`);
      // A page the engine freed before it arrived (snap_lazy_drop clears its bit) keeps the bytes it has now.
      const i2 = pg(2) - lp.lo();
      Module.HEAPU8[BITS + (i2 >>> 15)] &= ~(1 << ((i2 >>> 12) & 7));
      Module.HEAPU8.fill(0xee, pg(2), pg(2) + PG);
      lp.fault(pg(4));
      check(Module.HEAPU8[pg(2)] === 0xee && has(4) && has(6), 'lazy: a freed page is not overwritten when its chunk arrives');
      // The ring: the loader puts chunk 3, poll copies it in.
      const k = 3, ch = lazyChunks(lz)[k];
      const z = get('xj:/' + lz.packs[ch[0]].file, ch[1], ch[1] + ch[2]);
      const data = inflateSync(z);
      new Uint8Array(lp.bufs.ring, 0, data.length).set(data);
      Atomics.store(lp.bufs.ctrl, SLOT0, k);
      Atomics.store(lp.bufs.ctrl, HEAD, 1);
      const polled = lp.poll();
      const kp = Array.from(pages.subarray(k * 4, k * 4 + 4)).map((p) => (p * PG - POOL) / PG);
      check(polled === 4 && kp.every(has) && Atomics.load(lp.bufs.ctrl, TAIL) === 1 && Atomics.load(lp.bufs.chunkState, k) === DELIVERED,
        `lazy: poll copies in what the loader put in the ring (${polled} pages)`);
      failNext = 2;
      lp.fault(pg(39));
      check(has(39) && lp.stats.retries === 2, `lazy: a failed download is retried (${lp.stats.retries} retries)`);
      failNext = 10;
      let msg = '';
      try {
        lp.fault(pg(20));
      } catch (e) {
        msg = String(e.message);
      }
      failNext = 0;
      check(/could not be downloaded \(4 tries: HTTP 503\)/.test(msg) && lp.pending(pg(20) / PG),
        `lazy: a download that keeps failing ends with a clear error and the page still pending (${msg.slice(0, 90)})`);
      const fin = lp.finish();
      check(Array.from({ length: 40 }, (_, i) => i).filter((i) => i !== 2 && i !== 5).every(has) && fin > 0,
        'lazy: finish brings every pending page in');
    }

    // The background loader (web/snaploader.js) against the dev server: the packs arrive in the ring, a failed pack is
    // retried, and every lazy page comes in without a fault.
    const { DIRS, startServer } = await import('../web/serve.mjs');
    DIRS.xj = xj;
    const srv = await startServer({ port: 0 });
    const realFetch = globalThis.fetch;
    let failedOnce = 0;
    globalThis.fetch = async (url, init) => {
      if (String(url).endsWith(lz.packs[0].file) && failedOnce === 0) {
        failedOnce++;
        return new Response('busy', { status: 503 });
      }
      return realFetch(url, init);
    };
    const msgs = [];
    globalThis.self = { postMessage: (x) => msgs.push(x) };
    try {
      await import('../web/snaploader.js');
      const { Module, lp } = mk({ loader: { postMessage: (x) => globalThis.self.onmessage({ data: x }) } });
      globalThis.self.onmessage({ data: { type: 'start', base: srv.url + 'xj/', lazy: lz, prefetch: null, bufs: lp.bufs, prefetchBufs: null, hotIds: [] } });
      lp.mark(BITS);
      const t0 = Date.now();
      while (lp.stats.pagesIn < 39 && Date.now() - t0 < 20000) {
        await new Promise((r) => setTimeout(r, 5));
        lp.poll();
      }
      const all = Array.from({ length: 40 }, (_, i) => i).filter((i) => i !== 5)
        .every((i) => Buffer.from(Module.HEAPU8.subarray(pg(i), pg(i) + PG)).equals(page(i)));
      check(all && lp.stats.faults === 0 && failedOnce === 1,
        `lazy: the background loader brings every page in, retrying a failed pack (${lp.stats.pagesIn} pages, ${lp.stats.ringChunks} chunks)`);
      check(!msgs.some((x) => x.type === 'pack-error'), 'lazy: no pack failed for good');
    } finally {
      globalThis.fetch = realFetch;
      await srv.close();
    }
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
}

process.exit(failures ? 1 : 0);
