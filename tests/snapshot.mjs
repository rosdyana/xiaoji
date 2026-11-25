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

process.exit(failures ? 1 : 0);
